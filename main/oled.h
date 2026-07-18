/* =========================================================================
 * oled.h —— OLED 显示模块（SSD1306，I2C 接口）
 * -------------------------------------------------------------------------
 * 这块 0.96 寸 OLED 用 SSD1306 驱动芯片，通过 I2C 两根线（SDA/SCL）和 ESP32 通
 * 信。ESP-IDF 自带 esp_lcd 组件里就有现成的 SSD1306 驱动，我们只用三步走：
 *   i2c 总线 → panel_io(I2C) → panel(SSD1306)
 * 然后拿到一个 panel 句柄，往里“画”一个帧缓冲（显存）就能显示。
 *
 * 本模块对外提供：
 *   - oled_init()         ：初始化（建总线 + 面板），必须先调用
 *   - oled_clear()        ：清屏（只改内存）
 *   - oled_draw_pixel()   ：画/擦一个像素点
 *   - oled_refresh()      ：把内存里的帧缓冲推到屏幕
 *   - oled_draw_text()    ：用内置 8x8 字体写一行字
 *   - oled_show_lines()   ：一次性显示最多 4 行文本（清屏+刷新一步到位）
 *   - oled_show_status()  ：显示“STATE: XXX”（给后面的状态机用）
 *   - oled_task()         ：L4 演示任务，循环切状态方便肉眼验收
 * ========================================================================= */


#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 初始化 OLED（建 I2C 总线 + SSD1306 面板）。必须在其它 oled_xxx 之前调用。
 * 返回 ESP_OK 表示成功；失败会在内部 ESP_ERROR_CHECK 中止（初始化不过没必要继续）。 */
esp_err_t oled_init(void);
void oled_draw_pixel(int x, int y, bool on);
void oled_clear(void);
void oled_refresh(void);
void oled_draw_text(int x, int y, const char *str);
void oled_show_lines(const char *l0, const char *l1, const char *l2, const char *l3);
void oled_show_status(const char *status);
void oled_task(void *pvParameters);
