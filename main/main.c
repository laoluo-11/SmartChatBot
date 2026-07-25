#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_out.h"
#include "button.h"
#include "led.h"
#include "mic.h"
#include "oled.h"

static const char *TAG = "main";

/* app_main 是 ESP-IDF 在 FreeRTOS 启动后的程序入口。 */
void app_main(void)
{
    /* 启动时先打印一份摘要，串口里能直接看到 PSRAM 是否可用。 */
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total == 0) {
        ESP_LOGI(TAG, "=== SmartChatBot firmware ===");
        ESP_LOGI(TAG, "No PSRAM detected, running in internal SRAM mode.");
    } else {
        ESP_LOGI(TAG, "=== SmartChatBot firmware ===");
        ESP_LOGI(TAG, "PSRAM enabled, total size: %u KB", (unsigned)(psram_total / 1024));
    }

    /* 先做一次硬件初始化，再启动各自的任务。 */
    mic_init();
    audio_out_init();
    oled_init();
    button_init();

    /* 每个功能单独一个任务，后面继续扩展会更清晰。 */
    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    xTaskCreate(mic_task, "mic", 4096, NULL, 4, NULL);
    xTaskCreate(audio_out_task, "audio_out", 4096, NULL, 4, NULL);
    xTaskCreate(oled_task, "oled", 2048, NULL, 3, NULL);
    xTaskCreate(button_task, "button", 2048, NULL, 4, NULL);
}
