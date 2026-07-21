/* =========================================================================
 * wifi.h —— WiFi / NVS / SoftAP 配网模块（L6 新增）
 * -------------------------------------------------------------------------
 * 这个模块解决一个问题：机器人要连家里的 WiFi，但 WiFi 账号密码不能写死在
 * 代码里（换了网络就废了）。做法是：
 *   1) 用 NVS（芯片上的一块"掉电不丢"的小存储）保存账号密码
 *   2) 开机先读 NVS：有存档就直接以 STA 模式连家里 WiFi
 *   3) 没存档就进入"配网模式"：ESP32 自己变成一个 WiFi 热点(SoftAP)，
 *      并起一个网页服务器；手机连上热点、浏览器打开网页、填家里账号，
 *      提交后存进 NVS，ESP32 自动切回 STA 去连家里 WiFi。
 *
 * 这样账号密码既不写死、又能"换网络重新配"，是物联网设备最常见的配网方式。
 * ========================================================================= */

#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* NVS 命名空间：把 WiFi 账号密码单独放在 "wifi_creds" 名下，避免和其它键值冲突。 */
#define WIFI_NVS_NAMESPACE   "wifi_creds"
#define WIFI_MAX_SSID_LEN    32      // SSID 最长 32 字节（ESP32 硬件限制）
#define WIFI_MAX_PASS_LEN    64      // 密码最长 64 字节

/* "配网成功连上家里 WiFi"时回调（main 用它切 OLED 显示）。可以为 NULL。 */
typedef void (*wifi_connected_cb_t)(void);

/* 初始化 WiFi 子系统（必须最先调用）：
 *   - 初始化 NVS（存账号密码）
 *   - 初始化 TCP/IP 协议栈(esp_netif) 与默认事件循环
 *   - 创建 STA 和 AP 两个网络接口
 * 注意：本函数不会真正"连 WiFi"，只做好准备工作。 */
esp_err_t wifi_init(void);

/* NVS 里有没有存过账号？有则返回 true（可用于启动判断要不要进配网）。 */
bool wifi_has_saved_creds(void);

/* 用 NVS 里存过的账号，以 STA 模式去连家里 WiFi。
 * 返回 true 表示"有存档且已发起连接"（到底连没连上，看日志里的 IP 或回调）。
 * 返回 false 表示 NVS 里没账号，调用方应转去配网。 */
bool wifi_try_connect_saved(void);

/* 把账号密码存进 NVS（配网网页提交后调用）。 */
esp_err_t wifi_save_creds(const char *ssid, const char *password);

/* 进入配网模式：启动 SoftAP（ESP32 变热点）+ 启动网页服务器。
 * 手机连上热点后浏览器打开 http://192.168.4.1 即可填账号。 */
esp_err_t wifi_start_provisioning(void);

/* 注册"连上 WiFi"回调（可选，传 NULL 取消）。 */
void wifi_register_connected_cb(wifi_connected_cb_t cb);
