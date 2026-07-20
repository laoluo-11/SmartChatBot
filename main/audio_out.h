#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>

#include "driver/i2s_std.h"

/* MAX98357A 的 I2S 接线定义。 */
#define I2S_SPK_NUM       I2S_NUM_1
#define SPK_BCLK_GPIO     15
#define SPK_WS_GPIO       16
#define SPK_DOUT_GPIO      7
#define SPK_SAMPLE_RATE   16000

/* 初始化喇叭使用的 I2S 发送通道。 */
void audio_out_init(void);

/* 生成并播放一段正弦波测试音。 */
void audio_out_play_tone(uint16_t freq_hz, uint32_t duration_ms);

/* 软件音量范围：0-100%。 */
void audio_out_set_volume_percent(uint8_t volume_percent);
uint8_t audio_out_get_volume_percent(void);
void audio_out_volume_up(uint8_t step_percent);
void audio_out_volume_down(uint8_t step_percent);

/* 示例任务：播放一段测试音，静音一段时间，再循环。 */
void audio_out_task(void *pvParameters);

#endif
