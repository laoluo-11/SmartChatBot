/* =========================================================================
 * wifi.c —— WiFi / NVS / SoftAP 配网模块的"具体实现"
 * -------------------------------------------------------------------------
 * 三块功能：
 *   1) NVS 读写账号密码（wifi_save_creds / wifi_has_saved_creds / load）
 *   2) STA 连接家里 WiFi（wifi_try_connect_saved，带断线自动重连）
 *   3) 配网模式（wifi_start_provisioning：SoftAP 热点 + 网页服务器收账号）
 *
 * 给初学者的小知识：
 *   - esp_wifi 管"射频连不连得上"；esp_netif 管"连上后怎么走 IP 网络"。
 *     两者都要初始化，顺序：netif_init -> event_loop -> 建接口 -> wifi_init。
 *   - WiFi 事件是异步的（连上/断开会触发回调），所以我们用事件处理函数
 *     wifi_event_handler 来"等结果"，而不是傻等。
 * ========================================================================= */

#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"        // esp_wifi_init / set_mode / set_config / start / connect
#include "esp_netif.h"       // TCP/IP 协议栈、IP_EVENT_STA_GOT_IP、ip_event_got_ip_t
#include "esp_event.h"       // 事件循环、事件处理器注册
#include "esp_http_server.h" // 配网用的轻量网页服务器
#include "nvs_flash.h"       // NVS 存储（掉电不丢）
#include <string.h>

static const char *TAG = "wifi";

/* ---- 一些模块内部状态 ---- */
static wifi_connected_cb_t s_connected_cb = NULL;   // "连上 WiFi"回调（可为空）
static httpd_handle_t      s_server = NULL;         // 配网页服务器句柄
static int                 s_retry_num = 0;         // STA 断线重连次数
#define WIFI_MAX_RETRY      5                        // 最多重连几次

/* 配网热点信息（ESP32 变成的这个 WiFi 的名字/参数） */
#define PROV_AP_SSID        "ESP32-Chatbot"   // 手机搜到的热点名
#define PROV_AP_CHANNEL      1                // 信道
#define PROV_AP_MAX_CONN     4                // 最多几台设备同时连

/* =========================================================================
 * NVS 相关：把账号密码存进"掉电不丢"的小存储
 * ========================================================================= */

/* 从 NVS 读出已存的账号密码（没存过则返回 false，ssid[0] 保持 '\0'）。 */
static bool wifi_load_creds(char *ssid, char *password)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;   // 命名空间都打不开，说明从没存过
    }
    size_t len = WIFI_MAX_SSID_LEN;
    esp_err_t r1 = nvs_get_str(h, "ssid", ssid, &len);
    len = WIFI_MAX_PASS_LEN;
    esp_err_t r2 = nvs_get_str(h, "password", password, &len);
    nvs_close(h);
    return (r1 == ESP_OK && r2 == ESP_OK);
}

bool wifi_has_saved_creds(void)
{
    char ssid[WIFI_MAX_SSID_LEN] = {0};
    char pass[WIFI_MAX_PASS_LEN] = {0};
    return wifi_load_creds(ssid, pass);
}

esp_err_t wifi_save_creds(const char *ssid, const char *password)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h));
    esp_err_t r = nvs_set_str(h, "ssid", ssid);
    if (r == ESP_OK) r = nvs_set_str(h, "password", password);
    if (r == ESP_OK) r = nvs_commit(h);   // 必须 commit 才会真正写进去
    nvs_close(h);
    return r;
}

/* =========================================================================
 * 事件处理：连上 / 断开 都是"异步事件"，在这里统一处理
 * ========================================================================= */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 断线了：在重试次数内就自动重连，超过就放弃。 */
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi 断开，重连中 (%d/%d)...", s_retry_num, WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "WiFi 重连 %d 次仍失败，请检查密码或重新配网", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* 真正连上了：拿到 IP 地址。 */
        s_retry_num = 0;
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "已连上 WiFi，IP = " IPSTR, IP2STR(&ev->ip_info.ip));
        if (s_connected_cb) s_connected_cb();   // 通知 main（切 OLED 等）
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "有设备连上了配网热点");
    }
}

/* =========================================================================
 * 初始化：做好一切准备，但"不真正连 WiFi"
 * ========================================================================= */
esp_err_t wifi_init(void)
{
    /* 1) NVS：先初始化；若分区损坏则擦掉重建。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2) 协议栈 + 事件循环 + 默认网络接口（STA 与 AP 各一个）。 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* 3) WiFi 驱动初始化 + 注册事件处理器（等 STA 断开、拿到 IP、AP 有人连）。 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "WiFi 子系统初始化完成");
    return ESP_OK;
}

void wifi_register_connected_cb(wifi_connected_cb_t cb)
{
    s_connected_cb = cb;
}

/* =========================================================================
 * STA 模式：用存档账号连家里 WiFi
 * ========================================================================= */
bool wifi_try_connect_saved(void)
{
    char ssid[WIFI_MAX_SSID_LEN] = {0};
    char pass[WIFI_MAX_PASS_LEN] = {0};
    if (!wifi_load_creds(ssid, pass)) {
        return false;   // NVS 里没账号
    }

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;  // 至少 WPA2（家用 WiFi 基本都够）

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "STA 已发起连接，目标 SSID=%s", ssid);
    return true;
}

