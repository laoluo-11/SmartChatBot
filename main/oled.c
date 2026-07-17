/* =========================================================================
 * oled.c —— OLED 显示模块实现（SSD1306，I2C 接口）
 * -------------------------------------------------------------------------
 * 核心概念：SSD1306 是一块 128x64 的单色屏（1 位 = 1 个像素，亮或灭）。
 * 驱动芯片里面有“显存(GDDRAM)”，我们往里写一串字节它就能显示。
 *
 * ESP-IDF 的 esp_lcd 组件已经帮我们封装好了 SSD1306 的初始化和“写显存”
 * （esp_lcd_panel_draw_bitmap）。我们要做的只是：
 *   1) 先建一条 I2C 主总线（接 SDA/SCL 两根脚）
 *   2) 在这条总线上挂一个“I2C 面板 IO”（告诉它屏的 I2C 地址）
 *   3) 用这个 IO 创建一个 SSD1306 面板对象（panel 句柄）
 *   4) reset + init + 开显示
 * 之后，我们在自己的一块“帧缓冲数组 g_fb”里画点/写字，再 oled_refresh() 一把
 * 把整块数组丢给面板，屏幕就更新了。
 *
 * 帧缓冲布局（重要，理解了就不晕）：
 *   - 屏幕 128 列 × 64 行 = 8192 像素。
 *   - 单色 1bit/像素 → 共 1024 字节（8192 / 8）。
 *   - SSD1306 按“页(page)”组织：每页 = 8 行。64 行 = 8 页。
 *   - 所以 g_fb 下标 = 页号 * 128 + 列号，即 (y/8)*128 + x。
 *   - 每个字节里，bit0 是最上面那行，bit7 是最下面那行。
 * ========================================================================= */

#include "oled.h"
#include "oled_font.h"              // 8x8 点阵字库
#include <string.h>                 //memset:清屏用
#include "freertos/FreeRTOS.h"      // vTaskDelay 等
#include "esp_log.h"                //ESP_LOGI
#include "esp_lcd_panel_io.h"        // panel IO（含 I2C 的 io 配置）
#include "esp_lcd_panel_ssd1306"     // SSD1306 专用配置 + 建面板
#include "esp_lcd_panel_ops.h"       // panel 句柄、draw_bitmap、init/reset
#include "driver/i2c_master.h"       // v6 的 I2C 主总线 API

static const char *TAG = "oled";      // 日志标签："oled: ..."
/* ===== 配置：按你的接线 / 屏幕改这里 ===== */





