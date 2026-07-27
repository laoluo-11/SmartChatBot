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
#include "oled_font.h"                             // 8x8 点阵字库（已单独拆成 font8x8.h）
#include <string.h>                              // memset：清屏用
#include "freertos/FreeRTOS.h"                  // vTaskDelay 等
#include "freertos/task.h"                      // 任务相关
#include "esp_log.h"                            // ESP_LOGI
#include "esp_lcd_panel_io.h"                   // panel IO（含 I2C 的 io 配置）
#include "esp_lcd_panel_ssd1306.h"              // SSD1306 专用配置 + 建面板
#include "esp_lcd_panel_ops.h"                  // panel 句柄、draw_bitmap、init/reset
#include "driver/i2c_master.h"                  // v6 的 I2C 主总线 API
#include "esp_timer.h"                          // 滚动显示用的周期定时器
#include "oled_font_cjk.h"                      // 16x16 中文点阵字库（自动生成）

static const char *TAG = "oled";               // 日志标签："oled: ..."

/* ===== 配置：按你的接线 / 屏幕改这里 ===== */
#define OLED_I2C_PORT      I2C_NUM_0   // 用第 0 路 I2C 控制器
#define OLED_I2C_SDA_GPIO  41          // OLED 的 SDA → ESP32 的 GPIO41（默认是 JTAG-TCK，当 GPIO 用没问题）
#define OLED_I2C_SCL_GPIO  42          // OLED 的 SCL → ESP32 的 GPIO42（默认是 JTAG-TDO，当 GPIO 用没问题）
#define OLED_ADDR          0x3C        // 绝大多数 0.96 寸屏地址是 0x3C；若屏不亮可试 0x3D
#define OLED_WIDTH         128         // 屏宽（像素）
#define OLED_HEIGHT        64          // 屏高（像素）。若是 128x32 的小屏改成 32
#define OLED_I2C_FREQ_HZ   400000      // I2C 速率 400kHz。便宜的克隆屏若花屏，降到 100000

/* 帧缓冲：128*64/8 = 1024 字节，整块显存放在这儿（全局静态，不占任务栈） */
static uint8_t g_fb[OLED_WIDTH * OLED_HEIGHT / 8];
/* SSD1306 面板句柄：初始化成功后非空，后面 draw_bitmap 都用它 */
static esp_lcd_panel_handle_t g_panel = NULL;
/* 8x8 点阵字库 font8x8_basic[128][8] 在 font8x8.h 里（已单独拆文件） */

/* ---- 中文回答滚动显示状态 ----
 * 128x64 屏用 16x16 字只能放 8 列 × 4 行 ≈ 32 字，长回答需自动换行 + 纵向滚动。
 * g_answer_showing 为 true 时，状态机的状态屏(oled_show_status/oled_show_lines)让位，
 * 避免把正在显示的回答冲掉。 */
#define OLED_ANSWER_MAX_LINES  64        // 最多缓存行数（防止超长回答爆缓冲）
#define OLED_ANSWER_ROW_H      16        // 每行高 16 像素（16x16 字）
#define OLED_ANSWER_VISIBLE    4         // 一屏可见 4 行（64px）
#define OLED_ANSWER_SCROLL_MS  800       // 每 800ms 向上滚一行
#define OLED_ANSWER_BUF_LEN    700       // 回答文本缓冲（Qwen 已要求简短，够用）

typedef struct { int off; int len; } ans_line_t;

static char        g_answer_buf[OLED_ANSWER_BUF_LEN];
static ans_line_t  g_answer_lines[OLED_ANSWER_MAX_LINES];
static int         g_answer_nlines = 0;
static int         g_answer_scroll = 0;
static bool        g_answer_showing = false;
static esp_timer_handle_t g_scroll_timer = NULL;

/* -------------------------------------------------------------------------
 * oled_init：初始化 OLED（建 I2C 总线 → panel IO → SSD1306 面板）
 * ------------------------------------------------------------------------- */
