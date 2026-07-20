/* =========================================================================
 * mic.c —— 麦克风(INMP441)采集模块的"具体实现"
 * -------------------------------------------------------------------------
 * 负责两件事：
 *   1) mic_init()  ：配置并启动 I2S 接收通道（相当于"打开麦克风"）
 *   2) mic_task()  ：不停地读声音数据，算出一个"音量大小"(RMS) 并打印
 *
 * 给初学者的小知识：
 *   - INMP441 是"数字麦克风"，用 I2S 这种高速数字总线传声音，有 3 根关键线：
 *       BCLK（节拍时钟）、WS（左右声道选择）、DIN（数据，ESP32 接收）。
 *     ESP32 是"主"，负责发时钟，麦克风跟着走。
 *   - RMS（均方根）是衡量"音量大小"的常用指标：把每个样本平方→求平均→开根号。
 * ========================================================================= */

#include "mic.h"               // 本模块声明（引脚宏、mic_init/mic_task 原型）
#include <stdio.h>            // 标准输入输出（习惯带上）
#include <stdlib.h>           // malloc / free
#include <math.h>             // 数学库，用 sqrtf() 开平方根算 RMS
#include "freertos/FreeRTOS.h"// FreeRTOS 内核
#include "freertos/task.h"    // 任务函数（xTaskCreate / vTaskDelete / vTaskDelay）
#include "esp_log.h"          // 日志打印 ESP_LOGI / ESP_LOGE

/* 模块内部的"私有变量"：static 表示只在 mic.c 里可见，别的文件碰不到。
 * 这样可以避免不同模块之间变量名互相打架。 */
static const char *TAG = "mic";                 // 本模块日志标签："mic: ..."
static i2s_chan_handle_t mic_rx_chan = NULL;    // I2S 接收通道句柄（想象成麦克风的遥控器），先置空
static float g_mic_rms = 0.0f;                   // 最近一次 RMS 值（状态机读取用）

/* -------------------------------------------------------------------------
 * mic_init：打开并配置麦克风（只调用一次）
 * ------------------------------------------------------------------------- */
void mic_init(void)
{
    if (mic_rx_chan != NULL) {
        ESP_LOGW(TAG, "麦克风已初始化，跳过重复初始化");
        return;
    }

    /* 1) 通道配置：用哪个 I2S 控制器、ESP32 当主设备 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_NUM, I2S_ROLE_MASTER);

    /* 2) 创建通道：只传 RX（接收）句柄，TX（发送）填 NULL → "只要接收，不要发送"。
     *    麦克风只负责把声音送进来；往外发声音是后面 L3 喇叭的事。 */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &mic_rx_chan)); // 仅 RX

    /* 3) 配置 I2S 的"标准模式"参数（时钟 / 声道 / 引脚） */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),   // 时钟：用上面定义的 16000Hz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        // 声道：每样本 16bit（标准精度），单声道 MONO（麦克风只有一路，不需要左右声道）
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // 主时钟 MCLK：INMP441 不需要，标记"不用"
            .bclk = MIC_BCLK_GPIO,     // 位时钟 → GPIO5（接麦克风 SCK）
            .ws   = MIC_WS_GPIO,       // 声道选择 → GPIO4（接麦克风 WS）
            .dout = I2S_GPIO_UNUSED,   // 数据输出：麦克风是输入设备，无输出，标记"不用"
            .din  = MIC_DIN_GPIO,      // 数据输入 → GPIO6（接麦克风 SD，声音从这里进来）
            .invert_flags = {
                .bclk_inv = false,     // 不反转时钟极性（标准接法保持默认）
                .ws_inv   = false,     // 不反转声道极性
            },
        },
    };

    /* 4) 把配置应用到通道。
     * 【初学者注意】ESP-IDF v6 把原来的 i2s_channel_init_std_rx / _tx 合并成
     *    i2s_channel_init_std_mode()，方向(RX/TX)由前面 i2s_new_channel 传的句柄决定，
     *    不再靠函数名区分。所以这里不写 _rx，直接写 _mode。 */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_rx_chan));  // 5) 启用通道：正式开始"听"

    ESP_LOGI(TAG, "I2S 麦克风已启动: %d Hz, 16bit, MONO", SAMPLE_RATE);  // 打印确认信息
}

