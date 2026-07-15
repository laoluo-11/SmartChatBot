/* =========================================================================
 * led.c —— 板载 RGB LED 心跳灯模块的"具体实现"
 * -------------------------------------------------------------------------
 * 重要：板载 RGB 灯是"智能寻址灯"（WS2812 / SK68xx 一类），它和普通的 LED 完全不同：
 *   - 普通 LED：一个 GPIO 高低电平就能亮灭（gpio_set_level）。
 *   - 板载 RGB 灯：单根数据线，靠一串精确的时序脉冲来传"颜色数据"，
 *     必须用专门的 led_strip 驱动（底层用 RMT 外设生成时序），不能直接 gpio_set_level。
 *   所以本文件用 led_strip 组件，而不是 driver/gpio。
 *
 * 心跳效果：亮 500ms（指定颜色）→ 灭 500ms，循环，让你一眼确认"系统还活着"。
 * ========================================================================= */

#include "led.h"               // 引入本模块声明（RGB_LED_GPIO / 颜色宏、led_task 原型）
#include "freertos/FreeRTOS.h" // FreeRTOS 内核（vTaskDelay 延时用）
#include "freertos/task.h"     // 任务相关（vTaskDelay）
#include "esp_log.h"           // 日志打印（ESP_LOGI 用 TAG 输出"板载 RGB LED 已就绪"）
#include "led_strip.h"         // 板载 RGB 寻址灯驱动（基于 RMT 时序协议）

static const char *TAG = "led";  // 本模块日志标签：串口里会显示成 "led: ..."，方便区分是哪个模块的日志

/* 板载 RGB 灯句柄：led_strip 库用它代表"那一串灯"，先置空，初始化后再指向真实对象 */
static led_strip_handle_t g_strip = NULL;

/* 心跳灯任务本体 */
void led_task(void *pvParameters)
{
    (void)pvParameters;   // 本任务不接收外部参数，这行只是消除"未使用参数"的编译警告

    /* 1) 配置灯带参数：板载只有 1 颗灯，型号 WS2812，颜色顺序 GRB（WS2812/SK68xx 标准） */
    led_strip_config_t strip_config = {
        .strip_gpio_num         = RGB_LED_GPIO,                    // 数据脚：板载 RGB 灯接的 GPIO
        .max_leds               = 1,                               // 板载是单颗灯
        .led_model              = LED_MODEL_WS2812,                // 灯珠型号
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // 颜色顺序 GRB（不是 RGB！）
        .flags.invert_out       = false,                           // 不反转输出电平
    };

    /* 2) RMT 配置：驱动自动分配 RMT 通道；10MHz 时序精度足够（clk_src 留默认 0 即可） */
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz（设 0 则用默认 10MHz）
        .flags.with_dma = 0,                // 不用 DMA（单颗灯数据量极小）
    };

    /* 3) 创建灯带对象并启用。失败就让程序停（初始化是前提，用 ESP_ERROR_CHECK） */
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &g_strip));
    ESP_LOGI(TAG, "板载 RGB LED 已就绪 (GPIO%d)", RGB_LED_GPIO);

    /* while (1) 无限循环：任务一旦启动就一直跑（这正是"心跳"想要的） */
    while (1) {
        /* 点亮：给第 0 颗灯设置颜色，再 refresh 把数据发过去。
         * 颜色分量是 0-255，这里用 led.h 里定义的青绿色。 */
        ESP_ERROR_CHECK(led_strip_set_pixel(g_strip, 0, RGB_LED_R, RGB_LED_G, RGB_LED_B));
        ESP_ERROR_CHECK(led_strip_refresh(g_strip));
        vTaskDelay(pdMS_TO_TICKS(500));                      // 亮 500 毫秒

        /* 熄灭：clear 把所有灯清成 0 并刷新（比重新 set 全 0 更省事） */
        ESP_ERROR_CHECK(led_strip_clear(g_strip));
        vTaskDelay(pdMS_TO_TICKS(500));                      // 灭 500 毫秒 → 合计 1 秒闪一次
    }
}
