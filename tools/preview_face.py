#!/usr/bin/env python3
"""离线预览：不开硬件也能看媒体页长什么样。

做的事和固件完全一致：
  1. 解析 src/font_*.cpp 里的 2bpp 字形表与位图（和 cjk_font.cpp 的二分查找同一份数据）
  2. 复刻 media_face.cpp 的覆盖率抗锯齿（圆环 / 圆形 / 三角形）
  3. 写进 RGB565 帧缓冲，按 1/16 覆盖率做 alpha 混合（和 cjkBlendPixel 同一公式）
所以产出的 PNG 可以用来核对布局常量、字号、截断和抗锯齿效果，改完字体先跑一次。

用法：
    python tools/preview_face.py                    # 输出到 .screenshots/
    python tools/preview_face.py --out docs         # 输出到 docs/
    python tools/preview_face.py --title 想你的夜 --artist 周杰伦   # 换文案
"""

import argparse
import math
import os
import re

import numpy as np
from PIL import Image

SCREEN_W = SCREEN_H = 466
CX = CY = 233
RING_R_OUT, RING_R_IN = 230, 224
HEADER_Y, TITLE_Y1, TITLE_MAX_W = 78, 126, 336
ARTIST_Y, ALBUM_Y, TEXT_MAX_W = 208, 240, 330
CTRL_Y, TIME_Y = 352, 408
BTN_PREV_X, BTN_NEXT_X, BTN_PLAY_X = 132, 334, 233
BTN_SIDE_R, BTN_PLAY_R = 37, 42

COVERAGE_TAB = [0, 5, 11, 16]   # cjk_font.cpp 的 2bpp → 1/16 覆盖率


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


COL_BG = rgb565(0, 0, 0)
COL_RING = rgb565(38, 46, 60)
COL_ACCENT = rgb565(0, 214, 255)
COL_TEXT = rgb565(232, 238, 248)
COL_DIM = rgb565(176, 188, 208)
COL_DIMMER = rgb565(104, 116, 136)


class Surface:
    """RGB565 帧缓冲 + cjkBlendPixel 的等价实现。"""

    def __init__(self):
        self.fb = np.zeros((SCREEN_H, SCREEN_W), np.uint16)

    def blend(self, x, y, color, cov):
        cov = int(cov)
        if cov == 0 or x < 0 or y < 0 or x >= SCREEN_W or y >= SCREEN_H:
            return
        if cov >= 16:
            self.fb[y, x] = color
            return
        d = int(self.fb[y, x])
        s = int(color)
        inv = 16 - cov
        rb = ((s & 0xF81F) * cov + (d & 0xF81F) * inv) >> 4
        g = ((s & 0x07E0) * cov + (d & 0x07E0) * inv) >> 4
        self.fb[y, x] = (rb & 0xF81F) | (g & 0x07E0)

    def fill_rect(self, x, y, w, h, color):
        self.fb[max(y, 0):y + h, max(x, 0):x + w] = color


class Font:
    def __init__(self, path):
        src = open(path, encoding="utf-8").read()
        table = src.split("static const uint8_t BITS")[0]
        self.glyphs = {}
        for m in re.finditer(r"\{0x([0-9A-F]+),(\d+),(\d+),(\d+),(-?\d+),(-?\d+),(\d+)\}", table):
            g = m.groups()
            self.glyphs[int(g[0], 16)] = tuple(int(v) for v in g[1:])
        body = src.split("static const uint8_t BITS[] = {")[1].split("};")[0]
        self.bits = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-F]{2})", body))
        self.count, self.line_height, self.ascent = (
            int(n) for n in re.findall(r"\d+", src.strip().splitlines()[-1].split("BITS, ")[1]))

    def advance(self, cp):
        g = self.glyphs.get(cp)
        return g[5] if g else self.ascent // 2

    def width(self, text):
        return sum(self.advance(ord(c)) for c in text)

    def draw(self, surf, x, y_top, color, text):
        pen = x
        for ch in text:
            g = self.glyphs.get(ord(ch))
            if g:
                off, w, h, xoff, yoff, adv = g
                stride = (w + 3) // 4
                for yy in range(h):
                    for xx in range(w):
                        byte = self.bits[off + yy * stride + (xx >> 2)]
                        cov = COVERAGE_TAB[(byte >> (6 - 2 * (xx & 3))) & 3]
                        if cov:
                            surf.blend(pen + xoff + xx, y_top + yoff + yy, color, cov)
            pen += self.advance(ord(ch))
        return pen - x


