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
#include <stdio.h>            // sscanf（表单 URL 解码用）
#include <stdlib.h>           // malloc / free（扫描结果缓冲）
#include <ctype.h>            // isxdigit（URL 解码用）

static const char *TAG = "wifi";

/* ---- 一些模块内部状态 ---- */
static wifi_connected_cb_t s_connected_cb = NULL;   // "连上 WiFi"回调（可为空）
static httpd_handle_t      s_server = NULL;         // 配网页服务器句柄
static int                 s_retry_num = 0;         // STA 断线重连次数
static wifi_provisioning_cb_t s_provisioning_cb = NULL;  // "需重新配网"回调（可为空）
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
            ESP_LOGW(TAG, "WiFi 重连 %d 次仍失败，自动进入配网模式（开 SoftAP 热点）", WIFI_MAX_RETRY);
            if (s_provisioning_cb) s_provisioning_cb();   // 通知 main 切 PROVISIONING 显示
            esp_wifi_stop();                               // 先停当前 STA，才能切 AP 模式
            wifi_start_provisioning();                    // 开 SoftAP 热点 + 网页服务器
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

void wifi_register_provisioning_cb(wifi_provisioning_cb_t cb)
{
    s_provisioning_cb = cb;
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
    s_retry_num = 0;   // 重置重连计数：新账号若连不上，重新计 5 次再退回配网
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "配网完成，切换到 STA 连接家里 WiFi: %s", ssid);
}

/* =========================================================================
 * 配网网页服务器：GET 返回填写页面，POST 收账号并存 NVS
 * ========================================================================= */

/* 配网网页（手机浏览器打开就能填）。
 * 改进点：打开页面会自动 fetch /api/scan 扫描附近 WiFi，
 * 用自绘的自定义下拉列表（div 弹层，兼容手机浏览器）把搜到的
 * 网络名做成可点选列表，不用手动敲 SSID，也支持手输与实时过滤。 */
