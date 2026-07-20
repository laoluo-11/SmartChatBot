/* =========================================================================
 * state_machine.h —— 机器人状态机模块（L5 新增的"中枢"）
 * -------------------------------------------------------------------------
 * 它把"机器人现在在干嘛"分成几个明确状态，并提供切换函数。
 * 好处：以后无论谁（按键、定时器、网络事件）想改变状态，只喊一声
 *       bot_set_state(下一个状态)，屏幕/灯/喇叭就自动跟上，不用到处改代码。
 *
 * 后续关卡（L6+ 接真实语音流程）会在这里扩展更多状态，例如：
 *   STATE_PROVISIONING （配网中）、STATE_OFFLINE（离线兜底）、STATE_ERROR（出错）
 * ========================================================================= */

#pragma once
#include <stdint.h>

/* 机器人状态枚举。STATE_COUNT 不是真实状态，只是用来标记"一共有几个"。 */
typedef enum {
    STATE_IDLE = 0,    // 空闲：等用户按键，开始一次对话
    STATE_LISTENING,   // 聆听：正在采集用户说话（将来接麦克风 VAD 人声检测）
    STATE_THINKING,    // 思考：把语音送服务器做 ASR+LLM，等回复
    STATE_SPEAKING,    // 说话：收到回复，TTS 后从喇叭播放
    STATE_COUNT        // 状态总数（仅用于边界判断，不要当它当状态用）
} bot_state_t;

/* 状态 -> 字符串（给 OLED 显示用），例如 STATE_LISTENING -> "LISTENING" */
const char *bot_state_to_str(bot_state_t s);

/* 取当前状态 */
bot_state_t bot_get_state(void);

/* 初始化状态机：把当前状态设成 IDLE，并刷一次屏幕（显示 IDLE）+ 灭灯。
 * 必须在其它 bot_xxx 之前、且 led_init() 之后调用。 */
void bot_init(void);

/* 切换状态（核心函数）：
 *   - 更新内部当前状态
 *   - 自动把 OLED 屏幕刷成 STATE: xxx
 *   - 自动把板载 RGB 灯设成对应颜色（蓝=听、紫=想、绿=说、灭=空闲）
 *   - 进入 SPEAKING 时额外播一声 1kHz 测试音，模拟机器人"开口"
 * 同状态重复设置会被忽略（避免重复播声音）。 */
void bot_set_state(bot_state_t new_state);

/* 状态机自动推进任务：监听麦克风 → 检测声音/静音 → 自动切状态。
 * LISTENING: 等用户说话 → 静音后 → THINKING
 * THINKING:  等待 5 秒 → SPEAKING
 * SPEAKING:  等待 3 秒（喇叭播正弦波）→ IDLE
 * 由 app_main 创建为 FreeRTOS 任务。 */
void bot_state_task(void *pvParameters);