/* -------------------------------------------------------------------------
 * mic_task：采集任务（被 FreeRTOS 反复调度，无限循环）
 * ------------------------------------------------------------------------- */
void mic_task(void *pvParameters)
{
    (void)pvParameters;   // 本任务不接收外部参数，消除"未使用参数"警告

    /* 申请一块内存当"缓冲区"，临时存放从麦克风读出的声音数据。
     * malloc = 动态申请内存。512 个 int16_t（每个 2 字节）→ 约 1KB。
     * 这块很小，用芯片内部 SRAM 就够，普通 malloc 即可（不需动用外部 PSRAM）。 */
    int16_t *buf = (int16_t *)malloc(MIC_READ_SAMPLES * sizeof(int16_t));
    if (!buf) {   // 万一内存申请失败（返回 NULL），兜底退出，不能让程序崩
        ESP_LOGE(TAG, "麦克风缓冲 malloc 失败，任务退出");
        vTaskDelete(NULL);  // 删除自己这个任务，安全退出
        return;
    }

    size_t bytes_read = 0;   // 记录"这次实际读到了多少字节"
    uint32_t cnt = 0;        // 计数器：控制"大约每 1 秒打印一次"，避免刷屏

    /* 主循环：不停地读麦克风 */
    while (1) {
        /* i2s_channel_read：从麦克风通道读数据到 buf。
         * 参数：通道句柄、目标缓冲、想读的最大字节数、实际读到的字节数(出参)、超时(1秒) */
        esp_err_t r = i2s_channel_read(mic_rx_chan, buf,
                                       MIC_READ_SAMPLES * sizeof(int16_t),
                                       &bytes_read, pdMS_TO_TICKS(1000));
        if (r == ESP_OK && bytes_read > 0) {   // 读成功且确实读到了数据
            int samples = (int)(bytes_read / sizeof(int16_t));  // 把"字节数"换算成"样本数"

            /* --- 计算 RMS（均方根）---
             * 声音是上下波动的波形，单个样本有正有负，直接相加会抵消成 0。
             * 做法：每个样本先平方（变正数）→ 全部相加 → 除以样本数求平均 → 开平方根。
             * 结果 RMS 越大，说明声音越响。 */
            int64_t sum = 0;                    // 用 int64 累加，防止平方和溢出
            for (int i = 0; i < samples; i++) {
                int32_t s = buf[i];             // 取第 i 个样本（16bit 有符号，范围约 -32768~32767）
                sum += (int64_t)s * s;          // 平方后累加
            }
            float rms = sqrtf((float)((double)sum / samples));  // 平均后再开根号 = RMS
            g_mic_rms = rms;  // 更新全局 RMS 供状态机读取

            /* --- 大约每 1 秒打印一次音量仪表（即使安静也打印，方便你观察对比）---
             * 512 样本 @16kHz 只够 32 毫秒，循环很快；数 32 次循环（≈1秒）才打一行，不刷屏。
             *
             * 三种状态对照（接不接麦、说没说话，看 RMS 一眼分辨）：
             *   悬空(没接麦)  → RMS 很高且乱跳（几千），一直"有声音"
             *   接麦且安静     → RMS 接近 0，显示"安静/基线"
             *   接麦且说话     → RMS 明显升高，显示"有声音"
             * 注意：下面 200 只是用来贴"有声音"标签，不是判断接没接麦的标准。 */
            if ((++cnt % 32) == 0) {   // ++cnt 先自增，再对 32 取余；余 0 说明凑满 32 次
                const char *state = (rms > 200.0f) ? "有声音" : "安静/基线";  // 三目运算贴标签
                ESP_LOGI(TAG, "mic RMS=%5.0f | %s", rms, state);  // %5.0f = 留 5 位宽、不要小数
            }
        }
    }
}

float mic_get_rms(void)
{
    return g_mic_rms;
}