esp_err_t oled_init(void)
{
    /* 步骤 1：建一条 I2C 主总线。
     * SDA/SCL 两根脚由我们指定；enable_internal_pullup 让芯片内部上拉（模块上通常也
     * 有 4.7k 上拉电阻，双保险）。clk_source 用默认（XTAL 晶振）。 */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = OLED_I2C_PORT,          // 用第 0 路 I2C
        .sda_io_num        = OLED_I2C_SDA_GPIO,      // SDA → GPIO41
        .scl_io_num        = OLED_I2C_SCL_GPIO,      // SCL → GPIO42
        .clk_source        = I2C_CLK_SRC_DEFAULT,    // 默认时钟源
        .glitch_ignore_cnt = 7,                      // 抗毛刺（官方推荐值）
        .flags.enable_internal_pullup = true,        // 开启内部上拉
    };
    i2c_master_bus_handle_t bus_handle = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    /* 步骤 2：在这条总线上挂一个“I2C 面板 IO”。
     * dev_addr 是屏的 I2C 地址；control_phase_bytes=1 + dc_bit_offset=6 是 SSD1306
     * 的约定：每个传输前面带 1 个“控制字节”，其中第 6 位表示后面是命令(0)还是数据(1)。
     * 这些都不用记，是 SSD1306 的标配写法。 */
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr          = OLED_ADDR,
        .scl_speed_hz      = OLED_I2C_FREQ_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset     = 6,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        // flags 留默认：DC=1 表示数据、DC=0 表示命令（SSD1306 标准）
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus_handle, &io_cfg, &io_handle));

    /* 步骤 3：用这个 IO 创建 SSD1306 面板对象。
     * bits_per_pixel=1（单色）；reset_gpio_num=-1 表示屏没接复位脚（接了就改成对应 GPIO）；
     * vendor_config 传给驱动屏的“高”（64 还是 32）。 */
    esp_lcd_panel_ssd1306_config_t ssd1306_cfg = {
        .height = OLED_HEIGHT,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel  = 1,
        .reset_gpio_num  = -1,
        .vendor_config   = &ssd1306_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_cfg, &g_panel));

    /* 步骤 4：硬件复位 + 初始化 + 开显示。init 里驱动会发一串 SSD1306 命令
     * （开电荷泵、设寻址模式等），不用我们操心。 */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel, true));

    /* 清屏并刷新一次，确保上电后是干净的黑屏 */
    oled_clear();
    oled_refresh();
    ESP_LOGI(TAG, "OLED 已初始化 %dx%d @ I2C 0x%02X (SDA=GPIO%d, SCL=GPIO%d)",
             OLED_WIDTH, OLED_HEIGHT, OLED_ADDR, OLED_I2C_SDA_GPIO, OLED_I2C_SCL_GPIO);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * oled_draw_pixel：在帧缓冲里画/擦一个像素点
 * ------------------------------------------------------------------------- */
void oled_draw_pixel(int x, int y, bool on)
{
    /* 越界保护：坐标超出屏幕就直接返回，避免写坏数组 */
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    /* 算这个像素落在帧缓冲的哪个字节、哪一位（见文件头“帧缓冲布局”说明） */
    int idx = (y / 8) * OLED_WIDTH + x;   // 第几个字节（第几页的第几列）
    int bit = y % 8;                       // 字节里的第几位（行）
    if (on) {
        g_fb[idx] |= (1 << bit);           // 置 1 = 点亮
    } else {
        g_fb[idx] &= ~(1 << bit);          // 清 0 = 熄灭
    }
}

/* -------------------------------------------------------------------------
 * oled_clear：把整块帧缓冲清成 0（黑）
 * ------------------------------------------------------------------------- */
void oled_clear(void)
{
    memset(g_fb, 0, sizeof(g_fb));
}

/* -------------------------------------------------------------------------
 * oled_refresh：把帧缓冲整块推到屏幕
 * ------------------------------------------------------------------------- */
void oled_refresh(void)
{
    /* draw_bitmap 从 (0,0) 到 (宽,高) 把 g_fb 的数据发出去。
     * SSD1306 驱动内部会按 GDDRAM 的“页”布局接收，正好和我们的 g_fb 布局一致。 */
    esp_lcd_panel_draw_bitmap(g_panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, g_fb);
}

/* -------------------------------------------------------------------------
 * oled_draw_text：用 8x8 字体在 (x,y) 写一行字符串
 * ------------------------------------------------------------------------- */
void oled_draw_text(int x, int y, const char *str)
{
    if (str == NULL) {
        return;
    }
    int cur_x = x;                         // 当前写到哪一列
    for (int i = 0; str[i] != '\0' && cur_x < OLED_WIDTH; i++) {
        unsigned char c = (unsigned char)str[i];
        /* 只支持 0x20~0x7F 的可打印字符；越界统一当空格，避免数组越界 */
        if (c < 0x20 || c > 0x7F) {
            c = 0x20;
        }
        const uint8_t *glyph = font8x8_basic[c];   // 取这个字符的 8 字节点阵
        /* 逐行、逐列把点阵“贴”到帧缓冲 */
        for (int row = 0; row < 8; row++) {
            uint8_t line = glyph[row];
            for (int col = 0; col < 8; col++) {
                /* 点阵里 bit7 = 最左列，所以判断 (line & (1<<(7-col))) */
                if (line & (1 << (7 - col))) {
                    oled_draw_pixel(cur_x + col, y + row, true);
                }
            }
        }
        cur_x += 8;   // 每个字符宽 8 像素，往后挪
    }
}

