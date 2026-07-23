/* =========================================================================
 * mic.h —— 麦克风(INMP441)采集模块的"对外说明书"
 * -------------------------------------------------------------------------
 * 对外只暴露两样东西：
 *   1) mic_init()  ：初始化 I2S 麦克风（建通道 + 配置 + 启用）。app_main 里先调用它。
 *   2) mic_task()  ：麦克风采集任务，由 app_main 创建成 FreeRTOS 任务后自动循环跑。
 *
 * 配置宏（引脚、采样率）也放这里，方便你在一个地方改接线。
 * ========================================================================= */
#ifndef MIC_H
#define MIC_H

#include "driver/i2s_std.h"   // 为了能使用 I2S_NUM_0 等类型/宏（函数原型里会用到）

/* ===== I2S 数字麦克风 INMP441 的接线与参数：按你手里的接线改这里 ===== */
#define I2S_MIC_NUM      I2S_NUM_0   // ESP32-S3 有 I2S0 / I2S1 两个控制器，麦克风用第 0 个
#define MIC_BCLK_GPIO    5           // INMP441 的 SCK 脚 → ESP32 的 GPIO5（位时钟，ESP 输出）
#define MIC_WS_GPIO      4           // INMP441 的 WS  脚 → ESP32 的 GPIO4（声道选择，ESP 输出）
#define MIC_DIN_GPIO     6           // INMP441 的 SD  脚 → ESP32 的 GPIO6（数据，ESP 输入）
#define SAMPLE_RATE      16000       // 采样率 16000Hz（每秒采 16000 次），语音够用且省带宽
#define MIC_READ_SAMPLES 512         // 每次从麦克风读 512 个样本，每个 16bit=2字节 → 共 1KB

/* 初始化 I2S 麦克风通道。必须在创建 mic_task 之前调用一次。 */
void mic_init(void);

/* 麦克风采集任务：循环读声音、算 RMS 音量、打印到串口。 */
void mic_task(void *pvParameters);

/* 获取最近一次 RMS 音量值（状态机用此判断有无声音） */
float mic_get_rms(void);

/* ---- L7 新增：采集缓冲（LISTENING 期间把 PCM 暂存，供上传服务器）---- */
/* 开始采集：从这一刻起把麦克风 PCM 存进缓冲（最多 CAPTURE_SECONDS 秒）。
 * 重复调用会重置缓冲、从头记。 */
void mic_capture_start(void);

/* 停止采集：缓冲定格，可安全读取。 */
void mic_capture_stop(void);

/* 取出已采集的 PCM（samples 个 int16 样本）。返回的指针在下次 mic_capture_start 前有效。
 * 返回 NULL 表示没采到东西。 */
const int16_t *mic_capture_get(size_t *samples);

#endif /* MIC_H */
