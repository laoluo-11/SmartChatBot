/* =========================================================================
 * button.h —— 按键模块（L5 改造：支持三个按键，各管一个功能）
 * -------------------------------------------------------------------------
 * 对外暴露三样：
 *   1) button_init()             ：初始化三个按键的 GPIO 中断 + 事件队列（先调）
 *   2) button_task()             ：按键任务，由 app_main 创建成 FreeRTOS 任务
 *   3) button_register_callback()：注册"按键按下时要做什么"（回调收到的是"动作")
 *
 * 三个按键接线（接法：按键一端接 GPIO，另一端接 GND；内部开上拉，按下=下降沿）：
 *   按键1 (唤醒)  → GPIO0  （语音唤醒：开始一次聆听/对话）
 *   按键2 (音量-) → GPIO39 （降低音量）
 *   按键3 (音量+) → GPIO40 （升高音量）
 * 改脚只动下面三个宏即可（避开已用的 4/5/6/7/15/16/41/42/48）。
 *
 * ⚠️ 接线注意：
 *   - GPIO0 是 ESP32-S3 的 strapping 脚（上电/复位瞬间决定启动模式）。
 *     用作"唤醒键"完全没问题，但**不要在板子复位/上电的瞬间按住它**，
 *     否则芯片会进下载模式而不是正常启动。
 *   - GPIO39/40 默认是 JTAG 脚（TMS/TDI）。当普通输入用没问题，
 *     代价是你以后没法把外部 JTAG 调试器插到这几个脚上（USB 串口烧录不受影响）。
 * ========================================================================= */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ===== 按键接线：按你手里实际接线改这里 ===== */
#define BTN_WAKE_GPIO      0        // 按键1：唤醒（语音唤醒/开始对话）
#define BTN_VOL_DOWN_GPIO  39       // 按键2：音量 -
#define BTN_VOL_UP_GPIO    40       // 按键3：音量 +
#define BUTTON_DEBOUNCE_MS  50       // 软件消抖：按下后等 50ms 再确认，滤掉弹簧抖动

/* 按键"动作"枚举：每个物理按键对应一种语义功能。
 * 回调里拿到的是这个动作，而不是 gpio 号——这样将来换脚也不影响上层逻辑。 */
typedef enum {
    BTN_WAKE = 0,       // 唤醒：开始聆听/对话
    BTN_VOL_DOWN,       // 音量降低
    BTN_VOL_UP,         // 音量升高
    BTN_ACTION_COUNT    // 动作总数（仅用于边界，不要当动作用）
} button_action_t;

/* 初始化三个按键：配置各自 GPIO 上拉+下降沿中断，建一个事件队列。必须在最前调用。 */
void button_init(void);

/* 按键任务：等中断发来的事件 -> 软件消抖 -> 按 gpio 映射到动作 -> 调用注册的回调。
 * 由 app_main 创建成任务。 */
void button_task(void *pvParameters);

/* 注册"按键按下"时要执行的函数（回调）。
 * 回调参数为 button_action_t，用来区分是哪个功能被按了。 */
void button_register_callback(void (*cb)(button_action_t action));
