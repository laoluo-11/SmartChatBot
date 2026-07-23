/* =========================================================================
 * opus_codec.c —— 音频编解码模块的"具体实现"（L7 新增）
 * -------------------------------------------------------------------------
 * 见 opus_codec.h 头部说明：装了 esp-opus 组件用真 Opus，否则 PCM 透传。
 * 两种模式对外接口完全一致，调用方（comm.c）无需关心当前是哪种。
 * ========================================================================= */

#include "opus_codec.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "opus";

#if USE_OPUS
  #include "esp_opus_encoder.h"
  #include "esp_opus_decoder.h"
  static esp_opus_encoder_handle_t s_enc = NULL;   // Opus 编码器句柄
  static esp_opus_decoder_handle_t s_dec = NULL;   // Opus 解码器句柄
#endif

esp_err_t opus_codec_init(void)
{
#if USE_OPUS
    /* ---- 编码器：16kHz / 单声道 / 24kbps ---- */
    esp_opus_encoder_cfg_t ecfg = ESP_OPUS_ENCODER_CONFIG_DEFAULT();
    ecfg.sample_rate = 16000;
    ecfg.channel     = 1;
    ecfg.bitrate     = 24000;
    if (esp_opus_encoder_create(&ecfg, &s_enc) != ESP_OK) {
        ESP_LOGE(TAG, "Opus 编码器创建失败");
        return ESP_FAIL;
    }

    /* ---- 解码器：16kHz / 单声道 ---- */
    esp_opus_decoder_cfg_t dcfg = ESP_OPUS_DECODER_CONFIG_DEFAULT();
    dcfg.sample_rate = 16000;
    dcfg.channel     = 1;
    if (esp_opus_decoder_create(&dcfg, &s_dec) != ESP_OK) {
        ESP_LOGE(TAG, "Opus 解码器创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opus 编解码已启用（真 Opus，16kHz/单声道/24kbps）");
#else
    ESP_LOGW(TAG, "未安装 esp-opus 组件 -> 使用 PCM 透传（带宽大，仅用于本地联调；"
                 "装组件后自动切真 Opus）");
#endif
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * 编码一帧（960 样本 = 20ms）
 * ------------------------------------------------------------------------- */
int opus_encode_frame(const int16_t *pcm, uint8_t *out, int out_cap)
{
#if USE_OPUS
    if (!s_enc) return -1;
    int out_len = 0;
    /* 参数顺序：句柄, PCM输入, 输入字节数, 输出缓冲, 输出字节数(出参)
     * 若你的组件头文件里第二个参数是"样本数"而非"字节数"，按头文件改这一行即可。 */
    esp_opus_encoder_process(s_enc, pcm,
                             OPUS_FRAME_SAMPLES * (int)sizeof(int16_t),
                             out, &out_len);
    return out_len;
#else
    int n = OPUS_FRAME_SAMPLES * (int)sizeof(int16_t);
    if (n > out_cap) n = out_cap;
    memcpy(out, pcm, (size_t)n);    // 透传：原样拷贝 PCM 字节
    return n;
#endif
}

/* -------------------------------------------------------------------------
 * 解码一段音频包 → 最多 960 样本
 * ------------------------------------------------------------------------- */
int opus_decode_packet(const uint8_t *data, int len, int16_t *pcm, int pcm_cap)
{
#if USE_OPUS
    if (!s_dec) return -1;
    int pcm_len = 0;   // 字节数（出参）
    esp_opus_decoder_process(s_dec, data, len, pcm, &pcm_len);
    return pcm_len / (int)sizeof(int16_t);   // 转成"样本数"返回
#else
    int samples = len / (int)sizeof(int16_t);
    int cap_samples = pcm_cap / (int)sizeof(int16_t);
    if (samples > cap_samples) samples = cap_samples;
    memcpy(pcm, data, (size_t)samples * sizeof(int16_t));
    return samples;
#endif
}
