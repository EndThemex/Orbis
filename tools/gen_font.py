#!/usr/bin/env python3
"""生成 2bpp 灰阶点阵 CJK 字体（C++ 源文件）。

用途：Arduino_GFX 自带字体只有 ASCII，歌名/歌手中日文需要自带点阵。
输出为「字形索引表 + 位图数据」两段，渲染时二分查找 + 逐像素 alpha 混合。

为什么是 2bpp：1-bit 阈值会把 FreeType 的灰阶边缘一刀切掉，中文小字号的细笔画
中段直接消失，看起来发虚、断笔；存成 4 级覆盖度（0/1/2/3）后绘制端按覆盖率
与背景混合，笔画连续、边缘不再是台阶。2bpp 的位图是 1-bit 的 2 倍大小，
刚好还塞得进 6.4MB 的 app 分区；4bpp 会超。

用法：
    python tools/gen_font.py                      # 生成 30px 标题 + 22px 正文 + 52px 配对码数字
    python tools/gen_font.py --limit 800          # 只取前 800 个字，用于快速迭代
"""

import argparse
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTCollection, TTFont

# 候选字体：中文优先微软雅黑，日文补充 Yu Gothic / MS Gothic
FONT_CANDIDATES = [
    (r"C:\Windows\Fonts\msyh.ttc", 0),
    (r"C:\Windows\Fonts\YuGothM.ttc", 0),
    (r"C:\Windows\Fonts\msgothic.ttc", 0),
    (r"C:\Windows\Fonts\simhei.ttf", 0),
]

EXTRA_SYMBOLS = "…—–·•♪♫→←↑↓●○■□★☆×÷°±©®™「」『』【】〈〉《》、。，．！？：；"

# 配对码只用数字和空格，单独出一套大字号，避免 5x7 内置字体放大 6 倍的方块锯齿
NUM_CHARS = "0123456789 "


def build_charset():
    """GB2312 全集 + JIS X 0208（含假名/日文汉字）+ 拉丁 + 常用符号。"""
    cs = set(chr(c) for c in range(0x20, 0x7F))
    cs.update(chr(c) for c in range(0xA0, 0x100))
    cs.update(chr(c) for c in range(0x3000, 0x3040))   # CJK 标点
    cs.update(chr(c) for c in range(0x3041, 0x30FF))   # 假名
    cs.update(chr(c) for c in range(0xFF01, 0xFF60))   # 全角符号
    cs.update(EXTRA_SYMBOLS)

    # GB2312：中文歌名/歌手的主要来源
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                cs.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass

    # EUC-JP（JIS X 0208）：日文汉字
    for hi in range(0xA1, 0xFF):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("euc_jp")
            except UnicodeDecodeError:
                continue
            if 0x3000 <= ord(ch) <= 0x9FFF:
                cs.add(ch)
    return cs


def load_cmaps():
    """返回若干 (path, index, cmap) —— 用来过滤字体里没有的码位。"""
    out = []
    for path, index in FONT_CANDIDATES:
        if not os.path.exists(path):
            continue
        try:
            if path.lower().endswith(".ttc"):
                fonts = TTCollection(path).fonts
                cmap = set(fonts[index].getBestCmap().keys())
            else:
                cmap = set(TTFont(path).getBestCmap().keys())
            out.append((path, index, cmap))
        except Exception as exc:  # 字体损坏/权限问题就跳过
            print(f"跳过 {path}: {exc}")
    return out