static const char *PROV_HTML =
"<!DOCTYPE html><html lang='zh'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 配网</title>"
"<style>"
"body{font-family:-apple-system,Segoe UI,sans-serif;padding:16px;max-width:420px;margin:auto;color:#222}"
"h2{font-size:20px;margin:0 0 4px}"
"label{display:block;margin:14px 0 4px;font-weight:bold}"
"input{width:100%;padding:10px;font-size:16px;box-sizing:border-box;border:1px solid #ccc;border-radius:6px}"
"button{padding:12px 18px;font-size:15px;background:#1a73e8;color:#fff;border:0;border-radius:6px;cursor:pointer}"
".row{display:flex;gap:8px;align-items:stretch}"
".row input{flex:1}"
".muted{color:#666;font-size:13px;margin:6px 0}"
".ssid-box{position:relative}"
".ssid-pop{position:absolute;left:0;right:0;top:100%;margin-top:4px;background:#fff;border:1px solid #ccc;border-radius:6px;max-height:240px;overflow:auto;z-index:10;box-shadow:0 4px 12px rgba(0,0,0,.12);display:none}"
".ssid-pop.open{display:block}"
".ssid-pop div{padding:10px 12px;font-size:15px;border-bottom:1px solid #f0f0f0;cursor:pointer;display:flex;justify-content:space-between;gap:8px}"
".ssid-pop div:last-child{border-bottom:0}"
".ssid-pop div:active{background:#eef4ff}"
".ssid-pop .sig{color:#1a73e8;font-family:monospace;white-space:nowrap}"
".ssid-pop .none{padding:10px 12px;color:#999;font-size:14px}"
"</style></head><body>"
"<h2>ESP32 语音机器人 · 配网</h2>"
"<p class='muted'>已自动扫描附近 WiFi，点选网络并输入密码即可。</p>"
"<form method='post' action='/' onsubmit='return checkForm()'>"
"<label>WiFi 网络</label>"
"<div class='ssid-box'>"
"<div class='row'>"
"<input name='ssid' id='ssid' placeholder='选择或输入 WiFi 名称' autocomplete='off' oninput='filterList()' onfocus='showList()'>"
"<button type='button' onclick='scan()'>刷新</button>"
"</div>"
"<div id='ssidlist' class='ssid-pop'></div>"
"</div>"
"<div class='muted' id='hint'>正在扫描…</div>"
"<label>WiFi 密码</label>"
"<input name='password' type='password' placeholder='开放网络可留空'>"
"<button type='submit' style='width:100%;margin-top:12px'>保存并连接</button>"
"</form>"
"<script>"
"var allAps=[];"
"function sigBars(r){"
" if(r>=-50)return '▂▄▆█';"
" if(r>=-60)return '▂▄▆';"
" if(r>=-70)return '▂▄';"
" return '▂';"
"}"
"function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
"function showList(){document.getElementById('ssidlist').classList.add('open');}"
"function hideList(){document.getElementById('ssidlist').classList.remove('open');}"
"function renderList(q){"
" var box=document.getElementById('ssidlist');"
" box.innerHTML='';"
" var list=allAps.filter(function(a){return !q||a.ssid.toLowerCase().indexOf(q.toLowerCase())>=0;});"
" if(list.length===0){box.innerHTML='<div class=\"none\">没有匹配的网络，可直接输入</div>';return;}"
" list.forEach(function(a){"
"  var d=document.createElement('div');"
"  d.innerHTML='<span>'+esc(a.ssid)+'</span><span class=\"sig\">'+sigBars(a.rssi)+(a.auth===0?'（开放）':'')+'</span>';"
"  d.onclick=function(){document.getElementById('ssid').value=a.ssid;hideList();};"
"  box.appendChild(d);"
" });"
"}"
"function filterList(){var v=document.getElementById('ssid').value;renderList(v);showList();}"
"function scan(){"
" var hint=document.getElementById('hint');"
" hint.textContent='正在扫描…';"
" fetch('/api/scan').then(function(r){return r.json();}).then(function(d){"
"  allAps=d.aps||[];"
"  if(allAps.length===0){hint.textContent='未找到网络，可直接手动输入';return;}"
"  renderList('');showList();"
"  hint.textContent='已找到 '+allAps.length+' 个网络';"
" }).catch(function(){hint.textContent='扫描失败，可直接手动输入或点刷新';});"
"}"
"document.addEventListener('click',function(e){if(!e.target.closest('.ssid-box'))hideList();});"
"scan();"
"</script>"
"</body></html>";

