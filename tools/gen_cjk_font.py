#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_cjk_font.py —— 把系统里的中文字体烤成 16x16 点阵字库（C 头文件）

用法:
    python gen_cjk_font.py [输出头文件路径]

默认输出到固件 main/oled_font_cjk.h

覆盖的字符:
    1) GB2312 一级汉字（gb2312 高字节 0xB0~0xD7，约 3755 个，日常口语够用）
    2) 中文常用标点 / 全角符号（U+3000~U+303F、U+FF00~U+FFEF 中能编进 gb2312 的）

点阵格式（与 oled.c 的 oled_draw_glyph16 对应）:
    每个汉字 16 行 × 16 列 = 32 字节。
    第 r 行(0..15) = 2 字节: glyph[2r] 是高 8 位(左 8 像素), glyph[2r+1] 是低 8 位(右 8 像素)。
    字节里 bit7 = 最左列像素, bit0 = 最右列像素。
"""
import sys
import os
import struct
from PIL import Image, ImageFont, ImageDraw

# ---- 候选字体（按系统里实际存在的优先尝试）----
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",       # 微软雅黑
    r"C:\Windows\Fonts\simhei.ttf",     # 黑体
    r"C:\Windows\Fonts\simsun.ttc",     # 宋体
    r"C:\Windows\Fonts\NotoSansSC-VF.ttf",
    r"C:\Windows\Fonts\simkai.ttf",     # 楷体
]

OUT_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "main", "oled_font_cjk.h")

SIZE = 16          # 字号（像素）
THRESH = 128       # 二值化阈值（0~255）


def pick_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                # .ttc 集合用 index=0；单 ttf 也能用
                return ImageFont.truetype(path, SIZE, index=0), path
            except Exception as e:
                print("  跳过 %s: %s" % (path, e))
    raise RuntimeError("没找到可用中文字体，请检查 FONT_CANDIDATES")


def is_yijihanzhi(cp):
    """GB2312 一级汉字: 高字节 0xB0~0xD7（区 16~55）"""
    try:
        b = chr(cp).encode("gb2312")
    except Exception:
        return False
    return 0xB0 <= b[0] <= 0xD7


def is_punct(cp):
    """中文标点 / 全角符号范围"""
    return (0x3000 <= cp <= 0x303F) or (0xFF00 <= cp <= 0xFFEF)


def render_glyph(font, cp):
    """渲染一个 16x16 点阵，返回 32 字节 bytes。失败返回 None。
    渲染后做垂直居中：把字形的上下空白吃掉，使其在 16 像素格子里居中。"""
    img = Image.new("L", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(img)
    try:
        draw.text((0, 0), chr(cp), fill=255, font=font)
    except Exception:
        return None
    px = img.load()
    # 先落到 16x16 网格
    grid = [[1 if px[c, r] >= THRESH else 0 for c in range(SIZE)] for r in range(SIZE)]
    rows_with = [r for r in range(SIZE) if any(grid[r])]
    if not rows_with:
        return None
    top, bot = rows_with[0], rows_with[-1]
    h = bot - top + 1
    shift = (SIZE - h) // 2                      # 居中偏移
    final = [[0] * SIZE for _ in range(SIZE)]
    for r in range(top, bot + 1):
        nr = r - top + shift
        if 0 <= nr < SIZE:
            final[nr] = grid[r][:]
    bits = bytearray(32)
    for r in range(SIZE):
        word = 0
        for c in range(SIZE):
            if final[r][c]:
                word |= (1 << (15 - c))           # bit15=最左
        bits[2 * r] = (word >> 8) & 0xFF
        bits[2 * r + 1] = word & 0xFF
    return bytes(bits)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT
    out_path = os.path.abspath(out_path)

    font, font_path = pick_font()
    print("使用字体: %s" % font_path)

    glyphs = {}   # unicode -> 32 bytes
    # 一级汉字
    for cp in range(0x4E00, 0x9FFF + 1):
        if is_yijihanzhi(cp):
            g = render_glyph(font, cp)
            if g:
                glyphs[cp] = g
    # 标点 / 全角
    for cp in list(range(0x3000, 0x303F + 1)) + list(range(0xFF00, 0xFFEF + 1)):
        try:
            chr(cp).encode("gb2312")
        except Exception:
            continue
        g = render_glyph(font, cp)
        if g:
            glyphs[cp] = g

    codes = sorted(glyphs.keys())
    print("生成字形数: %d" % len(codes))

    # ---- 写头文件 ----
    lines = []
    lines.append("/* =========================================================================")
    lines.append(" * oled_font_cjk.h —— 16x16 中文点阵字库（由 gen_cjk_font.py 自动生成，勿手改）")
    lines.append(" * -------------------------------------------------------------------------")
    lines.append(" * 来源字体: %s" % os.path.basename(font_path))
    lines.append(" * 字符数:   %d（GB2312 一级汉字 + 中文标点/全角符号）" % len(codes))
    lines.append(" * 每字 32 字节，总大小约 %d 字节（存 flash，不占 RAM）。" % (len(codes) * 32))
    lines.append(" * 排版: 第 r 行 = glyph[2r](左8列) + glyph[2r+1](右8列)，bit7=最左像素。")
    lines.append(" * 查找: CJK_CODES[] 升序，运行时二分；下标与 CJK_GLYPHS[] 一一对应。")
    lines.append(" * ========================================================================= */")
    lines.append("")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#define CJK_GLYPH_BYTES 32")
    lines.append("")
    lines.append("/* 升序的 Unicode 码点表（用于二分查找） */")
    lines.append("static const uint16_t CJK_CODES[] = {")
    # 每行 12 个，逗号
    row = []
    for i, cp in enumerate(codes):
        row.append("0x%04X" % cp)
        if len(row) == 12 or i == len(codes) - 1:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    lines.append("};")
    lines.append("")
    lines.append("/* 与 CJK_CODES 一一对应的 16x16 点阵（每字 32 字节） */")
    lines.append("static const uint8_t CJK_GLYPHS[][CJK_GLYPH_BYTES] = {")
    for i, cp in enumerate(codes):
        g = glyphs[cp]
        hexstr = ", ".join("0x%02X" % b for b in g)
        comment = " // U+%04X %s" % (cp, chr(cp) if 0x20 <= cp < 0x7F or cp > 0x2E7F else "")
        lines.append("    { " + hexstr + " }," + comment)
    lines.append("};")
    lines.append("")
    lines.append("#define CJK_COUNT (%d)" % len(codes))
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("已写出: %s" % out_path)


if __name__ == "__main__":
    main()