def quantize(crop):
    """8-bit 灰度 → 2bpp（4 级覆盖度），行按字节对齐，4 像素/字节，MSB 在左。"""
    w, h = crop.size
    stride = (w + 3) // 4
    gray = np.asarray(crop, dtype=np.uint16)
    level = ((gray * 3 + 127) // 255).astype(np.uint8)   # 0..3

    padded = np.zeros((h, stride * 4), dtype=np.uint8)
    padded[:, :w] = level
    packed = ((padded[:, 0::4] << 6) | (padded[:, 1::4] << 4) |
              (padded[:, 2::4] << 2) | padded[:, 3::4])
    return packed.tobytes()


def render_glyph(ch, size, cache):
    """返回 (w, h, xoff, yoff, adv, bytes) —— 以行顶为原点。"""
    cp = ord(ch)
    if cp in cache:
        return cache[cp]

    path, index = None, 0
    # 逐字选择字体：优先雅黑，缺字再退到日文字体
    for p, i, cmap in CMAPS:
        if cp in cmap:
            path, index = p, i
            break

    if path is None:
        cache[cp] = None
        return None

    key = (path, index, size)
    if key not in FONT_CACHE:
        FONT_CACHE[key] = ImageFont.truetype(path, size, index=index)
    font = FONT_CACHE[key]

    canvas = Image.new("L", (size * 2, size * 2), 0)
    ImageDraw.Draw(canvas).text((size // 2, size // 2), ch, font=font, fill=255)

    bbox = canvas.getbbox()
    adv = int(round(font.getlength(ch)))
    if bbox is None:  # 空格等无墨迹字符
        cache[cp] = (0, 0, 0, 0, max(adv, 1), b"")
        return cache[cp]

    x0, y0, x1, y1 = bbox
    w, h = x1 - x0, y1 - y0
    crop = canvas.crop(bbox)

    result = (w, h, x0 - size // 2, y0 - size // 2, max(adv, 1), quantize(crop))
    cache[cp] = result
    return result


def emit(font_symbol, name, size, chars, out_path):
    metric_path, metric_index = (CMAPS[0][0], CMAPS[0][1]) if CMAPS else ("", 0)
    ascent, descent = ImageFont.truetype(metric_path, size, index=metric_index).getmetrics()
    glyphs = []
    blob = bytearray()
    cache = {}

    for ch in sorted(chars):
        g = render_glyph(ch, size, cache)
        if g is None:
            continue
        w, h, xoff, yoff, adv, data = g
        glyphs.append((ord(ch), len(blob), w, h, xoff, yoff, min(adv, 255)))
        blob += data

    lines = []
    lines.append("// 由 tools/gen_font.py 生成，请勿手工修改")
    lines.append(f'#include "cjk_font.h"')
    lines.append("")
    lines.append(f"// {name}: {size}px/2bpp, {len(glyphs)} 字形, 位图 {len(blob)} 字节")
    lines.append("static const CjkGlyph GLYPHS[] = {")
    for cp, off, w, h, xoff, yoff, adv in glyphs:
        lines.append(f"{{0x{cp:05X},{off},{w},{h},{xoff},{yoff},{adv}}},")
    lines.append("};")
    lines.append("")
    lines.append("static const uint8_t BITS[] = {")
    for i in range(0, len(blob), 24):
        lines.append(",".join(f"0x{b:02X}" for b in blob[i:i + 24]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"const CjkFont {font_symbol} = {{GLYPHS, BITS, {len(glyphs)}, "
                 f"{ascent + descent}, {ascent}}};")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as fp:
        fp.write("\n".join(lines))
    return len(glyphs), len(blob)


CMAPS = load_cmaps()
FONT_CACHE = {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default="src")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    chars = build_charset()
    if args.limit:
        chars = set(sorted(chars)[:args.limit])

    jobs = [
        ("FONT_TITLE", "font_title_30", 30, "font_title_30.cpp", chars),
        ("FONT_BODY", "font_body_22", 22, "font_body_22.cpp", chars),
        ("FONT_NUM", "font_num_52", 52, "font_num_52.cpp", set(NUM_CHARS)),
    ]
    for symbol, name, size, filename, charset in jobs:
        count, nbytes = emit(symbol, name, size, charset, os.path.join(args.out_dir, filename))
        print(f"{filename}: {count} 字形, {nbytes / 1024:.0f} KB 位图")


if __name__ == "__main__":
    main()
