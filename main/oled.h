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
 * ========================================================================= */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 初始化 OLED（建 I2C 总线 + SSD1306 面板）。必须在其它 oled_xxx 之前调用。
 * 返回 ESP_OK 表示成功；失败会在内部 ESP_ERROR_CHECK 中止（初始化不过没必要继续）。 */
esp_err_t oled_init(void);

/* 清屏：把内部“帧缓冲”（一块显存数组）全部清成 0（黑）。
 * 注意：只改内存，要让屏幕真正变黑，还要再调一次 oled_refresh()。 */
void oled_clear(void);

/* 在像素坐标 (x,y) 画/擦一个点。on=true 点亮（白），false 熄灭（黑）。
 * 超出屏幕范围自动忽略，不会越界写坏内存。 */
void oled_draw_pixel(int x, int y, bool on);

/* 把内部帧缓冲推送到屏幕。oled_clear / oled_draw_pixel 都只是改内存，
 * 必须调用 oled_refresh() 才会真正显示出来。 */
void oled_refresh(void);

/* 用内置 8x8 字体，在像素坐标 (x,y) 画一行字符串。
 * y 建议取 0 / 8 / 16 / 24 ...（每字符高 8 像素，对齐更整齐）。 */
void oled_draw_text(int x, int y, const char *str);

/* 便捷：显示最多 4 行文本（不用的行传 NULL）。内部会清屏 + 刷新一步到位。
 * 4 行正好占满 128x64 屏幕的上半区（行高 8，0/8/16/24）。 */
void oled_show_lines(const char *l0, const char *l1, const char *l2, const char *l3);

/* 便捷：显示“STATE:”+ 状态两行。这是给后面 L5 状态机准备的“状态显示屏”入口，
 * 比如 oled_show_status("LISTENING") 就会在屏幕上打出 STATE: / LISTENING。 */
void oled_show_status(const char *status);