/* -------------------------------------------------------------------------
 * oled_show_lines：一次性显示最多 4 行（清屏 + 画字 + 刷新）
 * ------------------------------------------------------------------------- */
void oled_show_lines(const char *l0, const char *l1, const char *l2, const char *l3)
{
    if (g_answer_showing) return;          // 正在显示回答，状态屏让位
    oled_clear();
    if (l0) oled_draw_text(0, 0,  l0);   // 第 0 行（y=0）
    if (l1) oled_draw_text(0, 8,  l1);   // 第 1 行（y=8）
    if (l2) oled_draw_text(0, 16, l2);   // 第 2 行（y=16）
    if (l3) oled_draw_text(0, 24, l3);   // 第 3 行（y=24）
    oled_refresh();
}

/* -------------------------------------------------------------------------
 * oled_show_status：显示“STATE:”+ 状态（给后面状态机用）
 * ------------------------------------------------------------------------- */
void oled_show_status(const char *status)
{
    if (g_answer_showing) return;          // 正在显示回答，状态屏让位
    oled_show_lines("STATE:", status, NULL, NULL);
}

/* =========================================================================
 * 中文显示：16x16 点阵 + 中英混排 + 自动换行 + 纵向滚动
 * ========================================================================= */

/* 在 (x,y) 画一个 16x16 字形。g 共 32 字节：第 r 行 = g[2r](左8) + g[2r+1](右8)，
 * 字节里 bit7 = 最左像素。（与 oled_font_cjk.h 的排版一一对应） */
static void oled_draw_glyph16(int x, int y, const uint8_t *g)
{
    if (!g) return;
    for (int r = 0; r < 16; r++) {
        uint8_t hi = g[2 * r];
        uint8_t lo = g[2 * r + 1];
        for (int c = 0; c < 8; c++) {
            if (hi & (1 << (7 - c))) oled_draw_pixel(x + c,     y + r, true);
        }
        for (int c = 0; c < 8; c++) {
            if (lo & (1 << (7 - c))) oled_draw_pixel(x + 8 + c, y + r, true);
        }
    }
}

/* 画一个 16x16 的空心方框，用作“字库里没有的字”的兜底占位 */
static void oled_draw_box16(int x, int y)
{
    for (int c = 0; c < 16; c++) {
        oled_draw_pixel(x + c, y,      true);
        oled_draw_pixel(x + c, y + 15, true);
    }
    for (int r = 0; r < 16; r++) {
        oled_draw_pixel(x,      y + r, true);
        oled_draw_pixel(x + 15, y + r, true);
    }
}

/* 在 CJK_CODES[](升序) 中二分查找 unicode，返回对应点阵或 NULL */
static const uint8_t *cjk_find(uint16_t cp)
{
    int lo = 0, hi = (int)CJK_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (CJK_CODES[mid] == cp)      return CJK_GLYPHS[mid];
        else if (CJK_CODES[mid] < cp)  lo = mid + 1;
        else                           hi = mid - 1;
    }
    return NULL;
}

/* 中英混排渲染：UTF-8 解码后，ASCII 用 8x8 字，其余用 16x16 字（找不到画方框）。
 * 行高 16 像素；ASCII 宽 8、CJK 宽 16，两种宽度混排也能对齐到同一基线。 */
void oled_draw_text_mixed(int x, int y, const char *str)
{
    if (!str) return;
    int cur_x = x;
    int n = (int)strlen(str);
    for (int i = 0; i < n; ) {
        unsigned char c0 = (unsigned char)str[i];
        uint32_t cp; int clen;
        if (c0 < 0x80)               { cp = c0;            clen = 1; }
        else if ((c0 & 0xE0) == 0xC0){ cp = c0 & 0x1F;     clen = 2; }
        else if ((c0 & 0xF0) == 0xE0){ cp = c0 & 0x0F;     clen = 3; }
        else if ((c0 & 0xF8) == 0xF0){ cp = c0 & 0x07;     clen = 4; }
        else                          { i++; continue; }            // 无效首字节，跳过
        for (int k = 1; k < clen && i + k < n; k++)
            cp = (cp << 6) | ((unsigned char)str[i + k] & 0x3F);
        i += clen;

        if (cp <= 0x7F) {
            unsigned char ch = (unsigned char)cp;
            if (ch < 0x20) ch = 0x20;
            const uint8_t *gl = font8x8_basic[ch];
            for (int r = 0; r < 8; r++) {
                uint8_t line = gl[r];
                for (int c = 0; c < 8; c++)
                    if (line & (1 << (7 - c))) oled_draw_pixel(cur_x + c, y + r, true);
            }
            cur_x += 8;
        } else {
            const uint8_t *g = cjk_find((uint16_t)cp);
            if (g) oled_draw_glyph16(cur_x, y, g);
            else   oled_draw_box16(cur_x, y);
            cur_x += 16;
        }
    }
}

