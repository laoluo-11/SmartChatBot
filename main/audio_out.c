#include "audio_out.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 喇叭输出走 I2S1。 */
static const char *TAG = "audio_out";
static i2s_chan_handle_t spk_tx_chan = NULL;
static volatile uint8_t s_volume_percent = 25;

#define MY_PI 3.14159265f
/* 留一点幅度余量，避免测试音在 100% 音量时过早失真。 */
#define SPK_SOFT_VOLUME_MAX_AMPLITUDE 12000.0f

static uint8_t audio_out_clamp_volume(uint8_t volume_percent)
{
    return (volume_percent > 100U) ? 100U : volume_percent;
}

void audio_out_init(void)
{
    /* 为 MAX98357A 创建一个只发送的 I2S 通道。 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPK_NUM, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &spk_tx_chan, NULL));

    /* 标准 Philips I2S，16 位单声道，和当前接线保持一致。 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws = SPK_WS_GPIO,
            .dout = SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(spk_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(spk_tx_chan));

    ESP_LOGI(TAG, "I2S speaker ready: %d Hz, 16-bit, mono", SPK_SAMPLE_RATE);
    ESP_LOGI(TAG, "Soft volume default: %u%%", s_volume_percent);
}

void audio_out_set_volume_percent(uint8_t volume_percent)
{
    s_volume_percent = audio_out_clamp_volume(volume_percent);
    ESP_LOGI(TAG, "Volume set to %u%%", s_volume_percent);
}

uint8_t audio_out_get_volume_percent(void)
{
    return s_volume_percent;
}

void audio_out_volume_up(uint8_t step_percent)
{
    int target = (int)audio_out_get_volume_percent() + (int)step_percent;
    if (target > 100) {
        target = 100;
    }
    audio_out_set_volume_percent((uint8_t)target);
}

void audio_out_volume_down(uint8_t step_percent)
{
    int target = (int)audio_out_get_volume_percent() - (int)step_percent;
    if (target < 0) {
        target = 0;
    }
    audio_out_set_volume_percent((uint8_t)target);
}

void audio_out_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    if (spk_tx_chan == NULL) {
        ESP_LOGE(TAG, "Speaker not initialized");
        return;
    }

    const int sr = SPK_SAMPLE_RATE;
    const int total = sr * (int)duration_ms / 1000;
    int16_t *buf = (int16_t *)malloc((size_t)total * sizeof(int16_t));
    if (buf == NULL) {
        ESP_LOGE(TAG, "Audio buffer malloc failed");
        return;
    }

    /* 将当前音量直接体现在生成出来的波形振幅上。 */
    const float amplitude =
        SPK_SOFT_VOLUME_MAX_AMPLITUDE * ((float)audio_out_get_volume_percent() / 100.0f);

    for (int i = 0; i < total; ++i) {
        buf[i] = (int16_t)(sinf(2.0f * MY_PI * (float)freq_hz * (float)i / (float)sr) * amplitude);
    }

    size_t offset = 0;
    while (offset < (size_t)total) {
        /* 分块写入，避免大缓冲区完全依赖单次 DMA 传输。 */
        size_t chunk = ((size_t)total - offset) * sizeof(int16_t);
        if (chunk > 4096U) {
            chunk = 4096U;
        }

        size_t written = 0;
        esp_err_t ret = i2s_channel_write(
            spk_tx_chan,
            buf + offset,
            chunk,
            &written,
            pdMS_TO_TICKS(1000));

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %d", ret);
            break;
        }

        offset += written / sizeof(int16_t);
    }

    free(buf);
    ESP_LOGI(TAG, "Tone done: %u Hz, %u ms", freq_hz, duration_ms);
}

void audio_out_task(void *pvParameters)
{
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        /* 项目开发阶段先保留一个可听见的“心跳音”方便确认喇叭工作正常。 */
        audio_out_play_tone(1000, 1000);
        i2s_channel_disable(spk_tx_chan);
        vTaskDelay(pdMS_TO_TICKS(2000));
        i2s_channel_enable(spk_tx_chan);
    }
}
