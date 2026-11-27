#include "cjk_font.h"

// UTF-8 解码：只处理到 4 字节，非法序列按单字节跳过
uint32_t cjkNextCodepoint(const char *&s)
{
  const uint8_t c = (uint8_t)*s;
  if (c < 0x80)
  {
    s++;
    return c;
  }

  int extra;
  uint32_t cp;
  if ((c & 0xE0) == 0xC0)
  {
    extra = 1;
    cp = c & 0x1F;
  }
  else if ((c & 0xF0) == 0xE0)
  {
    extra = 2;
    cp = c & 0x0F;
  }
  else if ((c & 0xF8) == 0xF0)
  {
    extra = 3;
    cp = c & 0x07;
  }
  else
  {
    s++;
    return '?';
  }

  for (int i = 1; i <= extra; i++)
  {
    const uint8_t n = (uint8_t)s[i];
    if ((n & 0xC0) != 0x80)
    {
      s += i;   // 序列被截断，吞掉已经读过的字节
      return '?';
    }
    cp = (cp << 6) | (n & 0x3F);
  }
  s += extra + 1;
  return cp;
}

const CjkGlyph *cjkFind(const CjkFont &font, uint32_t cp)
{
  int lo = 0, hi = (int)font.count - 1;
  while (lo <= hi)
  {
    const int mid = (lo + hi) >> 1;
    const uint32_t v = font.glyphs[mid].cp;
    if (v == cp)
      return &font.glyphs[mid];
    if (v < cp)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return nullptr;
}

// 找不到的字形按半字宽占位，避免文字挤在一起
static inline int glyphAdvance(const CjkFont &font, const CjkGlyph *g)
{
  return g ? g->adv : (font.ascent / 2);
}

// 追加到缓冲的 UTF-8 省略号
static const char ELLIPSIS[] = "\xE2\x80\xA6";

int cjkTextWidth(const CjkFont &font, const char *utf8)
{
  int w = 0;
  while (*utf8)
    w += glyphAdvance(font, cjkFind(font, cjkNextCodepoint(utf8)));
  return w;
}

static void drawGlyph(Arduino_GFX *gfx, int x, int yTop, const CjkFont &font,
                      uint16_t color, const CjkGlyph *g)
{
  if (!g || g->w == 0 || g->h == 0)
    return;
  gfx->drawBitmap(x + g->xoff, yTop + g->yoff, font.bits + g->off, g->w, g->h, color);
}

int cjkDrawText(Arduino_GFX *gfx, int x, int yTop, const CjkFont &font,
                uint16_t color, const char *utf8)
{
  int pen = x;
  while (*utf8)
  {
    const CjkGlyph *g = cjkFind(font, cjkNextCodepoint(utf8));
    drawGlyph(gfx, pen, yTop, font, color, g);
    pen += glyphAdvance(font, g);
  }
  return pen - x;
}

int cjkDrawTextClipped(Arduino_GFX *gfx, int x, int yTop, const CjkFont &font,
                       uint16_t color, const char *utf8, int maxWidth, bool ellipsis)
{
  const CjkGlyph *dots = cjkFind(font, 0x2026);
  const int dotsAdv = dots ? dots->adv : font.ascent / 2;

  int pen = x;
  while (*utf8)
  {
    const CjkGlyph *g = cjkFind(font, cjkNextCodepoint(utf8));
    const int adv = glyphAdvance(font, g);

    if (pen - x + adv > maxWidth)
    {
      if (ellipsis && dots && pen - x + dotsAdv <= maxWidth)
      {
        drawGlyph(gfx, pen, yTop, font, color, dots);
        pen += dotsAdv;
      }
      break;
    }

    drawGlyph(gfx, pen, yTop, font, color, g);
    pen += adv;
  }
  return pen - x;
}

int cjkWrapText(const CjkFont &font, const char *utf8, int maxWidth, int maxLines,
                char *out, int stride)
{
  int line = 0;
  const char *p = utf8;
  const CjkGlyph *dots = cjkFind(font, 0x2026);
  const int dotsAdv = dots ? dots->adv : font.ascent / 2;

  while (*p == '\n')
    p++;

  while (*p && line < maxLines)
  {
    int width = 0, bytes = 0;
    const char *cursor = p;
    const char *lastSpace = nullptr;
    bool any = false, overflow = false;

    while (*cursor)
    {
      const char *before = cursor;
      const uint32_t cp = cjkNextCodepoint(cursor);
      if (cp == '\n')
        break;

      const int adv = glyphAdvance(font, cjkFind(font, cp));
      if (any && width + adv > maxWidth)
      {
        cursor = before;   // 这一行到此为止
        overflow = true;
        break;
      }

      width += adv;
      bytes = (int)(cursor - p);
      any = true;
      if (cp == ' ')
        lastSpace = cursor;
    }

    // 拉丁文优先在空格处断行（空格前必须有内容）
    if (overflow && lastSpace && lastSpace > p)
    {
      cursor = lastSpace;
      bytes = (int)(cursor - p);
    }

    // 去掉行尾空格
    while (bytes > 0 && (p[bytes - 1] == ' ' || p[bytes - 1] == '\r'))
      bytes--;

    if (bytes > stride - 1)
      bytes = stride - 1;
    while (bytes > 0 && ((uint8_t)p[bytes] & 0xC0) == 0x80)   // 别切断 UTF-8
      bytes--;

    char *dst = out + (size_t)line * stride;
    memcpy(dst, p, bytes);
    dst[bytes] = 0;

    const bool more = (*cursor != 0);
    if (more && line == maxLines - 1 && bytes + 3 < stride)
    {
      // 最后一行还有剩余内容：能放下就补省略号
      const int used = cjkTextWidth(font, dst);
      if (used + dotsAdv <= maxWidth)
      {
        memcpy(dst + bytes, ELLIPSIS, 3);
        dst[bytes + 3] = 0;
      }
    }

    line++;
    p = cursor;
    while (*p == ' ' || *p == '\r')
      p++;
    if (*p == '\n')
      p++;
  }

  if (line == 0)
  {
    out[0] = 0;   // 空串也占一行
    return 1;
  }
  return line;
}
