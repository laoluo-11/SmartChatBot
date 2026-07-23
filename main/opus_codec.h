/* =========================================================================
 * opus_codec.h —— 音频编解码模块（L7 新增）
 * -------------------------------------------------------------------------
 * 干两件事：
 *   1) opus_encode_frame()  ：把 20ms 的 PCM（960 个 int16 样本）压成一小段 Opus
 *   2) opus_decode_packet() ：把一小段 Opus 解回 20ms 的 PCM
 *
 * 为什么用 Opus：16kHz 单声道 PCM 原始码率 256kbps，Opus 压到 20~32kbps，
 * 省了 8~10 倍带宽，手机/服务器传输才扛得住。
 *
 * 两种运行模式（自动切换，你不用改代码）：
 *   - 装了 Espressif 的 esp-opus 组件  ->  用"真 Opus"（小、省带宽）
 *   - 没装                              ->  用"PCM 透传"（大、但能先跑通联调）
 * 装组件命令（终端执行）：
 *   idf.py add-dependency "espressif/esp-opus-encoder"
 *   idf.py add-dependency "espressif/esp-opus-decoder"
 * 装完重新编译即可自动切到真 Opus（靠 __has_include 探测头文件是否存在）。
 *
 * 注意：esp-opus 组件的精确 API（参数名/顺序）请以你机器上
 *   managed_components/espressif__esp-opus-<版本>/include/esp_opus_encoder.h
 *   里的真实声明为准；若编译报参数不匹配，按头文件微调几行即可。
 * ========================================================================= */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Opus 编码帧：16kHz 单声道，20ms = 960 个样本（libopus / ESP-ADF 标准帧长） */
#define OPUS_FRAME_SAMPLES  960
#define OPUS_FRAME_MS       20

/* 音频帧里第 1 字节的"编解码格式"标识 */
#define CODEC_OPUS  0x01   // 负载是 Opus 压缩包
#define CODEC_PCM   0x02   // 负载是原始 int16 PCM（没装 Opus 组件时的兜底）

/* 自动探测：装了 esp_opus 编码器+解码器头文件就用真 Opus，否则 PCM 透传。
 * 你也可以用 -DUSE_OPUS=0/1 在 menuconfig/CMake 里强制覆盖。 */
#ifndef USE_OPUS
  #if defined(__has_include)
    #if __has_include("esp_opus_encoder.h") && __has_include("esp_opus_decoder.h")
      #define USE_OPUS 1
    #else
      #define USE_OPUS 0
    #endif
  #else
    #define USE_OPUS 0
  #endif
#endif

/* 初始化编码器与解码器（只需一次，在 app_main 里调）。返回 ESP_OK 表示就绪。 */
esp_err_t opus_codec_init(void);

/* 编码"正好一帧"PCM（必须 OPUS_FRAME_SAMPLES 个样本）→ 输出一段音频包。
 * 返回写入 out 的字节数（>0 成功，<=0 失败）。out_cap 至少要 2*OPUS_FRAME_SAMPLES+16。 */
int opus_encode_frame(const int16_t *pcm, uint8_t *out, int out_cap);

/* 解码一段音频包（与 opus_encode_frame 一一对应）→ 输出最多 OPUS_FRAME_SAMPLES 个样本。
 * 返回写入 pcm 的样本数（>0 成功，<=0 失败）。pcm_cap 至少要 OPUS_FRAME_SAMPLES*sizeof(int16_t)。 */
int opus_decode_packet(const uint8_t *data, int len, int16_t *pcm, int pcm_cap);
