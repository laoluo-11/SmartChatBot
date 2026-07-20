/* =========================================================================
 * led.h —— 板载 RGB LED 模块的"对外说明书"
 * -------------------------------------------------------------------------
 * 板载 RGB 灯是"智能寻址灯"（WS2812 / SK68xx 一类），必须用 led_strip 驱动
 * （底层用 RMT 外设生成时序），不能直接用 gpio_set_level 开关。
 *
 * 本模块对外提供三个函数，配合状态机使用：
 *   - led_init()        ：初始化灯带（只调一次，必须在 led_set_color 之前）
 *   - led_set_color()   ：把板载 RGB 灯设成指定颜色（R/G/B 各 0~255）
 *   - led_off()         ：熄灭（等价于 led_set_color(0,0,0)）
 *
 * 颜色现在由"状态机"决定（空闲=灭、聆听=蓝、思考=紫、说话=绿），
 * 所以这里不再有"心跳"任务，灯的颜色随状态实时变化。
 * ========================================================================= */

#ifndef LED_H
#define LED_H

#include <stdint.h>   // 用到 uint8_t（led_set_color 的颜色分量类型）

/* ===== 板载 RGB LED 配置（寻址灯，靠 RMT 时序协议控制，不是普通 GPIO）=====
 * ESP32-S3-DevKitC 系列板载 RGB 灯默认接 GPIO48。
 * 若你的板子接的是别的脚（少数板子用 GPIO38），只改这里即可。 */
#define RGB_LED_GPIO     48      // 板载 RGB 灯数据脚

/* 初始化板载 RGB 灯。内部会创建 led_strip 对象，必须先调用，且只调一次。 */
void led_init(void);

/* 设置灯的颜色。r/g/b 各 0~255（0=不亮，255=最亮）。
 * 例：led_set_color(0,255,0) 亮绿色；led_set_color(0,0,0) 熄灭。 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

/* 熄灭板载 RGB 灯（= led_set_color(0,0,0) 的简写）。 */
void led_off(void);

#endif /* LED_H */
