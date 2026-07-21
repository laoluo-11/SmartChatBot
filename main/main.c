/* =========================================================================
 * main.c —— 程序入口（L5 按键+状态机 + L6 WiFi/NVS/SoftAP 配网）
 * -------------------------------------------------------------------------
 * 现在各硬件模块各司其职：
 *   - mic.c           ：麦克风采集（打印 RMS 音量，做"系统还活着"的指示）
 *   - audio_out.c     ：喇叭（audio_out_play_tone 播测试音）
 *   - oled.c          ：OLED（oled_show_status 显示状态，oled_draw_text 写任意字）
 *   - led.c           ：板载 RGB（led_set_color 按状态变色）
 *   - state_machine.c ：状态机（中枢：切状态时自动驱动 OLED + LED）
 *   - button.c        ：按键（中断 → 队列 → 任务 → 回调，推进状态机）
 *   - wifi.c          ：WiFi/配网（L6：NVS 存账号、SoftAP 热点+网页配网、STA 连家里）
 *
 * L5 对话流程（一键触发全自动）：
 *   按键1 (GPIO0  唤醒) → 进入 LISTENING → 等用户说话 → 静音后 → THINKING（5秒）
 *                      → SPEAKING（喇叭播 3 秒正弦波）→ 自动回 IDLE
 *   按键2 (GPIO39 音量-) → 喇叭音量 -10，屏幕显示 VOL xx%
 *   按键3 (GPIO40 音量+) → 喇叭音量 +10，屏幕显示 VOL xx%
 *
 * L6 联网流程（开机自动）：
 *   有存档账号 → STA 直连家里 WiFi（屏幕 CONNECTING → ONLINE）
 *   无存档账号 → ESP32 变热点(ESP32-Chatbot) + 网页，手机填账号后存 NVS 并切 STA
 * 喇叭只在 SPEAKING 状态才发声，其他时候静音。
 * ========================================================================= */

#include <stdio.h>              // 标准输入输出（习惯带上）
#include "freertos/FreeRTOS.h" // FreeRTOS 内核
#include "freertos/task.h"     // 任务创建 xTaskCreate / 延时 vTaskDelay
#include "esp_err.h"
#include "esp_log.h"           // 日志打印
#include "esp_heap_caps.h"     // 内存能力查询：探测板子有没有 PSRAM
#include "led.h"               // 板载 RGB 灯（led_init / led_set_color / led_off）
#include "mic.h"               // 麦克风模块（mic_init / mic_task）
#include "audio_out.h"         // 喇叭模块（audio_out_init / play_tone / task / 音量）
#include "oled.h"              // OLED 模块（oled_init / 清屏 / 画点 / 写字 / 显示）
#include "state_machine.h"     // 状态机（bot_init / bot_set_state / bot_get_state / bot_state_to_str / bot_state_task）
#include "button.h"            // 按键模块（button_init / button_task / button_register_callback）
#include "wifi.h"              // WiFi / NVS / SoftAP 配网（L6 新增）

static const char *TAG = "main";  // 本文件日志标签："main: ..."（入口相关的日志归这里）

/* -------------------------------------------------------------------------
 * show_volume：把当前音量打到 OLED（音量键调完顺手看一下）
 * ------------------------------------------------------------------------- */
static void show_volume(void)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", audio_out_get_volume());
    oled_show_lines("VOL:", buf, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * on_button_pressed：按键"按下"时要做的事（按键模块会在任务里回调它）
 *   唤醒键 → 进入 LISTENING（已在该状态则忽略，同状态不重复进）
 *   音量键 → 调音量 + 屏幕显示
 * ------------------------------------------------------------------------- */
static void on_button_pressed(button_action_t action)
{
    switch (action) {
        case BTN_WAKE:
            ESP_LOGI(TAG, "唤醒键：进入 LISTENING");
            bot_set_state(STATE_LISTENING);
            break;
        case BTN_VOL_DOWN:
            audio_out_volume_down();
            show_volume();
            break;
        case BTN_VOL_UP:
            audio_out_volume_up();
            show_volume();
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------------------
 * on_wifi_connected：WiFi 真正连上（拿到 IP）时的回调。
 * 由 wifi 模块在事件里异步调用，用来把 OLED/LED 显示成"在线"。
 * ------------------------------------------------------------------------- */
static void on_wifi_connected(void)
{
    ESP_LOGI(TAG, "WiFi 已连上，机器人进入在线状态");
    oled_show_status("ONLINE");          // 屏幕显示 ONLINE
    led_set_color(0, 255, 80);           // 绿灯：在线
}

/* -------------------------------------------------------------------------
 * demo_all_features：开机自检——逐一调用各模块"还没被用到的"公开函数，
 * 让 mic / 喇叭 / OLED / LED / 状态机 的全部能力都被执行到一遍。
 * 注意：不再手动遍历状态机（交给 bot_state_task 负责），只做硬件自检。
 * ------------------------------------------------------------------------- */
static void demo_all_features(void)
{
    ESP_LOGI(TAG, "===== 开机自检：逐一调用各模块功能 =====");

    /* ---- 1) LED：亮红 → 亮绿 → 熄灭 ---- */
    ESP_LOGI(TAG, "[LED] 亮红色");
    led_set_color(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "[LED] 亮绿色");
    led_set_color(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "[LED] 熄灭（led_off）");
    led_off();

    /* ---- 2) OLED：清屏 → 画点 → 写字 → 刷新 → 状态屏 → 多行 ---- */
    ESP_LOGI(TAG, "[OLED] 清屏 + 画像素点 + 写文字 + 刷新");
    oled_clear();
    oled_draw_pixel(10, 10, true);
    oled_draw_pixel(12, 10, true);
    oled_draw_pixel(10, 12, true);
    oled_draw_text(0, 0, "HELLO");
    oled_draw_text(0, 16, "ESP32-S3");
    oled_refresh();
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "[OLED] oled_show_status(\"DEMO\")");
    oled_show_status("DEMO");
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "[OLED] oled_show_lines(4 行)");
    oled_show_lines("LINE0", "LINE1", "LINE2", "LINE3");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* ---- 3) 喇叭：设音量 + 短播测试音验证硬件通路 ---- */
    ESP_LOGI(TAG, "[AUDIO] 设置音量 80%%");
    audio_out_set_volume(80);
    show_volume();
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "[AUDIO] 播放测试音 1kHz 300ms");
    audio_out_play_tone(1000, 300);
    vTaskDelay(pdMS_TO_TICKS(300));

    /* ---- 4) 状态机查询 ---- */
    bot_state_t s = bot_get_state();
    const char *name = bot_state_to_str(s);
    ESP_LOGI(TAG, "[STATE] 当前状态=%s (%d)", name, s);

    ESP_LOGI(TAG, "===== 开机自检完成 =====");

    /* 恢复默认音量 30%，并把开机屏幕显式刷成 IDLE（bot_set_state(STATE_IDLE) 因状态未变是空操作，
     * 不会刷新 OLED，所以这里直接调 oled_show_status 确保开机就显示 STATE: IDLE） */
    audio_out_set_volume(30);
    oled_show_status(bot_state_to_str(STATE_IDLE));
}

