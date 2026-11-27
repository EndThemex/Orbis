#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ============================================================
// 1-bit 点阵 CJK 字体（中文 / 日文 / 拉丁）
//
// Arduino_GFX 自带字体只有 ASCII，歌名和歌手几乎一定是中日文，
// 所以这里用 tools/gen_font.py 从系统字体生成点阵：
//   FONT_TITLE 30px  标题
//   FONT_BODY  20px  歌手 / 专辑 / 时间 / 状态
// 每个字形是「紧贴墨迹」的 1-bit 位图 + 相对行顶的偏移，
// 绘制时用 drawBitmap 逐字贴到画布上。
// ============================================================

struct CjkGlyph
{
  uint32_t cp;
  uint32_t off;   // 位图在 bits 中的字节偏移
  uint8_t w, h;   // 位图宽高（行按字节对齐，MSB 在左）
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

extern const CjkFont FONT_TITLE;
extern const CjkFont FONT_BODY;

// UTF-8 解码：返回当前码位并把 s 前移；非法字节按 1 字节跳过
uint32_t cjkNextCodepoint(const char *&s);

const CjkGlyph *cjkFind(const CjkFont &font, uint32_t cp);

int cjkTextWidth(const CjkFont &font, const char *utf8);

// yTop = 行顶（ascent 之上），返回本行实际宽度
int cjkDrawText(Arduino_GFX *gfx, int x, int yTop, const CjkFont &font,
                uint16_t color, const char *utf8);

// 超出 maxWidth 时用 … 截断（ellipsis=true），否则直接不画超出的部分
int cjkDrawTextClipped(Arduino_GFX *gfx, int x, int yTop, const CjkFont &font,
                       uint16_t color, const char *utf8, int maxWidth,
                       bool ellipsis = true);

// 折行：最多 maxLines 行；放不下时最后一行以 … 结尾。
// out 是 maxLines 个长度为 stride 的缓冲；返回实际行数。
int cjkWrapText(const CjkFont &font, const char *utf8, int maxWidth, int maxLines,
                char *out, int stride);
