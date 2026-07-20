/* =========================================================================
 * led.c —— 板载 RGB LED 模块的"具体实现"
 * -------------------------------------------------------------------------
 * 重要：板载 RGB 灯是"智能寻址灯"（WS2812 / SK68xx 一类），它和普通的 LED 完全不同：
 *   - 普通 LED：一个 GPIO 高低电平就能亮灭（gpio_set_level）。
 *   - 板载 RGB 灯：单根数据线，靠一串精确的时序脉冲来传"颜色数据"，
 *     必须用专门的 led_strip 驱动（底层用 RMT 外设生成时序），不能直接 gpio_set_level。
 *   所以本文件用 led_strip 组件，而不是 driver/gpio。
 *
 * 在 L5 之后，灯的颜色由"状态机"驱动：
 *   空闲(IDLE)=灭、聆听(LISTENING)=蓝、思考(THINKING)=紫、说话(SPEAKING)=绿。
 * 所以这里只提供 led_init（建灯）+ led_set_color（设色）+ led_off（熄灭）三个接口，
 * 不再有"心跳"任务——颜色随状态实时变，一眼能看出机器人在干嘛。
 * ========================================================================= */

#include "led.h"               // 本模块声明（RGB_LED_GPIO、led_init/led_set_color/led_off 原型）
#include "freertos/FreeRTOS.h" // FreeRTOS 内核
#include "esp_log.h"           // 日志打印（ESP_LOGI）
#include "led_strip.h"         // 板载 RGB 寻址灯驱动（基于 RMT 时序协议）

static const char *TAG = "led";  // 日志标签："led: ..."，方便区分是哪个模块的日志

/* 板载 RGB 灯句柄：led_strip 用这个句柄代表"那一串灯"，先置空，初始化后再指向真实对象 */
static led_strip_handle_t g_strip = NULL;

/* -------------------------------------------------------------------------
 * led_init：初始化板载 RGB 灯（只调一次）
 * 把灯带的创建从原来的"心跳任务"里抽出来，变成独立的初始化函数。
 * 这样状态机在任何时候都能调用 led_set_color 去设颜色，而不必等一个任务在跑。
 * ------------------------------------------------------------------------- */
void led_init(void)
{
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
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
        .flags.with_dma = 0,                // 不用 DMA（单颗灯数据量极小）
    };

    /* 3) 创建灯带对象并启用。失败就让程序停（初始化是前提，用 ESP_ERROR_CHECK） */
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &g_strip));
    ESP_LOGI(TAG, "板载 RGB LED 已就绪 (GPIO%d)", RGB_LED_GPIO);
}

/* -------------------------------------------------------------------------
 * led_set_color：把板载 RGB 灯设成指定颜色
 * ------------------------------------------------------------------------- */
void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (g_strip == NULL) {
        return;   // 防御：灯还没初始化（led_init 没调）就别设，避免空指针
    }
    /* set_pixel 把第 0 颗灯设成 (r,g,b)；refresh 把数据真正发出去亮灯 */
    ESP_ERROR_CHECK(led_strip_set_pixel(g_strip, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(g_strip));
}

/* -------------------------------------------------------------------------
 * led_off：熄灭板载 RGB 灯
 * ------------------------------------------------------------------------- */
void led_off(void)
{
    if (g_strip == NULL) {
        return;
    }
    /* clear 把整串灯清成 0（黑）并刷新，比重新 set 全 0 更省事 */
    ESP_ERROR_CHECK(led_strip_clear(g_strip));
}