/* 把 SSID 转成 JSON 安全的字符串：转义 " \ 和控制字符，其余 UTF-8 原样保留。 */
static void json_escape_ssid(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 8 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            int n = snprintf(out + o, out_len - o, "\\u%04x", c);
            o += (size_t)n;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

#define PROV_MAX_AP 20

/* 扫描周围 WiFi，结果写成 JSON 到 out（调用方分配，长度 out_len）。
 * 格式：{"count":N,"aps":[{"ssid":"...","rssi":-45,"auth":3,"ch":6}, ...]} */
static esp_err_t wifi_scan_to_json(char *out, size_t out_len)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    esp_err_t r = esp_wifi_scan_start(&scan_cfg, true);   // 阻塞式扫描
    if (r != ESP_OK) {
        snprintf(out, out_len, "{\"count\":0,\"error\":\"scan_start\"}");
        return r;
    }

    uint16_t num = PROV_MAX_AP;
    wifi_ap_record_t *aps = malloc(sizeof(wifi_ap_record_t) * PROV_MAX_AP);
    if (!aps) {
        snprintf(out, out_len, "{\"count\":0,\"error\":\"oom\"}");
        return ESP_FAIL;
    }
    r = esp_wifi_scan_get_ap_records(&num, aps);
    if (r != ESP_OK) {
        free(aps);
        snprintf(out, out_len, "{\"count\":0,\"error\":\"get_records\"}");
        return r;
    }

    /* 去重：同一 SSID 只保留信号最强的一条。 */
    wifi_ap_record_t uniq[PROV_MAX_AP];
    int uniq_n = 0;
    for (int i = 0; i < (int)num; i++) {
        int j;
        for (j = 0; j < uniq_n; j++) {
            /* ssid[33] 由 ESP-IDF 保证以 '\0' 结尾，strncmp 限长 32 防止越界。 */
            if (strncmp((char *)aps[i].ssid, (char *)uniq[j].ssid, 32) == 0) {
                if (aps[i].rssi > uniq[j].rssi) uniq[j] = aps[i];
                break;
            }
        }
        if (j == uniq_n && uniq_n < PROV_MAX_AP) uniq[uniq_n++] = aps[i];
    }
    free(aps);

    /* 按信号强度排序（强在前），前端展示更直观。 */
    for (int i = 0; i < uniq_n - 1; i++) {
        for (int j = i + 1; j < uniq_n; j++) {
            if (uniq[j].rssi > uniq[i].rssi) {
                wifi_ap_record_t t = uniq[i];
                uniq[i] = uniq[j];
                uniq[j] = t;
            }
        }
    }

    int off = snprintf(out, out_len, "{\"count\":%d,\"aps\":[", uniq_n);
    for (int i = 0; i < uniq_n; i++) {
        if ((size_t)off + 256 >= out_len) break;   // 余量保护，避免越界
        char esc[200];
        json_escape_ssid((char *)uniq[i].ssid, esc, sizeof(esc));
        off += snprintf(out + off, out_len - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"ch\":%d}",
                        (i ? "," : ""), esc, (int)uniq[i].rssi,
                        (int)uniq[i].authmode, (int)uniq[i].primary);
    }
    snprintf(out + off, out_len - off, "]}");
    return ESP_OK;
}

/* GET /api/scan：返回附近 WiFi 的 JSON 列表，给网页自动填充下拉框。 */
static esp_err_t prov_scan_handler(httpd_req_t *req)
{
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    wifi_scan_to_json(buf, 4096);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* 从 "key=value&key2=value2" 形式的 body 里取某个 key 的值。
 * 做了完整的 URL 解码：%XX -> 字节，'+' -> 空格（这样中文 SSID 也能正确解析）。 */
static void parse_form_value(const char *body, const char *key,
                             char *out, size_t out_len)
{
    char *p = strstr(body, key);
    if (!p) return;
    p += strlen(key) + 1;                 // 跳过 "key="
    char *end = strchr(p, '&');           // 值到下一个 & 或结尾
    size_t raw_len = end ? (size_t)(end - p) : strlen(p);

    size_t o = 0;
    for (size_t i = 0; i < raw_len && o < out_len - 1; ) {
        if (p[i] == '%' && i + 2 < raw_len &&
            isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2])) {
            unsigned int v = 0;
            sscanf(p + i + 1, "%2x", &v);  // 把 %XX 两位十六进制转成字节
            out[o++] = (char)v;
            i += 3;
        } else if (p[i] == '+') {
            out[o++] = ' ';                // 表单里空格常被编码成 '+'
            i++;
        } else {
            out[o++] = p[i++];
        }
    }
    out[o] = '\0';
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

/* 启动网页服务器（GET 看页面、POST 收账号、GET /api/scan 出 WiFi 列表）。 */
static esp_err_t start_prov_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;   // 支持通配符匹配
    config.stack_size   = 5120;                        // 扫描时留足栈空间

    static const httpd_uri_t uri_scan = {
        .uri      = "/api/scan",
        .method   = HTTP_GET,
        .handler  = prov_scan_handler,
        .user_ctx = NULL,
    };
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
        /* 精确匹配 /api/scan 优先于通配URI，确保扫描接口不被页面覆盖。 */
        httpd_register_uri_handler(s_server, &uri_scan);
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
