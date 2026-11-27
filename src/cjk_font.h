#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ============================================================
// 2bpp 灰阶点阵 CJK 字体（中文 / 日文 / 拉丁）
//
// Arduino_GFX 自带字体只有 ASCII，歌名和歌手几乎一定是中日文，
// 所以这里用 tools/gen_font.py 从系统字体生成点阵：
//   FONT_TITLE 30px  标题
//   FONT_BODY  22px  歌手 / 专辑 / 时间 / 状态
//   FONT_NUM   52px  配对码数字（只含 0-9 和空格）
// 每个字形是「紧贴墨迹」的 2bpp 位图（4 级覆盖度，4 像素/字节，MSB 在左）
// + 相对行顶的偏移。绘制时按覆盖度与画布已有颜色做 alpha 混合 —— 1-bit 阈值
// 会把 FreeType 的灰阶边缘削掉，细笔画发虚；灰阶混合后笔画连续、边缘无台阶。
// ============================================================

struct CjkGlyph
{
  uint32_t cp;
  uint32_t off;   // 位图在 bits 中的字节偏移
  uint8_t w, h;   // 位图宽高（行按字节对齐，4 像素/字节，MSB 在左）
  int8_t xoff, yoff;
  uint8_t adv;    // 前进宽度
};

struct CjkFont
{
  const CjkGlyph *glyphs;
  const uint8_t *bits;
  uint16_t count;
  uint8_t lineHeight;
  uint8_t ascent;
};

// 绘制目标：Arduino_Canvas 的 RGB565 framebuffer（rotation 0）。
// alpha 混合必须直接读写像素，走不了 Arduino_GFX 的 API；抗锯齿几何
// （media_face.cpp 里的圆环 / 圆形 / 三角形）也复用这套像素混合。
struct CjkSurface
{
  uint16_t *fb;      // canvas framebuffer；为空时退化为逐像素写面板（背景按黑处理）
  int16_t w, h;
  Arduino_GFX *gfx;  // 仅 fb 为空时使用
};

extern const CjkFont FONT_TITLE;
extern const CjkFont FONT_BODY;
extern const CjkFont FONT_NUM;

// UTF-8 解码：返回当前码位并把 s 前移；非法字节按 1 字节跳过
uint32_t cjkNextCodepoint(const char *&s);

const CjkGlyph *cjkFind(const CjkFont &font, uint32_t cp);

int cjkTextWidth(const CjkFont &font, const char *utf8);

// 把 color 按覆盖率混合到像素上：cov 以 1/16 为单位（0 = 不着色，16 = 直接覆盖）
void cjkBlendPixel(const CjkSurface &surf, int16_t x, int16_t y, uint16_t color, uint8_t cov);

// yTop = 行顶（ascent 之上），返回本行实际宽度
int cjkDrawText(const CjkSurface &surf, int x, int yTop, const CjkFont &font,
                uint16_t color, const char *utf8);

// 超出 maxWidth 时用 … 截断（ellipsis=true），否则直接不画超出的部分
int cjkDrawTextClipped(const CjkSurface &surf, int x, int yTop, const CjkFont &font,
                       uint16_t color, const char *utf8, int maxWidth,
                       bool ellipsis = true);

// 折行：最多 maxLines 行；放不下时最后一行以 … 结尾。
// out 是 maxLines 个长度为 stride 的缓冲；返回实际行数。
int cjkWrapText(const CjkFont &font, const char *utf8, int maxWidth, int maxLines,
                char *out, int stride);
