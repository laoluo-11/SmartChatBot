/* =========================================================================
 * audio_out.c —— 喇叭(MAX98357A)播放模块（最小版本排查问题）
 * ========================================================================= */

#include "audio_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "audio_out";
static i2s_chan_handle_t spk_tx_chan = NULL;
static SemaphoreHandle_t s_audio_mutex = NULL;
static int g_volume = 60;   // 默认音量 60%（原 30% 太低，弱语音回放被噪声盖住）

/* 功放静音控制：仅当 SPK_SD_GPIO 配置为有效 GPIO 时生效（-1 时所有调用为空操作）。
 * 拉高=开功放(出声)，拉低=静音。用于在待机/静音时关掉 MAX98357A，消除常通电流声。 */
static inline void spk_amp_on(void)  { if (SPK_SD_GPIO >= 0) gpio_set_level(SPK_SD_GPIO, 1); }
static inline void spk_amp_off(void) { if (SPK_SD_GPIO >= 0) gpio_set_level(SPK_SD_GPIO, 0); }

/* 立体声暂存缓冲：放在 .bss（静态区），不占任务栈！
 * 之前写成 audio_out_play_pcm 里的局部数组 int16_t tmp[4096*2]（16KB），
 * 而播放任务栈仅 10KB，直接栈溢出踩穿相邻堆里的 esp_timer 控制块，
 * 一进 SPEAKING 播放就崩 StoreProhibited（timer_remove 写坏地址）。 */
#define STEREO_TMP_SAMPLES  (4096 * 2)   // 最多 4096 个单声道样本 → 8192 个 int16
static int16_t s_stereo_tmp[STEREO_TMP_SAMPLES];

#define MY_PI  3.14159265f

void audio_out_init(void)
{
    if (spk_tx_chan != NULL) {
        ESP_LOGW(TAG, "喇叭已初始化");
        return;
    }

    s_audio_mutex = xSemaphoreCreateMutex();
    if (s_audio_mutex == NULL) {
        ESP_LOGE(TAG, "mutex 创建失败");
        return;
    }

    /* 若 SD 脚接了 GPIO：配置为输出并默认拉低(静音)，由播放开始/结束控制开合，
     * 避免功放常通时的待机电流声。-1 时跳过（硬件硬接线控制）。 */
    if (SPK_SD_GPIO >= 0) {
        gpio_reset_pin(SPK_SD_GPIO);
        gpio_set_direction(SPK_SD_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(SPK_SD_GPIO, 0);   // 初始静音
        ESP_LOGI(TAG, "功放 SD 脚 GPIO%d 已配置为静音控制", SPK_SD_GPIO);
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPK_NUM, I2S_ROLE_MASTER);
    /* 【关键】auto_clear=true：DMA 欠载（流式播放时网络帧没跟上）就输出静音 0。
     * 默认 false 时欠载会【反复循环播放上一块旧数据】——这正是流式回声里
     * "电流声/嗡嗡 + 机器人抖动感"的元凶！play_tone 一次性写完整段不会欠载所以听不出，
     * 只有"边收边播"的流式路径会踩中。 */
    chan_cfg.auto_clear = true;
    /* 加大 DMA 缓冲：默认 6 块 x 240 帧（16bit立体声=4B/帧）≈90ms 余量太少，
     * 提到 8 块 x 480 帧 ≈ 240ms，网络抖动时不容易被掏空。 */
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 480;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &spk_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        /* 喇叭 MAX98357A 是标准 I2S（要 L/R 双声道帧）。用 MONO 帧播放会让真实语音被
         * "抽帧/提速"成怪声（单频测试音听不出，语音就变噪声）。这里用 STEREO，播放时把
         * 单声道样本复制到 L 和 R 两个声道（见 audio_out_play_pcm / audio_out_play_tone）。 */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws   = SPK_WS_GPIO,
            .dout = SPK_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .bclk_inv = false, .ws_inv = false },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(spk_tx_chan, &std_cfg));
    ESP_LOGI(TAG, "I2S 喇叭已配置: %d Hz", SPK_SAMPLE_RATE);
}

