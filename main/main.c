/* =========================================================================
 * 语音聊天机器人固件 · 第 2 关 (L2)：INMP441 麦克风 I2S 采集
 * -------------------------------------------------------------------------
 * 在 L1(LED 心跳)基础上叠加：
 *   - led_task    ：心跳灯（保留）
 *   - mic_task    ：I2S 读 INMP441 PCM，计算 RMS 音量并打印（有声音才打）
 *
 * 硬件前提：本板为 N16R8（已确认含 8MB Octal PSRAM；ESP-IDF v6 必须在 menuconfig
 *           选 Octal/OPI 类型才能识别，残留 TYPE_QUAD 会导致探测失败）。
 *           L2 麦克风先用小缓冲（512 样本）放内部 SRAM（普通 malloc）足够；
 *           后续需要大缓冲 / 离线 multinet / WiFi-Opus 时再用 MALLOC_CAP_SPIRAM。
 * ========================================================================= */
 


#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"  //任务函数（xTaskCreate / vTaskDelete / vTaskDelay）
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "led.h"
#include "mic.h" 



static const char *TAG = "main";
void app_main(void)
{
    //先检测板子是否有PSRAM
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if(psram_total==0){
        ESP_LOGI(TAG, "=== 语音聊天机器人 固件 (L2 麦克风) ===");
        ESP_LOGI(TAG, "[模式] 本板无 PSRAM，运行于内部 SRAM 模式：音频用小缓冲，"
                      "离线大模型/multinet 不可用，在线流式对话不受影响。");
    }
    else {
        ESP_LOGI(TAG, "=== 语音聊天机器人 固件 (L2 麦克风) ===");
        ESP_LOGI(TAG, "PSRAM 已启用，总容量=%u KB", (unsigned)(psram_total / 1024));  // 字节转 KB 打印
    }

    /* 第二步：初始化硬件模块。
     * 麦克风需要先"打开"(mic_init) 再交给任务去循环采集。LED 很简单，任务里自己会配置 GPIO。 */
    mic_init();   // 打开并配置 I2S 麦克风通道（建通道 + 配置引脚 + 启用）

    /* 第三步：创建任务，让 LED 心跳 和 麦克风采集 并行工作 */
  
     xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);  // 任务函数、名字、栈大小(2KB)、参数(NULL)、优先级(5)、句柄(NULL)
    xTaskCreate(mic_task, "mic", 4096, NULL, 4, NULL);  // 麦克风任务栈大一点(4KB)，因为里面有缓冲区



}
