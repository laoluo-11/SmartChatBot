/* =========================================================================
 * state_machine.c —— 机器人状态机（L5 新增的"中枢"）
 * -------------------------------------------------------------------------
 * 它干三件事：
 *   1) 记住"现在在哪个状态"（g_state）
 *   2) bot_set_state() 切状态时，自动把 OLED 屏幕更新成 STATE: xxx
 *   3) 同时把板载 RGB 灯设成对应颜色；进入 SPEAKING 时额外播一声测试音
 *
 * 设计意图：别处（比如按键回调）只要喊"切到下一个状态"，屏幕/灯/喇叭就都跟上了，
 * 不用在按键代码里又写 OLED、又写 LED、又写喇叭——那会乱。状态机就是那个"中枢"。
 * ========================================================================= */

#include "state_machine.h"
#include "oled.h"        // oled_show_status()：把状态名打到屏幕
#include "led.h"         // led_set_color() / led_off()：板载 RGB 灯
#include "audio_out.h"   // audio_out_play_tone()：SPEAKING 时播测试音
#include "esp_log.h"
#include <string.h>

static const char *TAG = "state";   // 日志标签："state: ..."

/* 当前状态。开机默认空闲，等用户按键。 */
static bot_state_t g_state = STATE_IDLE;

/* 状态名查表：枚举 -> 字符串（给 OLED 显示） */
const char *bot_state_to_str(bot_state_t s)
{
    switch (s) {
        case STATE_IDLE:      return "IDLE";
        case STATE_LISTENING: return "LISTENING";
        case STATE_THINKING:  return "THINKING";
        case STATE_SPEAKING:  return "SPEAKING";
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
        default: break;
    }

    /* 3) 进入"说话"状态：喇叭播一声 1kHz 测试音，模拟机器人开口 */
    if (new_state == STATE_SPEAKING) {
        audio_out_play_tone(1000, 800);
    }
}
