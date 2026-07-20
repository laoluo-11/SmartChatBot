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

static const char *TAG = "audio_out";
static i2s_chan_handle_t spk_tx_chan = NULL;
static SemaphoreHandle_t s_audio_mutex = NULL;
static int g_volume = 30;

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

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPK_NUM, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &spk_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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

    /* 2) 生成正弦波 */
    const int sr = SPK_SAMPLE_RATE;
    const int total = sr * duration_ms / 1000;
    ESP_LOGI(TAG, "总样本数=%d", total);
    int16_t *buf = (int16_t *)malloc(total * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "malloc 失败");
        i2s_channel_disable(spk_tx_chan);
        xSemaphoreGive(s_audio_mutex);
        return;
    }

    int amplitude = (int)(20000.0f * (float)g_volume / 100.0f);
    ESP_LOGI(TAG, "振幅=%d", amplitude);
    for (int i = 0; i < total; i++) {
        buf[i] = (int16_t)(sinf(2.0f * MY_PI * freq_hz * i / sr) * amplitude);
    }

    /* 3) 写入 I2S */
    size_t offset = 0;
    while (offset < (size_t)total) {
        size_t chunk = (total - offset) * sizeof(int16_t);
        if (chunk > 8192) chunk = 8192;
        size_t written = 0;
        err = i2s_channel_write(spk_tx_chan, buf + offset, chunk, &written, pdMS_TO_TICKS(2000));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write 失败 @ offset=%d: %d", (int)offset, err);
            break;
        }
        offset += written / sizeof(int16_t);
    }
    free(buf);
    ESP_LOGI(TAG, "写入完成, offset=%d/%d", (int)offset, total);

    /* 4) disable */
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