def coverage(d):
    if d <= -0.5:
        return 0
    if d >= 0.5:
        return 16
    return int((d + 0.5) * 16.0 + 0.5)


def fill_circle(s, cx, cy, r, color):
    r2 = (r + 0.5) ** 2
    for y in range(cy - r - 1, cy + r + 2):
        dy = y + 0.5 - cy
        for x in range(cx - r - 1, cx + r + 2):
            dx = x + 0.5 - cx
            d2 = dx * dx + dy * dy
            if d2 > r2:
                continue
            s.blend(x, y, color, coverage(r - math.sqrt(d2)))


def stroke_circle(s, cx, cy, r, width, color):
    half = width * 0.5
    ro, ri = r + half, r - half
    ro2, ri2 = (ro + 0.5) ** 2, (ri - 0.5) ** 2
    yr = int(math.ceil(ro + 0.5))
    for y in range(cy - yr, cy + yr + 1):
        dy = y + 0.5 - cy
        for x in range(cx - yr, cx + yr + 1):
            dx = x + 0.5 - cx
            d2 = dx * dx + dy * dy
            if d2 > ro2 or d2 < ri2:
                continue
            dist = math.sqrt(d2)
            s.blend(x, y, color, coverage(min(ro - dist, dist - ri)))


def edge_dist(px, py, qx, qy, tx, ty, x, y):
    ex, ey = qx - px, qy - py
    ln = math.hypot(ex, ey)
    if ln <= 0:
        return 1e9
    d = (ex * (y - py) - ey * (x - px)) / ln
    return -d if (ex * (ty - py) - ey * (tx - px)) < 0 else d


def fill_triangle(s, x0, y0, x1, y1, x2, y2, color):
    for y in range(min(y0, y1, y2), max(y0, y1, y2) + 1):
        py = y + 0.5
        for x in range(min(x0, x1, x2), max(x0, x1, x2) + 1):
            px = x + 0.5
            d = min(edge_dist(x0, y0, x1, y1, x2, y2, px, py),
                    edge_dist(x1, y1, x2, y2, x0, y0, px, py),
                    edge_dist(x2, y2, x0, y0, x1, y1, px, py))
            if d > -0.5:
                s.blend(x, y, color, coverage(d))


def ring_span(s, y, dy, x_from, x_to, a0, a1, full, color):
    for x in range(x_from, x_to + 1):
        dx = x + 0.5 - CX
        dist = math.hypot(dx, dy)
        if not full:
            ang = math.degrees(math.atan2(dx, -dy))
            if ang < 0:
                ang += 360.0
            if ang < a0 or ang > a1:
                continue
        s.blend(x, y, color, coverage(min(RING_R_OUT - dist, dist - RING_R_IN)))


def draw_ring(s, a0, a1, color):
    if a1 - a0 < 0.05:
        return
    full = (a1 - a0) >= 359.9
    ro2 = (RING_R_OUT + 0.5) ** 2
    ri2 = (RING_R_IN - 0.5) ** 2
    yr = int(math.ceil(RING_R_OUT + 0.5))
    for y in range(CY - yr, CY + yr + 1):
        dy = y + 0.5 - CY
        dy2 = dy * dy
        if dy2 > ro2:
            continue
        xo = int(math.sqrt(ro2 - dy2))
        xi = int(math.sqrt(ri2 - dy2)) if dy2 < ri2 else 0
        if xi > 0:
            ring_span(s, y, dy, CX - xo, CX - xi, a0, a1, full, color)
            ring_span(s, y, dy, CX + xi, CX + xo, a0, a1, full, color)
        else:
            ring_span(s, y, dy, CX - xo, CX + xo, a0, a1, full, color)


def centered(surf, font, y, color, text):
    font.draw(surf, (SCREEN_W - font.width(text)) // 2, y, color, text)


def to_image(fb):
    r = ((fb >> 11) & 0x1F).astype(np.uint16)
    g = ((fb >> 5) & 0x3F).astype(np.uint16)
    b = (fb & 0x1F).astype(np.uint16)
    rgb = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1)
    return Image.fromarray(rgb.astype(np.uint8))