/* 配网完成后：停掉热点和网页服务器，切回 STA 去连家里 WiFi。 */
static void provisioning_finish(void)
{
    if (s_server) {
        httpd_stop(s_server);   // 关掉配网页服务器
        s_server = NULL;
    }

    char ssid[WIFI_MAX_SSID_LEN] = {0};
    char pass[WIFI_MAX_PASS_LEN] = {0};
    wifi_load_creds(ssid, pass);   // 刚存进去的账号

    ESP_ERROR_CHECK(esp_wifi_stop());   // 先停，才能切换模式
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "配网完成，切换到 STA 连接家里 WiFi: %s", ssid);
}

/* =========================================================================
 * 配网网页服务器：GET 返回填写页面，POST 收账号并存 NVS
 * ========================================================================= */

/* 一个超简单的 HTML 表单页（手机浏览器打开就能填）。 */
static const char *PROV_HTML =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32 配网</title></head><body style=\"font-family:sans-serif;padding:16px\">"
    "<h2>ESP32 语音机器人 配网</h2>"
    "<form method=\"post\" action=\"/\">"
    "WiFi 名称(SSID):<br><input name=\"ssid\" style=\"width:100%;padding:8px\"><br><br>"
    "WiFi 密码:<br><input name=\"password\" type=\"password\" style=\"width:100%;padding:8px\"><br><br>"
    "<button type=\"submit\" style=\"padding:10px 20px\">保存并连接</button>"
    "</form></body></html>";

/* 从 "key=value&key2=value2" 形式的 body 里取某个 key 的值（最小实现）。 */
static void parse_form_value(const char *body, const char *key,
                             char *out, size_t out_len)
{
    char *p = strstr(body, key);
    if (!p) return;
    p += strlen(key) + 1;                 // 跳过 "key="
    char *end = strchr(p, '&');           // 值到下一个 & 或结尾
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    /* 最小 URL 解码：把 '+' 还原成空格（密码里的 & 会截断，属已知简化限制）。 */
    for (size_t i = 0; i < len; i++) {
        if (out[i] == '+') out[i] = ' ';
    }
}

static esp_err_t prov_get_handler(httpd_req_t *req)
{
    httpd_resp_send(req, PROV_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t prov_post_handler(httpd_req_t *req)
{
    /* 读取 POST body（表单数据）。 */
    char buf[256] = {0};
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int r = httpd_req_recv(req, buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) break;     // 收完或出错
        total += r;
    }
    buf[total] = '\0';

    char ssid[WIFI_MAX_SSID_LEN] = {0};
    char pass[WIFI_MAX_PASS_LEN] = {0};
    parse_form_value(buf, "ssid", ssid, sizeof(ssid));
    parse_form_value(buf, "password", pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        const char *err = "<html><body>SSID 不能为空，<a href=\"/\">返回</a></body></html>";
        httpd_resp_send(req, err, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_ERROR_CHECK(wifi_save_creds(ssid, pass));
    ESP_LOGI(TAG, "配网网页提交：SSID=%s（已存 NVS）", ssid);

    const char *ok =
        "<html><body><h3>已保存！</h3><p>设备正在连接家里 WiFi，请稍候...</p></body></html>";
    httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);

    /* 响应发完后再切回 STA（HTTP 任务里直接切即可）。 */
    provisioning_finish();
    return ESP_OK;
}

/* 启动网页服务器（GET 看页面、POST 收账号）。 */
static esp_err_t start_prov_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;   // 支持 "/*" 通配

    static const httpd_uri_t uri_get = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = prov_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t uri_post = {
        .uri      = "/*",
        .method   = HTTP_POST,
        .handler  = prov_post_handler,
        .user_ctx = NULL,
    };

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_register_uri_handler(s_server, &uri_get);
        httpd_register_uri_handler(s_server, &uri_post);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* 进入配网模式：ESP32 变成热点 + 起网页服务器。 */
esp_err_t wifi_start_provisioning(void)
{
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", PROV_AP_SSID);
    ap.ap.ssid_len    = strlen(PROV_AP_SSID);
    ap.ap.channel     = PROV_AP_CHANNEL;
    ap.ap.max_connection = PROV_AP_MAX_CONN;
    ap.ap.authmode    = WIFI_AUTH_OPEN;   // 开放热点、无密码（配网体验最简单）

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));  // 同时支持 AP+STA
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "配网热点已启动：SSID=%s（开放，无密码）", PROV_AP_SSID);
    ESP_LOGI(TAG, "手机连上后，浏览器打开 http://192.168.4.1 填写家里 WiFi");

    return start_prov_http_server();
}