void audio_out_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    if (s_audio_mutex == NULL || spk_tx_chan == NULL) {
        ESP_LOGE(TAG, "喇叭未就绪");
        return;
    }

    ESP_LOGI(TAG, "=== play_tone 开始: %u Hz, %u ms, 音量=%d%% ===", freq_hz, duration_ms, g_volume);

    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "获取互斥锁超时");
        return;
    }

    /* 1) enable */
    esp_err_t err = i2s_channel_enable(spk_tx_chan);
    ESP_LOGI(TAG, "i2s_channel_enable -> %d", err);
    if (err != ESP_OK) {
        xSemaphoreGive(s_audio_mutex);
        return;
    }
    spk_amp_on();   // 开功放（SD 拉高），开始出声

    /* 2) 生成正弦波（立体声：L=R，MAX98357A 用标准 I2S 双声道帧） */
    const int sr = SPK_SAMPLE_RATE;
    const int total = sr * duration_ms / 1000;   // 单声道样本数
    ESP_LOGI(TAG, "总样本数=%d", total);
    int16_t *buf = (int16_t *)malloc((size_t)total * 2 * sizeof(int16_t));  // 立体声缓冲（L+R）
    if (!buf) {
        ESP_LOGE(TAG, "malloc 失败");
        i2s_channel_disable(spk_tx_chan);
        xSemaphoreGive(s_audio_mutex);
        return;
    }

    int amplitude = (int)(20000.0f * (float)g_volume / 100.0f);
    ESP_LOGI(TAG, "振幅=%d", amplitude);
    for (int i = 0; i < total; i++) {
        int16_t v = (int16_t)(sinf(2.0f * MY_PI * freq_hz * i / sr) * amplitude);
        buf[i * 2]     = v;   // 左声道
        buf[i * 2 + 1] = v;   // 右声道
    }

    /* 3) 写入 I2S */
    size_t offset = 0;   // 单声道样本偏移
    while (offset < (size_t)total) {
        size_t remain_samples = (size_t)total - offset;
        size_t chunk_bytes = remain_samples * 2 * sizeof(int16_t);  // 立体声字节数
        if (chunk_bytes > 8192) chunk_bytes = 8192;
        size_t written = 0;
        err = i2s_channel_write(spk_tx_chan, buf + offset * 2, chunk_bytes, &written, pdMS_TO_TICKS(2000));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write 失败 @ offset=%d: %d", (int)offset, err);
            break;
        }
        offset += written / (2 * sizeof(int16_t));
    }
    free(buf);
    ESP_LOGI(TAG, "写入完成, offset=%d/%d", (int)offset, total);

    /* 4) disable */
    spk_amp_off();  // 先静音，再关 I2S，避免末尾爆音
    err = i2s_channel_disable(spk_tx_chan);
    ESP_LOGI(TAG, "i2s_channel_disable -> %d", err);

    xSemaphoreGive(s_audio_mutex);
    ESP_LOGI(TAG, "=== play_tone 结束 ===");
}

void audio_out_task(void *pvParameters)
{
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(500));
    while (1) {
        audio_out_play_tone(1000, 1000);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void audio_out_set_volume(int vol)
{
    if (vol < AUDIO_VOL_MIN) vol = AUDIO_VOL_MIN;
    if (vol > AUDIO_VOL_MAX) vol = AUDIO_VOL_MAX;
    g_volume = vol;
    ESP_LOGI(TAG, "音量设置为 %d%%", g_volume);
}

int  audio_out_get_volume(void) { return g_volume; }
void audio_out_volume_up(void)   { audio_out_set_volume(g_volume + 10); }
void audio_out_volume_down(void) { audio_out_set_volume(g_volume - 10); }

/* -------------------------------------------------------------------------
 * L7 流式 PCM 播放：一段语音只开关一次 I2S，避免逐帧爆音
 * ------------------------------------------------------------------------- */
esp_err_t audio_out_stream_begin(void)
{
    if (s_audio_mutex == NULL || spk_tx_chan == NULL) {
        ESP_LOGE(TAG, "喇叭未就绪");
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "获取互斥锁超时");
        return ESP_FAIL;
    }
    esp_err_t err = i2s_channel_enable(spk_tx_chan);
    if (err != ESP_OK) {
        xSemaphoreGive(s_audio_mutex);
        ESP_LOGE(TAG, "I2S 使能失败 %d", err);
        return err;
    }
    spk_amp_on();   // 开功放
    return ESP_OK;
}

esp_err_t audio_out_play_pcm(const int16_t *pcm, size_t samples)
{
    if (spk_tx_chan == NULL || pcm == NULL || samples == 0) return ESP_FAIL;
    if (s_audio_mutex == NULL) return ESP_FAIL;   // 必须先 stream_begin 拿到锁

    int vol = g_volume;
    size_t offset = 0;
    /* 分块写，单块不超过 4K 单声道样本；立体声输出时每个样本复制成 L/R 两路。
     * 暂存缓冲用静态区 s_stereo_tmp，避免栈上开大数组导致栈溢出。 */
    int16_t *tmp = s_stereo_tmp;
    while (offset < samples) {
        size_t chunk = samples - offset;
        if (chunk > 4096) chunk = 4096;
        for (size_t i = 0; i < chunk; i++) {
            int32_t s = (int32_t)pcm[offset + i] * vol / 100;   // 按当前音量缩放
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            tmp[i * 2]     = (int16_t)s;   // 左
            tmp[i * 2 + 1] = (int16_t)s;   // 右
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(spk_tx_chan, tmp,
                                          chunk * 2 * sizeof(int16_t),
                                          &written, pdMS_TO_TICKS(2000));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "audio_out_play_pcm 写入失败 @%d: %d", (int)offset, err);
            break;
        }
        offset += written / (2 * sizeof(int16_t));
    }
    return ESP_OK;
}

esp_err_t audio_out_stream_end(void)
{
    spk_amp_off();  // 先静音
    if (spk_tx_chan) i2s_channel_disable(spk_tx_chan);
    if (s_audio_mutex) xSemaphoreGive(s_audio_mutex);
    return ESP_OK;
}
