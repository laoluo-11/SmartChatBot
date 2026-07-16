/* =========================================================================
 * main.c —— 程序入口（第 3 关 L3：叠加 MAX98357A 喇叭 I2S 播放）
 * -------------------------------------------------------------------------
 * 拆分后，main.c 变得很薄，只负责几件事：
 *   1) 探测板子有没有 PSRAM，打印启动信息
 *   2) 调用各模块的初始化函数（mic_init / audio_out_init）
 *   3) 创建 FreeRTOS 任务，让 LED、麦克风、喇叭同时工作
 *
 * 具体的硬件操作都搬到了各自的模块文件：
 *   - led.c / led.h        ：心跳灯
 *   - mic.c / mic.h        ：麦克风采集（含 I2S 接收初始化）
 *   - audio_out.c / .h     ：喇叭播放（含 I2S 发送初始化）
 * 以后加屏幕、WiFi 等，也是"加一对文件 + 在 main.c 里调一下"即可。
 *
 * 给初学者的小知识：
 *   - app_main() 是 ESP32 程序的总入口，相当于电脑程序的 main()。
 *     芯片上电、FreeRTOS 启动完后，会自动调用它。
 *   - "任务(Task)"是 FreeRTOS 里可以同时跑的多个小程序；xTaskCreate 把它们启动起来。
 * ========================================================================= */

#include <stdio.h>              // 标准输入输出（习惯带上）
#include "freertos/FreeRTOS.h" // FreeRTOS 内核
#include "freertos/task.h"     // 任务创建 xTaskCreate
#include "esp_log.h"           // 日志打印
#include "esp_heap_caps.h"     // 内存能力查询：探测板子有没有 PSRAM
#include "led.h"               // 引入 LED 模块（拿到 led_task 声明）
#include "mic.h"               // 引入麦克风模块（拿到 mic_init / mic_task 声明）
#include "audio_out.h"          // 引入喇叭模块（拿到 audio_out_init / audio_out_task 声明）

static const char *TAG = "main";  // 本文件日志标签："main: ..."（入口相关的日志归这里）

/* -------------------------------------------------------------------------
 * app_main：程序入口
 * ------------------------------------------------------------------------- */
void app_main(void)
{
    /* 第一步：探测板子到底有没有 PSRAM（外部大内存）。
     * heap_caps_get_total_size(MALLOC_CAP_SPIRAM) 返回 PSRAM 总容量（字节）。
     * 你的板子是真 N16R8，所以这里会 > 0，走 else 分支。
     * 保留 if/else 两套提示，是为了以后万一换到没有 PSRAM 的板子，程序也能说明情况。 */
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total == 0) {
        ESP_LOGI(TAG, "=== 语音聊天机器人 固件 (L2 麦克风) ===");
        ESP_LOGI(TAG, "[模式] 本板无 PSRAM，运行于内部 SRAM 模式：音频用小缓冲，"
                      "离线大模型/multinet 不可用，在线流式对话不受影响。");
    } else {
        ESP_LOGI(TAG, "=== 语音聊天机器人 固件 (L2 麦克风) ===");
        ESP_LOGI(TAG, "PSRAM 已启用，总容量=%u KB", (unsigned)(psram_total / 1024));  // 字节转 KB 打印
    }

    /* 第二步：初始化硬件模块。
     * 麦克风需要先"打开"(mic_init) 再交给任务去循环采集。LED 很简单，任务里自己会配置 GPIO。 */
    mic_init();        // 打开并配置 I2S 麦克风通道（建通道 + 配置引脚 + 启用）
    audio_out_init();  // 打开并配置 I2S 喇叭发送通道（建通道 + 配置引脚 + 启用）

    /* 第三步：创建任务，让 LED 心跳 和 麦克风采集 并行工作 */
    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);  // 任务函数、名字、栈大小(2KB)、参数(NULL)、优先级(5)、句柄(NULL)
    xTaskCreate(mic_task, "mic", 4096, NULL, 4, NULL);  // 麦克风任务栈大一点(4KB)，因为里面有缓冲区
    xTaskCreate(audio_out_task, "spk", 4096, NULL, 3, NULL); // 喇叭播放任务（栈 4KB 存正弦波缓冲）
}