/* -------------------------------------------------------------------------
 * app_main：程序入口
 * ------------------------------------------------------------------------- */
void app_main(void)
{
    /* 第一步：探测板子到底有没有 PSRAM（外部大内存）。 */
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total == 0) {
        ESP_LOGI(TAG, "=== 语音聊天机器人 固件 (L5 按键+状态机) ===");
        ESP_LOGI(TAG, "[模式] 本板无 PSRAM，运行于内部 SRAM 模式。");
    } else {
        ESP_LOGI(TAG, "=== 语音聊天机器人 固件 (L5 按键+状态机) ===");
        ESP_LOGI(TAG, "PSRAM 已启用，总容量=%u KB", (unsigned)(psram_total / 1024));
    }

    /* 第二步：初始化所有硬件模块。
     * 注意顺序：led_init 必须先于 bot_init（状态机会在初始化时灭灯）。 */
    led_init();        // 初始化板载 RGB 灯
    oled_init();       // 建立 I2C 总线 + SSD1306 面板，清屏
    mic_init();        // 打开并配置 I2S 麦克风通道
    audio_out_init();  // 打开并配置 I2S 喇叭发送通道

    /* 第三步：初始化状态机（把当前状态设成 IDLE，并刷一次屏幕 + 灭灯） */
    bot_init();

    /* 第三步半（L6）：初始化 WiFi 子系统，并按 NVS 里是否存过账号决定启动方式：
     *   - 有存档账号 → 直接以 STA 模式连家里 WiFi（屏幕显示 CONNECTING）
     *   - 没存档账号 → 进入配网模式（ESP32 自己变热点 + 网页，屏幕显示 PROVISION）
     * 无论哪种，连上后 on_wifi_connected 会把屏幕刷成 ONLINE、灯变绿。 */
    wifi_init();
    wifi_register_connected_cb(on_wifi_connected);
    if (wifi_has_saved_creds()) {
        ESP_LOGI(TAG, "检测到已保存 WiFi 账号，尝试以 STA 连接...");
        oled_show_status("CONNECTING");
        wifi_try_connect_saved();
    } else {
        ESP_LOGI(TAG, "未配置 WiFi，进入配网模式（热点 ESP32-Chatbot）...");
        bot_set_state(STATE_PROVISIONING);   // OLED 显示 PROVISION + 黄灯
        wifi_start_provisioning();
    }

    /* 第四步：注册"按键按下"的回调，并初始化按键（GPIO 中断 + 事件队列）。 */
    button_register_callback(on_button_pressed);
    esp_err_t btn_init_err = button_init();
    if (btn_init_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(btn_init_err));
    }

    /* 第五步：创建后台任务。
     * 麦克风持续采集（打印 RMS）作为"系统活着"的指示；
     * 状态机任务：自动推进 LISTENING → THINKING → SPEAKING → IDLE；
     * 按键任务等中断事件、消抖、分派动作。 */
    BaseType_t mic_ok = xTaskCreate(mic_task, "mic", 4096, NULL, 4, NULL);
    if (mic_ok != pdPASS) {
        ESP_LOGE(TAG, "麦克风任务创建失败");
    }

    BaseType_t bot_ok = xTaskCreate(bot_state_task, "bot_st", 4096, NULL, 5, NULL);
    if (bot_ok != pdPASS) {
        ESP_LOGE(TAG, "状态机任务创建失败");
    }

    if (btn_init_err == ESP_OK) {
        BaseType_t btn_ok = xTaskCreate(button_task, "btn", 3072, NULL, 5, NULL);
        if (btn_ok != pdPASS) {
            ESP_LOGE(TAG, "按键任务创建失败");
        }
    }

    /* 第六步：开机自检——逐一调用各模块函数，确保硬件都通。 */
    demo_all_features();

    ESP_LOGI(TAG, "系统就绪：按唤醒键开始对话（IDLE→LISTEN→THINK→SPEAK→IDLE 全自动）；联网见上方日志");
}