def draw_controls(s, playing):
    for bx in (BTN_PREV_X, BTN_NEXT_X):
        stroke_circle(s, bx, CTRL_Y, BTN_SIDE_R, 1.0, COL_RING)
    # 上一首：竖条 + 左指三角
    s.fill_rect(BTN_PREV_X - 11, CTRL_Y - 10, 3, 21, COL_TEXT)
    fill_triangle(s, BTN_PREV_X + 11, CTRL_Y - 10, BTN_PREV_X + 11, CTRL_Y + 11,
                  BTN_PREV_X - 4, CTRL_Y, COL_TEXT)
    # 下一首：竖条 + 右指三角
    s.fill_rect(BTN_NEXT_X + 8, CTRL_Y - 10, 3, 21, COL_TEXT)
    fill_triangle(s, BTN_NEXT_X - 11, CTRL_Y - 10, BTN_NEXT_X - 11, CTRL_Y + 11,
                  BTN_NEXT_X + 4, CTRL_Y, COL_TEXT)
    # 播放 / 暂停
    fill_circle(s, BTN_PLAY_X, CTRL_Y, BTN_PLAY_R, COL_ACCENT)
    if playing:
        s.fill_rect(BTN_PLAY_X - 9, CTRL_Y - 12, 7, 24, COL_BG)
        s.fill_rect(BTN_PLAY_X + 2, CTRL_Y - 12, 7, 24, COL_BG)
    else:
        fill_triangle(s, BTN_PLAY_X - 7, CTRL_Y - 13, BTN_PLAY_X - 7, CTRL_Y + 13,
                      BTN_PLAY_X + 14, CTRL_Y, COL_BG)


def draw_note_icon(s, cx, cy, scale, color):
    fill_circle(s, cx - scale, cy + scale, scale, color)
    s.fill_rect(cx - scale // 2, cy - scale * 2, scale // 2 + 1, scale * 3, color)
    fill_triangle(s, cx - scale // 2, cy - scale * 2,
                  cx - scale // 2, cy - scale,
                  cx + scale, cy - scale * 2 + scale // 2, color)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=".screenshots")
    ap.add_argument("--title", default="晴天")
    ap.add_argument("--artist", default="周杰伦")
    ap.add_argument("--album", default="叶惠美 · 七里香")
    ap.add_argument("--progress", type=float, default=0.62)
    args = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    title = Font(os.path.join(here, "src", "font_title_30.cpp"))
    body = Font(os.path.join(here, "src", "font_body_22.cpp"))
    num = Font(os.path.join(here, "src", "font_num_52.cpp"))
    os.makedirs(os.path.join(here, args.out), exist_ok=True)

    # ---- 播放中 ----
    s = Surface()
    draw_ring(s, 0, 360, COL_RING)
    draw_ring(s, 0, args.progress * 360, COL_ACCENT)
    w = body.width("Apple Music")
    centered(s, body, HEADER_Y, COL_DIM, "Apple Music")
    fill_circle(s, (SCREEN_W - w) // 2 - 13, HEADER_Y + body.ascent // 2, 4, COL_ACCENT)
    one_line = title.width(args.title) <= TITLE_MAX_W
    centered(s, title, TITLE_Y1 + (12 if one_line else 0), COL_TEXT, args.title)
    centered(s, body, ARTIST_Y, COL_TEXT, args.artist)
    centered(s, body, ALBUM_Y, COL_DIM, args.album)
    centered(s, body, TIME_Y, COL_DIM, "1:23/4:29")
    draw_controls(s, True)
    playing = os.path.join(here, args.out, "media_face_aa_playing.png")
    to_image(s.fb).save(playing)

    # ---- 配对 ----
    p = Surface()
    draw_ring(p, 0, 360, COL_RING)
    centered(p, body, HEADER_Y, COL_DIM, "正在配对")
    centered(p, body, 150, COL_DIM, "配对码")
    centered(p, num, 168, COL_TEXT, "482 913")
    centered(p, body, 264, COL_DIM, "在 iPhone 上输入此配对码")
    pairing = os.path.join(here, args.out, "media_face_aa_pairing.png")
    to_image(p.fb).save(pairing)

    # ---- 空闲 ----
    i = Surface()
    draw_ring(i, 0, 360, COL_RING)
    centered(i, body, HEADER_Y, COL_DIM, "等待 iPhone 连接")
    draw_note_icon(i, CX, 176, 9, COL_RING)
    centered(i, title, 214, COL_TEXT, "Orbis")
    centered(i, body, 264, COL_DIM, "等待 iPhone 配对")
    centered(i, body, 296, COL_DIMMER, "设置 → 蓝牙 → Orbis")
    idle = os.path.join(here, args.out, "media_face_aa_idle.png")
    to_image(i.fb).save(idle)

    for path in (playing, pairing, idle):
        print(path)


if __name__ == "__main__":
    main()
