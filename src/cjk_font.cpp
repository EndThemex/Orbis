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

// 2bpp 覆盖度 → 1/16 覆盖率（0, 1/3, 2/3, 1 近似为 0, 5/16, 11/16, 1）
static const uint8_t COVERAGE[4] = {0, 5, 11, 16};

// RGB565 双通道并行混合：cov 以 1/16 为单位，乘 16 后按字段对齐右移 4 位，
// 5/6/5 各字段之间有足够空位，不会互相进位。
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t cov)
{
  const uint32_t inv = 16u - cov;
  const uint32_t rb = ((uint32_t)(src & 0xF81F) * cov + (uint32_t)(dst & 0xF81F) * inv) >> 4;
  const uint32_t g = ((uint32_t)(src & 0x07E0) * cov + (uint32_t)(dst & 0x07E0) * inv) >> 4;
  return (uint16_t)((rb & 0xF81F) | (g & 0x07E0));
}

void cjkBlendPixel(const CjkSurface &surf, int16_t x, int16_t y, uint16_t color, uint8_t cov)
{
  if (cov == 0 || x < 0 || y < 0 || x >= surf.w || y >= surf.h)
    return;

  if (surf.fb)
  {
    uint16_t *p = surf.fb + (size_t)y * surf.w + x;
    *p = (cov >= 16) ? color : blend565(*p, color, cov);
  }
  else if (surf.gfx)
  {
    // 退化路径：拿不到 framebuffer，只能按黑底混合后逐像素写面板
    surf.gfx->writePixel(x, y, (cov >= 16) ? color : blend565(0, color, cov));
  }
}

static void drawGlyph(const CjkSurface &surf, int x, int yTop, const CjkFont &font,
                      uint16_t color, const CjkGlyph *g)
{
  if (!g || g->w == 0 || g->h == 0)
    return;

  const int gx = x + g->xoff;
  const int gy = yTop + g->yoff;
  const int stride = (g->w + 3) >> 2;

  int x0 = 0, x1 = g->w;
  if (gx < 0)
    x0 = -gx;
  if (gx + g->w > surf.w)
    x1 = surf.w - gx;
  int y0 = 0, y1 = g->h;
  if (gy < 0)
    y0 = -gy;
  if (gy + g->h > surf.h)
    y1 = surf.h - gy;

  for (int yy = y0; yy < y1; yy++)
  {
    const uint8_t *src = font.bits + g->off + (size_t)yy * stride;
    const int py = gy + yy;
    for (int xx = x0; xx < x1; xx++)
    {
      const uint8_t cov = COVERAGE[(src[xx >> 2] >> (6 - 2 * (xx & 3))) & 0x03];
      if (cov)
        cjkBlendPixel(surf, gx + xx, py, color, cov);
    }
  }
}

int cjkDrawText(const CjkSurface &surf, int x, int yTop, const CjkFont &font,
                uint16_t color, const char *utf8)
{
  int pen = x;
  while (*utf8)
  {
    const CjkGlyph *g = cjkFind(font, cjkNextCodepoint(utf8));
    drawGlyph(surf, pen, yTop, font, color, g);
    pen += glyphAdvance(font, g);
  }
  return pen - x;
}

// 带亚像素横向偏移的字形：把每列覆盖率按 1/16 权重摊到相邻两列后再混合一次，
// 这样滚动位置可以落在像素之间，视觉上才是匀速滑动。
static void drawGlyphSubpixel(const CjkSurface &surf, int x, int yTop, const CjkFont &font,
                             uint16_t color, const CjkGlyph *g, uint8_t frac)
{
  if (!g || g->w == 0 || g->h == 0 || frac == 0)
  {
    drawGlyph(surf, x, yTop, font, color, g);
    return;
  }

  const int gx = x + g->xoff;
  const int gy = yTop + g->yoff;
  const int stride = (g->w + 3) >> 2;
  // 一列最多同时收到左右两列字形的贡献，最多 16 + 16，用 uint8 累加不会溢出
  uint8_t acc[64];
  const int cols = g->w < (int)sizeof(acc) - 1 ? g->w : (int)sizeof(acc) - 1;
  const uint8_t inv = (uint8_t)(16 - frac);

  for (int yy = 0; yy < g->h; yy++)
  {
    const int py = gy + yy;
    if (py < 0 || py >= surf.h)
      continue;

    memset(acc, 0, (size_t)cols + 1);
    const uint8_t *src = font.bits + g->off + (size_t)yy * stride;
    for (int xx = 0; xx < cols; xx++)
    {
      const uint8_t c = COVERAGE[(src[xx >> 2] >> (6 - 2 * (xx & 3))) & 0x03];
      if (!c)
        continue;
      acc[xx] += (uint8_t)((c * inv + 8) >> 4);
      acc[xx + 1] += (uint8_t)((c * frac + 8) >> 4);
    }

    for (int xx = 0; xx <= cols; xx++)
    {
      uint8_t cov = acc[xx];
      if (!cov)
        continue;
      if (cov > 16)
        cov = 16;
      cjkBlendPixel(surf, gx + xx, py, color, cov);
    }
  }
}

int cjkDrawTextSubpixel(const CjkSurface &surf, int x, int yTop, const CjkFont &font,
                        uint16_t color, const char *utf8, uint8_t subpx)
{
  const uint8_t frac = subpx >> 4;   // 0..15，1/16 像素
  int pen = x;
  while (*utf8)
  {
    const CjkGlyph *g = cjkFind(font, cjkNextCodepoint(utf8));
    drawGlyphSubpixel(surf, pen, yTop, font, color, g, frac);
    pen += glyphAdvance(font, g);
  }
  return pen - x;
}

int cjkDrawTextClipped(const CjkSurface &surf, int x, int yTop, const CjkFont &font,
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
        drawGlyph(surf, pen, yTop, font, color, dots);
        pen += dotsAdv;
      }
      break;
    }

    drawGlyph(surf, pen, yTop, font, color, g);
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
