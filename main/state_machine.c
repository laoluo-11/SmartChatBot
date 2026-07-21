/* =========================================================================
 * state_machine.c —— 机器人状态机（L5 新增的"中枢"）
 * -------------------------------------------------------------------------
 * 它干三件事：
 *   1) 记住"现在在哪个状态"（g_state）
 *   2) bot_set_state() 切状态时，自动把 OLED 屏幕更新成 STATE: xxx
 *   3) 同时把板载 RGB 灯设成对应颜色；进入 SPEAKING 时额外播一声测试音
 *
 * bot_state_task 是新增的自动推进任务：
 *   IDLE:      什么都不做，等按键唤醒
 *   LISTENING: 监听麦克风 → 检测到声音后再静音 → 自动 → THINKING
 *   THINKING:  等待 5 秒 → 自动 → SPEAKING
 *   SPEAKING:  等待 3 秒（喇叭播正弦波）→ 自动 → IDLE
 *
 * 设计意图：别处（比如按键回调）只要喊"切到下一个状态"，屏幕/灯/喇叭就都跟上了，
 * 不用在按键代码里又写 OLED、又写 LED、又写喇叭——那会乱。状态机就是那个"中枢"。
 * ========================================================================= */

#include "state_machine.h"
#include "oled.h"        // oled_show_status()：把状态名打到屏幕
#include "led.h"         // led_set_color() / led_off()：板载 RGB 灯
#include "audio_out.h"   // audio_out_play_tone()：SPEAKING 时播测试音
#include "mic.h"         // mic_get_rms()：读取当前麦克风音量
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "state";   // 日志标签："state: ..."

/* 当前状态。开机默认空闲，等用户按键。 */
static bot_state_t g_state = STATE_IDLE;

/* 静音检测阈值：RMS 低于此值认为"没在说话" */
#define SILENCE_THRESHOLD   200.0f
/* 静音持续多久（毫秒）才确认用户说完了，自动切到 THINKING */
#define SILENCE_TIMEOUT_MS  1500
/* LISTENING 最长等多久（毫秒），超时还没人说话就退回 IDLE */
#define LISTEN_MAX_MS       8000

/* 状态名查表：枚举 -> 字符串（给 OLED 显示） */
const char *bot_state_to_str(bot_state_t s)
{
    switch (s) {
        case STATE_IDLE:      return "IDLE";
        case STATE_LISTENING: return "LISTENING";
        case STATE_THINKING:  return "THINKING";
        case STATE_SPEAKING:  return "SPEAKING";
        case STATE_PROVISIONING: return "PROVISION";
        default:              return "UNKNOWN";
    }
}

bot_state_t bot_get_state(void)
{
    return g_state;
}

void bot_init(void)
{
    g_state = STATE_IDLE;
    oled_show_status(bot_state_to_str(STATE_IDLE));  // 屏幕先显示 IDLE
    led_off();                                        // 灯先灭
    ESP_LOGI(TAG, "状态机已初始化，当前 IDLE");
}

void bot_set_state(bot_state_t new_state)
{
    if (new_state >= STATE_COUNT) return;     // 防御：非法状态直接忽略
    if (new_state == g_state) return;         // 同状态不重复处理（避免重复播声音等副作用）

    g_state = new_state;
    const char *name = bot_state_to_str(new_state);
    ESP_LOGI(TAG, "状态切换 -> %s", name);

    /* 1) 屏幕：显示 STATE: / 状态名（oled_show_status 内部已清屏+刷新） */
    oled_show_status(name);

    /* 2) LED 颜色随状态变（板载 RGB 灯） */
    switch (new_state) {
        case STATE_IDLE:      led_set_color(0, 0, 0);     break;  // 灭
        case STATE_LISTENING: led_set_color(0, 60, 255);  break;  // 蓝：在听
        case STATE_THINKING:  led_set_color(200, 0, 255); break;  // 紫：在想
        case STATE_SPEAKING:  led_set_color(0, 255, 80);  break;  // 绿：在说
        case STATE_PROVISIONING: led_set_color(255, 255, 0); break; // 黄：配网中
        default: break;
    }

    /* 3) 进入"说话"状态：不在这里播放，交给 bot_state_task 的 SPEAKING 分支处理。
     * bot_set_state 只管 OLED + LED，不做阻塞操作。 */
}

/* -------------------------------------------------------------------------
 * bot_state_task：状态机自动推进任务
 * ------------------------------------------------------------------------- */
void bot_state_task(void *pvParameters)
{
    (void)pvParameters;

    /* VAD 状态变量：仅在 LISTENING 中使用 */
    bool sound_detected = false;       // 是否已检测到有人说话
    uint32_t silence_start = 0;       // 静音开始的时间戳
    uint32_t listen_start = 0;        // 进入 LISTENING 的时间戳

    while (1) {
        bot_state_t cur = bot_get_state();

        switch (cur) {

        case STATE_LISTENING: {
            /* 记录首次进入 LISTENING 的时刻 */
            if (!sound_detected && listen_start == 0) {
                listen_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
                ESP_LOGI(TAG, "LISTENING: 开始监听...");
            }

            float rms = mic_get_rms();
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

            /* 检测到声音：RMS 超过阈值 */
            if (rms > SILENCE_THRESHOLD) {
                if (!sound_detected) {
                    sound_detected = true;
                    ESP_LOGI(TAG, "LISTENING: 检测到声音 (RMS=%.0f)", rms);
                }
                silence_start = 0;  // 还在说话，重置静音计时
            }

            /* 检测到声音后，开始计静音时长 */
            if (sound_detected && rms <= SILENCE_THRESHOLD) {
                if (silence_start == 0) {
                    silence_start = now;
                } else if (now - silence_start >= SILENCE_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "LISTENING: 静音 %.0f ms → 进入 THINKING",
                             (float)(now - silence_start));
                    sound_detected = false;
                    silence_start = 0;
                    listen_start = 0;
                    bot_set_state(STATE_THINKING);
                    break;  // 状态已变，跳出 switch
                }
            }

            /* 超时：等了 LISTEN_MAX_MS 还没人说话 → 退回 IDLE */
            if (!sound_detected && now - listen_start >= LISTEN_MAX_MS) {
                ESP_LOGI(TAG, "LISTENING: %d ms 无声音 → 退回 IDLE", LISTEN_MAX_MS);
                listen_start = 0;
                bot_set_state(STATE_IDLE);
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(50));  // 每 50ms 检查一次
            break;
        }

        case STATE_THINKING:
            ESP_LOGI(TAG, "THINKING: 思考中（5 秒）...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            bot_set_state(STATE_SPEAKING);
            break;

        case STATE_SPEAKING:
            ESP_LOGI(TAG, "SPEAKING: 播放中（3 秒）...");
            audio_out_play_tone(1000, 3000);
            bot_set_state(STATE_IDLE);
            break;

        default:
            /* IDLE 或其他未知状态：什么也不做，睡一会儿再查 */
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }
    }
}
