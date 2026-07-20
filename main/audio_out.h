/* =========================================================================
 * audio_out.h —— 喇叭(MAX98357A)播放模块的"对外说明书"
 * -------------------------------------------------------------------------
 * 对外暴露三样东西：
 *   1) audio_out_init()        ：初始化 I2S 发送通道（相当于"打开喇叭"）
 *   2) audio_out_play_tone()   ：播放一段指定频率/时长的测试音（正弦波）
 *   3) audio_out_task()        ：[可选] 循环播放"1秒 1kHz → 停 2 秒"的演示任务。
 *                              L5 起喇叭改由状态机在 SPEAKING 时驱动，默认不创建此任务；
 *                              若想恢复"每 3 秒自动嘀一声"的演示，在 main.c 里
 *                              xTaskCreate(audio_out_task, ...) 即可。
 *   4) 音量控制                 ：audio_out_set_volume / get_volume / volume_up / volume_down
 *                              音量用 0~100 的百分比表示，play_tone 会把正弦波振幅按它缩放。
 *
 * 配置宏（引脚、采样率）放这里，方便你在一个地方改接线。
 * ========================================================================= */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include "driver/i2s_std.h"   // 为了使用 I2S_NUM_1 等类型/宏（函数原型里会用到）

/* ===== MAX98357A 数字功放的接线与参数：按你手里的接线改这里 =====
 * 你实际接线（L3 实测版）：
 *   MAX98357A.DIN  → ESP32 GPIO7   （数据：ESP 把声音送出去）
 *   MAX98357A.BCLK → ESP32 GPIO15  （位时钟）
 *   MAX98357A.LRC  → ESP32 GPIO16  （声道选择 WSEL/LRC）
 *   MAX98357A.GAIN → GND           （增益 9dB，固定接 GND 即可，无需代码控制）
 *   MAX98357A.SD   → 3V3           （关断脚接高电平 = 功放常开，上电即工作）
 * 注意：DIN 是"功放的数据输入脚"，对应 ESP32 这边的 I2S TX 数据"输出"（dout）。
 */
#define I2S_SPK_NUM      I2S_NUM_1   // 喇叭用第 1 个 I2S 控制器（麦克风用 I2S0，分开避免互相抢占）
#define SPK_BCLK_GPIO    15          // MAX98357A 的 BCLK 脚 → ESP32 的 GPIO15（位时钟，ESP 输出）
#define SPK_WS_GPIO      16          // MAX98357A 的 WSEL/LRC 脚 → ESP32 的 GPIO16（声道选择，ESP 输出）
#define SPK_DOUT_GPIO    7           // MAX98357A 的 DIN 脚 → ESP32 的 GPIO7（数据，ESP 输出到功放）
#define SPK_SAMPLE_RATE  16000       // 采样率 16000Hz（和麦克风保持一致，将来 ASR 也吃 16kHz）

/* 初始化 I2S 喇叭发送通道。必须在 audio_out_play_tone / 创建 audio_out_task 之前调用一次。 */
void audio_out_init(void);

/* 播放一段正弦波测试音。
 *   freq_hz     ：频率，比如 1000 = 1kHz（人耳易辨别的"嘀"声）
 *   duration_ms ：持续时间，单位毫秒，比如 1000 = 1 秒 */
void audio_out_play_tone(uint16_t freq_hz, uint32_t duration_ms);

/* [可选] 喇叭演示任务：循环播放"1秒 1kHz 测试音 → 停 2 秒"。
 * 默认不创建（见本文件头说明）；需要自动"嘀"演示时由 main.c 创建。 */
void audio_out_task(void *pvParameters);

/* ---------- 音量控制（配合"音量+/音量-"按键使用） ---------- */
#define AUDIO_VOL_MAX  100     // 最大音量（百分比）
#define AUDIO_VOL_MIN  0       // 最小音量（静音）

void audio_out_set_volume(int vol);   // 设置音量（自动 clamp 到 AUDIO_VOL_MIN~MAX）
int  audio_out_get_volume(void);      // 读取当前音量（0~100）
void audio_out_volume_up(void);       // 音量 +10（封顶 100）
void audio_out_volume_down(void);     // 音量 -10（到底 0）

#endif /* AUDIO_OUT_H */