/* 把回答文本按屏宽(128px)自动换行，结果存进 g_answer_lines[]（指向 g_answer_buf
 * 内的偏移/长度，不另开内存）。遇到 '\n' 强制换行。 */
static void oled_wrap_answer(const char *text)
{
    g_answer_nlines = 0;
    int n = (int)strlen(text);
    int line_start = 0, line_w = 0, i = 0;
    while (i < n) {
        unsigned char c0 = (unsigned char)text[i];
        int clen, w; uint32_t cp;
        if (c0 < 0x80)               { cp = c0;        clen = 1; }
        else if ((c0 & 0xE0) == 0xC0){ cp = c0 & 0x1F; clen = 2; }
        else if ((c0 & 0xF0) == 0xE0){ cp = c0 & 0x0F; clen = 3; }
        else if ((c0 & 0xF8) == 0xF0){ cp = c0 & 0x07; clen = 4; }
        else                          { i++; continue; }
        for (int k = 1; k < clen && i + k < n; k++)
            cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);

        if (c0 == '\n') {                                  // 硬换行
            if (g_answer_nlines < OLED_ANSWER_MAX_LINES)
                g_answer_lines[g_answer_nlines++] = (ans_line_t){ line_start, i - line_start };
            i++; line_start = i; line_w = 0; continue;
        }
        w = (cp <= 0x7F) ? 8 : 16;
        if (line_w + w > OLED_WIDTH && line_w > 0) {       // 这行放不下，另起一行
            if (g_answer_nlines < OLED_ANSWER_MAX_LINES)
                g_answer_lines[g_answer_nlines++] = (ans_line_t){ line_start, i - line_start };
            line_start = i; line_w = 0;
            /* 不消费本字符，留到新行开头 */
        } else {
            line_w += w; i += clen;
        }
    }
    if (i > line_start && g_answer_nlines < OLED_ANSWER_MAX_LINES)
        g_answer_lines[g_answer_nlines++] = (ans_line_t){ line_start, i - line_start };
}

/* 把当前滚动位置对应的若干行画到帧缓冲并刷新 */
static void oled_render_answer(void)
{
    oled_clear();
    for (int k = 0; k < OLED_ANSWER_VISIBLE; k++) {
        int li = g_answer_scroll + k;
        if (li >= g_answer_nlines) break;
        int s = g_answer_lines[li].off;
        int l = g_answer_lines[li].len;
        char tmp = g_answer_buf[s + l];
        g_answer_buf[s + l] = '\0';                        // 临时截断，渲染后恢复
        oled_draw_text_mixed(0, k * OLED_ANSWER_ROW_H, g_answer_buf + s);
        g_answer_buf[s + l] = tmp;
    }
    oled_refresh();
}

/* 周期定时器回调：向上滚一行（仅在显示回答时生效） */
static void oled_scroll_timer_cb(void *arg)
{
    (void)arg;
    if (!g_answer_showing) return;
    oled_render_answer();
    if (g_answer_nlines > OLED_ANSWER_VISIBLE) {
        g_answer_scroll++;
        if (g_answer_scroll > g_answer_nlines - OLED_ANSWER_VISIBLE)
            g_answer_scroll = 0;                           // 滚到末尾后回到开头循环
    }
}

/* 显示服务器回答（自动换行 + 滚动）。text 最长 OLED_ANSWER_BUF_LEN-1。 */
void oled_show_answer(const char *text)
{
    if (!text) return;
    strncpy(g_answer_buf, text, OLED_ANSWER_BUF_LEN - 1);
    g_answer_buf[OLED_ANSWER_BUF_LEN - 1] = '\0';
    oled_wrap_answer(g_answer_buf);
    g_answer_scroll = 0;
    g_answer_showing = true;
    if (!g_scroll_timer) {
        esp_timer_handle_t t = NULL;
        esp_timer_create(&(esp_timer_create_args_t){ .callback = oled_scroll_timer_cb,
                                                      .name = "oled_scroll" }, &t);
        g_scroll_timer = t;
    }
    if (g_scroll_timer) {
        esp_timer_stop(g_scroll_timer);
        esp_timer_start_periodic(g_scroll_timer, OLED_ANSWER_SCROLL_MS * 1000);
    }
    oled_render_answer();                                  // 立即显示第一屏
}

/* 隐藏回答、把屏幕交还给状态机 */
void oled_answer_hide(void)
{
    g_answer_showing = false;
    if (g_scroll_timer) esp_timer_stop(g_scroll_timer);
}
