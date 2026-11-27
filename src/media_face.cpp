#include "media_face.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "cjk_font.h"
#include "config.h"

// ---------------- 几何 ----------------
static const int16_t CX = SCREEN_W / 2;
static const int16_t CY = SCREEN_H / 2;

static const int16_t RING_R_OUT = 230;
static const int16_t RING_R_IN = 224;

static const int16_t HEADER_Y = 78;
static const int16_t TITLE_Y1 = 126;
static const int16_t TITLE_Y2 = 162;
static const int16_t TITLE_MAX_W = 336;
static const int16_t ARTIST_Y = 208;
static const int16_t ALBUM_Y = 240;

static const int16_t CTRL_Y = 352;
static const int16_t TIME_Y = 408;
static const int16_t BTN_PREV_X = 132;
static const int16_t BTN_NEXT_X = 334;
static const int16_t BTN_PLAY_X = 233;
static const int16_t BTN_SIDE_R = 37;
static const int16_t BTN_PLAY_R = 42;

// ---------------- 滚动文字 ----------------
// 歌名 / 歌手 / 专辑放不下时不再用省略号截断，改为窗口内横向滚动。
// 滚动只重绘那条文字带子（不整屏重绘、不整屏 flush），否则 40px 文字以
// 30fps 走路会把 BLE 和 QSPI 总线全吃满。
static const int16_t SCROLL_W = 336;          // 滚动窗口宽度（居中，不压到进度环）
static const int16_t SCROLL_INSET = 4;        // 起笔留白，避免首字左侧溢出被切
static const int16_t SCROLL_SPEED = 34;       // 滚动速度 px/s
static const int16_t SCROLL_GAP = 56;         // 两轮文字之间的空隙
static const uint32_t SCROLL_PAUSE_MS = 1400; // 每轮开始前的停顿
static const int16_t SCROLL_MAX_H = 40;       // 带区最大高度（FONT_TITLE.lineHeight）

// ---------------- 刷新节奏 ----------------
// 整帧（434KB）flush 在 QSPI 上要几十毫秒，按 250ms 整帧重绘会让进度环和
// 滚动文字都变成「一格一格跳」。现在只在内容变化时整屏重绘，其余时间按
// 固定 30fps 做局部刷新：进度环只补脏扇区、时间文字只在秒变化时重画、
// 滚动文字带各自推自己那一条。
static const uint32_t FRAME_MS = 33;               // 增量刷新周期（约 30fps）
static const float DEG2RAD_F = 0.0174532925f;      // 角度 → 弧度
static const float RING_TIP_MARGIN_DEG = 1.0f;     // 弧尖两侧多补的角度（约 4px）
static const float RING_REDRAW_DEG = 6.0f;         // 单帧跳变超过这个角度就整屏重绘

enum
{
  SLOT_TITLE,
  SLOT_ARTIST,
  SLOT_ALBUM,
  SLOT_COUNT
};

// ---------------- 配色 ----------------
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b) >> 3)))

static const uint16_t COL_BG = RGB565(0, 0, 0);
static const uint16_t COL_RING = RGB565(38, 46, 60);
static const uint16_t COL_ACCENT = RGB565(0, 214, 255);
static const uint16_t COL_TEXT = RGB565(232, 238, 248);
static const uint16_t COL_DIM = RGB565(176, 188, 208);
static const uint16_t COL_DIMMER = RGB565(104, 116, 136);
static const uint16_t COL_WARN = RGB565(255, 176, 32);

// ---------------- 抗锯齿 ----------------
// 全部直接按覆盖率混合到 canvas framebuffer：Arduino_GFX 的圆形/三角形都是
// Bresenham 硬边，466px 圆屏上台阶非常明显，这里改成逐像素算覆盖率。
// d = 到形状边缘的距离（像素，正值在形状内部）；返回 1/16 覆盖率
static inline uint8_t coverage(float d)
{
  if (d <= -0.5f)
    return 0;
  if (d >= 0.5f)
    return 16;
  return (uint8_t)((d + 0.5f) * 16.0f + 0.5f);
}

static inline int16_t imin(int16_t a, int16_t b) { return a < b ? a : b; }
static inline int16_t imax(int16_t a, int16_t b) { return a > b ? a : b; }

static void fillCircleAA(const CjkSurface &s, int16_t cx, int16_t cy, int16_t r, uint16_t color)
{
  const float edge = r + 0.5f;
  const float r2 = edge * edge;
  for (int16_t y = cy - r - 1; y <= cy + r + 1; y++)
  {
    const float dy = y + 0.5f - cy;
    for (int16_t x = cx - r - 1; x <= cx + r + 1; x++)
    {
      const float dx = x + 0.5f - cx;
      const float d2 = dx * dx + dy * dy;
      if (d2 > r2)
        continue;
      cjkBlendPixel(s, x, y, color, coverage(r - sqrtf(d2)));
    }
  }
}

static void strokeCircleAA(const CjkSurface &s, int16_t cx, int16_t cy, float r,
                           float width, uint16_t color)
{
  const float half = width * 0.5f;
  const float ro = r + half, ri = r - half;
  const float ro2 = (ro + 0.5f) * (ro + 0.5f);
  const float ri2 = (ri - 0.5f) * (ri - 0.5f);
  const int16_t yr = (int16_t)ceilf(ro + 0.5f);
  for (int16_t y = cy - yr; y <= cy + yr; y++)
  {
    const float dy = y + 0.5f - cy;
    for (int16_t x = cx - yr; x <= cx + yr; x++)
    {
      const float dx = x + 0.5f - cx;
      const float d2 = dx * dx + dy * dy;
      if (d2 > ro2 || d2 < ri2)
        continue;
      const float dist = sqrtf(d2);
      cjkBlendPixel(s, x, y, color, coverage(fminf(ro - dist, dist - ri)));
    }
  }
}

// 点到有向边的距离；inside 表示三角形第三个顶点在边的哪一侧
static inline float edgeDist(float px, float py, float qx, float qy, float tx, float ty,
                             float x, float y)
{
  const float ex = qx - px, ey = qy - py;
  const float len = sqrtf(ex * ex + ey * ey);
  if (len <= 0.0f)
    return 1e9f;
  const float d = (ex * (y - py) - ey * (x - px)) / len;
  return ((ex * (ty - py) - ey * (tx - px)) < 0.0f) ? -d : d;
}

static void fillTriangleAA(const CjkSurface &s, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                           int16_t x2, int16_t y2, uint16_t color)
{
  const int16_t xa = imin(imin(x0, x1), x2), xb = imax(imax(x0, x1), x2);
  const int16_t ya = imin(imin(y0, y1), y2), yb = imax(imax(y0, y1), y2);
  for (int16_t y = ya; y <= yb; y++)
  {
    const float py = y + 0.5f;
    for (int16_t x = xa; x <= xb; x++)
    {
      const float px = x + 0.5f;
      const float d = fminf(edgeDist(x0, y0, x1, y1, x2, y2, px, py),
                            fminf(edgeDist(x1, y1, x2, y2, x0, y0, px, py),
                                  edgeDist(x2, y2, x0, y0, x1, y1, px, py)));
      if (d <= -0.5f)
        continue;
      cjkBlendPixel(s, x, y, color, coverage(d));
    }
  }
}

// 圆环扇区的一段（同一行只跑环带内的像素，避免整圆 bbox 全扫）
static void ringSpanC(const CjkSurface &s, int16_t y, float dy, int16_t xFrom, int16_t xTo,
                      float deg)
{
  for (int16_t x = xFrom; x <= xTo; x++)
  {
    const float dx = x + 0.5f - CX;
    const float dist = sqrtf(dx * dx + dy * dy);
    const uint8_t rc = coverage(fminf(RING_R_OUT - dist, dist - RING_R_IN));
    if (rc == 0)
      continue;

    // 先铺底环，再把弧尖之前的部分叠成强调色
    cjkBlendPixel(s, x, y, COL_RING, rc);
    if (deg <= 0.0f)
      continue;

    // 角度自 12 点起顺时针；用弧长（像素）做覆盖率，弧尖走动才是亚像素平滑的
    float ang = atan2f(dx, -dy) * RAD_TO_DEG;
    if (ang < 0.0f)
      ang += 360.0f;
    const uint8_t ac = coverage(dist * (deg - ang) * DEG2RAD_F);
    if (ac == 0)
      continue;
    const uint8_t cov = (uint8_t)(((uint16_t)rc * ac + 8) >> 4);
    if (cov)
      cjkBlendPixel(s, x, y, COL_ACCENT, cov);
  }
}

static void fillRectFb(const CjkSurface &s, int16_t x, int16_t y, int16_t w, int16_t h,
                       uint16_t color)
{
  if (!s.fb || w <= 0 || h <= 0)
    return;
  if (x < 0)
  {
    w += x;
    x = 0;
  }
  if (y < 0)
  {
    h += y;
    y = 0;
  }
  if (x + w > s.w)
    w = s.w - x;
  if (y + h > s.h)
    h = s.h - y;
  if (w <= 0 || h <= 0)
    return;
  for (int16_t row = 0; row < h; row++)
  {
    uint16_t *p = s.fb + (size_t)(y + row) * s.w + x;
    for (int16_t i = 0; i < w; i++)
      p[i] = color;
  }
}

// 环形扇区的包围盒：两端点（内外半径）+ 落在区间内的正方向极值，四周留 2px 给抗锯齿
static void wedgeBBox(float a0, float a1, int16_t &bx0, int16_t &by0, int16_t &bx1, int16_t &by1)
{
  float xs[10], ys[10];
  int n = 0;
  const float radii[2] = {(float)RING_R_OUT, (float)RING_R_IN};
  for (int k = 0; k < 2; k++)
    for (int e = 0; e < 2; e++)
    {
      const float rad = (e ? a1 : a0) * DEG2RAD_F;
      xs[n] = CX + radii[k] * sinf(rad);
      ys[n] = CY - radii[k] * cosf(rad);
      n++;
    }
  for (int q = 0; q <= 4; q++)
  {
    const float a = (float)q * 90.0f;
    if (a < a0 || a > a1)
      continue;
    const float rad = a * DEG2RAD_F;
    xs[n] = CX + RING_R_OUT * sinf(rad);
    ys[n] = CY - RING_R_OUT * cosf(rad);
    n++;
  }

  float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
  for (int i = 1; i < n; i++)
  {
    x0 = fminf(x0, xs[i]);
    x1 = fmaxf(x1, xs[i]);
    y0 = fminf(y0, ys[i]);
    y1 = fmaxf(y1, ys[i]);
  }
  bx0 = (int16_t)floorf(x0) - 3;
  by0 = (int16_t)floorf(y0) - 3;
  bx1 = (int16_t)ceilf(x1) + 3;
  by1 = (int16_t)ceilf(y1) + 3;
}

// 重绘矩形内的整条进度环（底色环 + 进度弧），弧尖在 deg 处。
// 只重绘一个脏矩形时，矩形里除了环没有别的内容，先清成底色再画是安全的。
static void paintRingRect(const CjkSurface &s, int16_t bx0, int16_t by0, int16_t bx1,
                          int16_t by1, float deg, bool clear)
{
  if (bx0 < 0)
    bx0 = 0;
  if (by0 < 0)
    by0 = 0;
  if (bx1 > SCREEN_W - 1)
    bx1 = SCREEN_W - 1;
  if (by1 > SCREEN_H - 1)
    by1 = SCREEN_H - 1;
  if (bx0 > bx1 || by0 > by1)
    return;

  if (clear)
    fillRectFb(s, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1, COL_BG);

  const float ro2 = (RING_R_OUT + 0.5f) * (RING_R_OUT + 0.5f);
  const float ri2 = (RING_R_IN - 0.5f) * (RING_R_IN - 0.5f);

  for (int16_t y = by0; y <= by1; y++)
  {
    const float dy = y + 0.5f - CY;
    const float dy2 = dy * dy;
    if (dy2 > ro2)
      continue;

    const int16_t xo = (int16_t)sqrtf(ro2 - dy2);
    const int16_t xi = (dy2 < ri2) ? (int16_t)sqrtf(ri2 - dy2) : 0;
    const int16_t l0 = imax(bx0, CX - xo), l1 = imin(bx1, CX - xi);
    const int16_t r0 = imax(bx0, CX + xi), r1 = imin(bx1, CX + xo);
    if (xi > 0)
    {
      if (l0 <= l1)
        ringSpanC(s, y, dy, l0, l1, deg);
      if (r0 <= r1)
        ringSpanC(s, y, dy, r0, r1, deg);
    }
    else
    {
      // 内圆还没张开（环的上下两端），整行只画一次
      if (l0 <= r1)
        ringSpanC(s, y, dy, l0, r1, deg);
    }
  }
}

static int drawCentered(const CjkSurface &s, const CjkFont &font, int yTop,
                        uint16_t color, const char *text)
{
  const int w = cjkTextWidth(font, text);
  return cjkDrawText(s, (SCREEN_W - w) / 2, yTop, font, color, text);
}

static void formatTime(float seconds, char *out, size_t cap)
{
  int total = (int)(seconds + 0.5f);
  if (total < 0)
    total = 0;
  const int h = total / 3600;
  const int m = (total % 3600) / 60;
  const int s = total % 60;
  if (h > 0)
    snprintf(out, cap, "%d:%02d:%02d", h, m, s);
  else
    snprintf(out, cap, "%d:%02d", m, s);
}

// ---------------- MediaFace ----------------

void MediaFace::begin(Arduino_GFX *panel, Arduino_Canvas *canvas, AmsClient *ams)
{
  _panel = panel;
  _canvas = canvas;
  _gfx = canvas ? (Arduino_GFX *)canvas : panel;
  _ams = ams;

  // 抗锯齿混合要直接写像素，只有 rotation 0 的 canvas 帧缓冲才是连续 RGB565
  _surf.fb = (canvas && canvas->getRotation() == 0) ? canvas->getFramebuffer() : nullptr;
  _surf.w = SCREEN_W;
  _surf.h = SCREEN_H;
  _surf.gfx = panel;

  _forceDraw = true;

  // 滚动文字的带区缓冲（336x40x2 ≈ 26KB）
  _bandBuf = (uint16_t *)malloc((size_t)SCROLL_W * SCROLL_MAX_H * 2);
  if (!_bandBuf)
    Serial.println("[UI] marquee buffer alloc FAILED, 超长文字仍用省略号");
}

void MediaFace::drawNoteIcon(int16_t cx, int16_t cy, int16_t scale, uint16_t color)
{
  // 简化的八分音符：符头 + 符干 + 旗
  fillCircleAA(_surf, cx - scale, cy + scale, scale, color);
  _gfx->fillRect(cx - scale / 2, cy - scale * 2, scale / 2 + 1, scale * 3, color);
  fillTriangleAA(_surf, cx - scale / 2, cy - scale * 2,
                 cx - scale / 2, cy - scale,
                 cx + scale, cy - scale * 2 + scale / 2, color);
}

void MediaFace::drawHeader(AmsLinkState link, const MediaInfo &info)
{
  const char *text = "";
  uint16_t dot = COL_DIMMER;

  switch (link)
  {
  case AMS_LINK_ADVERTISING:
    text = "等待 iPhone 连接";
    dot = COL_WARN;
    break;
  case AMS_LINK_PAIRING:
    text = "正在配对";
    dot = COL_WARN;
    break;
  case AMS_LINK_CONNECTED:
    text = "正在读取媒体服务";
    dot = COL_ACCENT;
    break;
  case AMS_LINK_READY:
  default:
    text = info.app[0] ? info.app : "已连接 iPhone";
    dot = COL_ACCENT;
    break;
  }

  const int w = cjkTextWidth(FONT_BODY, text);
  const int x = (SCREEN_W - w) / 2;
  cjkDrawText(_surf, x, HEADER_Y, FONT_BODY, COL_DIM, text);
  fillCircleAA(_surf, x - 13, HEADER_Y + FONT_BODY.ascent / 2, 4, dot);
}

void MediaFace::drawScrollingText(int slot, const CjkFont &font, int16_t yTop, uint16_t color,
                                  const char *text)
{
  Marquee &m = _marquee[slot];
  m.font = &font;
  m.yTop = yTop;
  m.color = color;

  const int w = cjkTextWidth(font, text);
  if (w <= SCROLL_W)
  {
    // 放得下：照旧居中静态显示
    m.active = false;
    m.text[0] = 0;
    cjkDrawText(_surf, (SCREEN_W - w) / 2, yTop, font, color, text);
    return;
  }

  if (!_bandBuf)
  {
    // 没有滚动缓冲，退回省略号截断
    m.active = false;
    cjkDrawTextClipped(_surf, (SCREEN_W - SCROLL_W) / 2, yTop, font, color, text, SCROLL_W);
    return;
  }

  // 文字变了就从头开始滚
  if (strncmp(m.text, text, sizeof(m.text) - 1) != 0)
  {
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = 0;
    m.startMs = millis();
  }
  m.active = true;
  paintMarquee(slot, false);
}

void MediaFace::paintMarquee(int slot, bool toPanel)
{
  Marquee &m = _marquee[slot];
  if (!m.active || !m.font || !_bandBuf)
    return;

  const CjkFont &font = *m.font;
  const int16_t bw = SCROLL_W;
  const int16_t bh = font.lineHeight;
  const int textW = cjkTextWidth(font, m.text);
  const int span = textW + SCROLL_GAP;

  // 每轮先停顿，再匀速左移；位置按时间算成 1/16 像素，刷新间隔抖动不会改变速度，
  // 小数部分交给亚像素绘制，滚动才是连续的而不是整数像素跳格
  const uint32_t elapsed = millis() - m.startMs;
  int32_t pos16 = 0;
  if (elapsed > SCROLL_PAUSE_MS)
    pos16 = (int32_t)(((uint64_t)(elapsed - SCROLL_PAUSE_MS) * (uint32_t)SCROLL_SPEED * 16u) /
                      1000u) %
            (int32_t)(span * 16);
  // 位置 = -pos16/16（向左匀速），其中小数部分靠「整像素位置向下取整 + 亚像素右移」实现
  const int16_t pos = (int16_t)((pos16 + 15) >> 4);
  const uint8_t subpx = (uint8_t)(((16 - (pos16 & 15)) & 15) << 4);

  // 每轮开头的停顿期间位置不变，没必要重复渲染 + 推屏
  if (toPanel && m.lastPos16 == pos16)
    return;
  m.lastPos16 = pos16;

  memset(_bandBuf, 0, (size_t)bw * bh * 2);   // 带区底色是纯黑，直接清零

  CjkSurface band = {_bandBuf, bw, bh, nullptr};
  for (int x = -pos; x < bw; x += span)
  {
    if (x + textW <= 0)
      continue;
    cjkDrawTextSubpixel(band, x + SCROLL_INSET, 0, font, m.color, m.text, subpx);
  }

  const int16_t x0 = (SCREEN_W - bw) / 2;
  const int16_t y0 = m.yTop;

  uint16_t *fb = _surf.fb;
  if (fb)
  {
    // 先同步进 canvas，整屏 flush 时内容才是对的
    for (int16_t row = 0; row < bh; row++)
      memcpy(fb + (size_t)(y0 + row) * SCREEN_W + x0, _bandBuf + (size_t)row * bw,
             (size_t)bw * 2);
  }
  if (toPanel || !fb)
    _panel->draw16bitRGBBitmap(x0, y0, _bandBuf, bw, bh);
}

void MediaFace::repaintMarquee()
{
  for (int i = 0; i < SLOT_COUNT; i++)
    if (_marquee[i].active)
      paintMarquee(i, true);
}

void MediaFace::clearMarquee()
{
  for (int i = 0; i < SLOT_COUNT; i++)
    _marquee[i].active = false;
}

void MediaFace::drawNowPlaying(const MediaInfo &info)
{
  // 歌名：一行放不下就折两行；两行仍放不下则整条单行滚动
  char lines[3][160];
  const int lineCount = cjkWrapText(FONT_TITLE, info.title, TITLE_MAX_W, 3,
                                    &lines[0][0], sizeof(lines[0]));

  if (lineCount >= 3)
  {
    drawScrollingText(SLOT_TITLE, FONT_TITLE, TITLE_Y1 + 12, COL_TEXT, info.title);
  }
  else if (lineCount == 2)
  {
    drawCentered(_surf, FONT_TITLE, TITLE_Y1, COL_TEXT, lines[0]);
    drawCentered(_surf, FONT_TITLE, TITLE_Y2, COL_TEXT, lines[1]);
  }
  else
  {
    drawCentered(_surf, FONT_TITLE, TITLE_Y1 + 12, COL_TEXT, lines[0]);
  }

  if (info.artist[0])
    drawScrollingText(SLOT_ARTIST, FONT_BODY, ARTIST_Y, COL_TEXT, info.artist);
  else
    _marquee[SLOT_ARTIST].active = false;

  if (info.album[0])
    drawScrollingText(SLOT_ALBUM, FONT_BODY, ALBUM_Y, COL_DIM, info.album);
  else
    _marquee[SLOT_ALBUM].active = false;

  // 时间：底部居中「已播/总时长」
  char elapsed[16], total[16];
  formatTime(AmsClient::elapsedNow(info), elapsed, sizeof(elapsed));
  formatTime(info.duration > 0 ? info.duration : 0, total, sizeof(total));
  snprintf(_timeText, sizeof(_timeText), "%s/%s", elapsed, total);
  drawCentered(_surf, FONT_BODY, TIME_Y, COL_DIM, _timeText);
}

void MediaFace::drawIdleScreen(AmsLinkState link)
{
  if (link == AMS_LINK_ADVERTISING)
  {
    drawNoteIcon(CX, 176, 9, COL_RING);
    drawCentered(_surf, FONT_TITLE, 214, COL_TEXT, "Orbis");
    drawCentered(_surf, FONT_BODY, 264, COL_DIM, "等待 iPhone 配对");
    drawCentered(_surf, FONT_BODY, 296, COL_DIMMER, "设置 → 蓝牙 → Orbis");
    return;
  }

  if (link == AMS_LINK_PAIRING)
  {
    drawCentered(_surf, FONT_BODY, 150, COL_DIM, "配对码");

    char code[16];
    if (_ams->hasPasskey())
    {
      const uint32_t pk = _ams->passkey();
      snprintf(code, sizeof(code), "%03lu %03lu",
               (unsigned long)(pk / 1000), (unsigned long)(pk % 1000));
      // 用 52px 数字字体横排，比内置 5x7 字体放大 6 倍干净得多
      drawCentered(_surf, FONT_NUM, 168, COL_TEXT, code);
      drawCentered(_surf, FONT_BODY, 264, COL_DIM, "在 iPhone 上输入此配对码");
    }
    else
    {
      drawCentered(_surf, FONT_BODY, 200, COL_DIM, "请在 iPhone 上确认配对");
    }
    return;
  }

  if (link == AMS_LINK_CONNECTED)
  {
    drawCentered(_surf, FONT_TITLE, 190, COL_TEXT, "已连接 iPhone");
    drawCentered(_surf, FONT_BODY, 250, COL_DIM, "正在获取播放信息…");
  }
  else
  {
    drawNoteIcon(CX, 186, 9, COL_RING);
    drawCentered(_surf, FONT_BODY, 244, COL_DIM, "iPhone 当前没有播放内容");
  }

  char hint[64];
  _ams->statusText(hint, sizeof(hint));
  if (hint[0])
    drawCentered(_surf, FONT_BODY, 300, COL_WARN, hint);
}

void MediaFace::drawControls(bool playing, uint8_t pressedCmd)
{
  const bool active = (_ams->linkState() == AMS_LINK_READY);

  // 上一首
  {
    const bool pressed = (pressedCmd == AMS_CMD_PREV);
    const uint16_t fg = pressed ? COL_BG : (active ? COL_TEXT : COL_DIMMER);
    if (pressed)
      fillCircleAA(_surf, BTN_PREV_X, CTRL_Y, BTN_SIDE_R, COL_ACCENT);
    else
      strokeCircleAA(_surf, BTN_PREV_X, CTRL_Y, BTN_SIDE_R, 1.0f,
                     active ? COL_RING : COL_DIMMER);
    _gfx->fillRect(BTN_PREV_X - 11, CTRL_Y - 10, 3, 21, fg);
    fillTriangleAA(_surf, BTN_PREV_X + 11, CTRL_Y - 10, BTN_PREV_X + 11, CTRL_Y + 11,
                   BTN_PREV_X - 4, CTRL_Y, fg);
  }

  // 播放 / 暂停
  {
    const bool pressed = (pressedCmd == AMS_CMD_TOGGLE);
    const uint16_t fill = active ? COL_ACCENT : COL_DIMMER;
    fillCircleAA(_surf, BTN_PLAY_X, CTRL_Y, BTN_PLAY_R, fill);
    if (pressed)
      strokeCircleAA(_surf, BTN_PLAY_X, CTRL_Y, BTN_PLAY_R + 4, 1.0f, COL_TEXT);

    const uint16_t fg = COL_BG;   // 青色圆底上图标用底色反白
    if (playing)
    {
      _gfx->fillRect(BTN_PLAY_X - 9, CTRL_Y - 12, 7, 24, fg);
      _gfx->fillRect(BTN_PLAY_X + 2, CTRL_Y - 12, 7, 24, fg);
    }
    else
    {
      fillTriangleAA(_surf, BTN_PLAY_X - 7, CTRL_Y - 13, BTN_PLAY_X - 7, CTRL_Y + 13,
                     BTN_PLAY_X + 14, CTRL_Y, fg);
    }
  }

  // 下一首
  {
    const bool pressed = (pressedCmd == AMS_CMD_NEXT);
    const uint16_t fg = pressed ? COL_BG : (active ? COL_TEXT : COL_DIMMER);
    if (pressed)
      fillCircleAA(_surf, BTN_NEXT_X, CTRL_Y, BTN_SIDE_R, COL_ACCENT);
    else
      strokeCircleAA(_surf, BTN_NEXT_X, CTRL_Y, BTN_SIDE_R, 1.0f,
                     active ? COL_RING : COL_DIMMER);
    _gfx->fillRect(BTN_NEXT_X + 8, CTRL_Y - 10, 3, 21, fg);
    fillTriangleAA(_surf, BTN_NEXT_X - 11, CTRL_Y - 10, BTN_NEXT_X - 11, CTRL_Y + 11,
                   BTN_NEXT_X + 4, CTRL_Y, fg);
  }
}

// ---------------- 整屏重绘（只在实际内容变化时走这里）----------------

static inline uint32_t hashU32(uint32_t h, uint32_t v)
{
  h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
  return h;
}

static inline uint32_t hashStr(uint32_t h, const char *s)
{
  while (*s)
    h = hashU32(h, (uint8_t)*s++);
  return h;
}

void MediaFace::render(const MediaInfo &info, AmsLinkState link)
{
  _gfx->fillScreen(COL_BG);
  clearMarquee();

  float deg = 0;
  if (link == AMS_LINK_READY && info.hasTrack && info.duration > 0)
    deg = AmsClient::elapsedNow(info) / info.duration * 360.0f;
  if (deg < 0.0f)
    deg = 0.0f;
  if (deg > 360.0f)
    deg = 360.0f;
  paintRingRect(_surf, 0, 0, SCREEN_W - 1, SCREEN_H - 1, deg, false);
  _ringDeg = deg;

  drawHeader(link, info);

  _timeText[0] = 0;
  if (link == AMS_LINK_READY && info.hasTrack)
    drawNowPlaying(info);
  else
    drawIdleScreen(link);

  if (link == AMS_LINK_READY)
    drawControls(info.playState == AMS_STATE_PLAYING, _pressedCmd);

  if (_canvas)
    _canvas->flush();
}

// ---------------- 局部刷新 ----------------

// 把 framebuffer 的 [y, y+h) 整行推给面板。走的是和 canvas->flush() 完全相同的
// 调用形式（起点 0、宽 SCREEN_W、源指针直接指向 fb），所以面板像素与 framebuffer
// 严格一一对应；宽度恒为 466（偶数）也避开了零宽/奇数宽度的窗口问题。
void MediaFace::flushRows(int16_t y, int16_t h)
{
  if (!_surf.fb || !_panel)
    return;

  if (y < 0)
  {
    h += y;
    y = 0;
  }
  if (y + h > SCREEN_H)
    h = SCREEN_H - y;
  if (h <= 0)
    return;

  _panel->draw16bitRGBBitmap(0, y, _surf.fb + (size_t)y * SCREEN_W, SCREEN_W, h);
}

// 进度环：只补「旧弧尖 → 新弧尖」那一小块，弧尖本身按 1px 覆盖率过渡
void MediaFace::updateRing(const MediaInfo &info, AmsLinkState link)
{
  float deg = 0;
  if (link == AMS_LINK_READY && info.hasTrack && info.duration > 0)
    deg = AmsClient::elapsedNow(info) / info.duration * 360.0f;
  if (deg < 0.0f)
    deg = 0.0f;
  if (deg > 360.0f)
    deg = 360.0f;

  const float delta = fabsf(deg - _ringDeg);
  if (delta < 0.005f)
    return;

  if (delta > RING_REDRAW_DEG)
  {
    _forceDraw = true;   // 拖动进度条这类大跳变，整屏重绘反而更省
    return;
  }

  const float a0 = fmaxf(0.0f, fminf(deg, _ringDeg) - RING_TIP_MARGIN_DEG);
  const float a1 = fminf(360.0f, fmaxf(deg, _ringDeg) + RING_TIP_MARGIN_DEG);

  int16_t bx0, by0, bx1, by1;
  wedgeBBox(a0, a1, bx0, by0, bx1, by1);
  paintRingRect(_surf, bx0, by0, bx1, by1, deg, true);
  flushRows(by0, (int16_t)(by1 - by0 + 1));
  _ringDeg = deg;
}

// 时间文字每秒变一次：只重画它自己那一小条（宽度会变，按新旧较宽的一侧清底）
void MediaFace::updateTimeText(const MediaInfo &info, AmsLinkState link)
{
  if (link != AMS_LINK_READY || !info.hasTrack)
    return;

  char elapsed[16], total[16], text[40];
  formatTime(AmsClient::elapsedNow(info), elapsed, sizeof(elapsed));
  formatTime(info.duration > 0 ? info.duration : 0, total, sizeof(total));
  snprintf(text, sizeof(text), "%s/%s", elapsed, total);
  if (strcmp(text, _timeText) == 0)
    return;

  const int oldW = _timeText[0] ? cjkTextWidth(FONT_BODY, _timeText) : 0;
  const int newW = cjkTextWidth(FONT_BODY, text);
  // 时间行两侧到这个高度已经进到进度环里了，清底宽度限制在环内（左右各留余量）
  const int16_t maxW = 176;
  int16_t w = (int16_t)((oldW > newW ? oldW : newW) + 8);
  if (w > maxW)
    w = maxW;
  const int16_t x0 = (SCREEN_W - w) / 2;
  const int16_t h = FONT_BODY.lineHeight;

  fillRectFb(_surf, x0, TIME_Y, w, h, COL_BG);
  drawCentered(_surf, FONT_BODY, TIME_Y, COL_DIM, text);
  flushRows(TIME_Y, h);

  strncpy(_timeText, text, sizeof(_timeText) - 1);
  _timeText[sizeof(_timeText) - 1] = 0;
}

void MediaFace::tick()
{
  if (!_gfx || !_ams)
    return;

  const uint32_t now = millis();
  MediaInfo info;
  _ams->snapshot(info);
  const AmsLinkState link = _ams->linkState();

  if (_pressedCmd != 0xFF && now > _pressedUntil)
  {
    _pressedCmd = 0xFF;
    _forceDraw = true;
  }

  // 结构指纹：只有真正影响布局/文案的变化才整屏重绘。
  // 播放位置（elapsed）不参与，它走增量刷新，否则每秒都要搬一次整屏。
  uint32_t h = 2166136261u;
  h = hashU32(h, (uint32_t)link);
  h = hashU32(h, info.hasTrack ? 1u : 0u);
  h = hashU32(h, info.playState);
  h = hashU32(h, info.duration > 0 ? (uint32_t)(info.duration + 0.5f) : 0u);
  h = hashStr(h, info.title);
  h = hashStr(h, info.artist);
  h = hashStr(h, info.album);
  h = hashStr(h, info.app);
  h = hashU32(h, _ams->hasPasskey() ? _ams->passkey() + 1u : 0u);
  h = hashU32(h, _pressedCmd);
  if (link != AMS_LINK_READY)
  {
    char status[64];
    _ams->statusText(status, sizeof(status));
    h = hashStr(h, status);
  }

  const bool needFull = _forceDraw || h != _structHash;

  if (!_surf.fb || !_bandBuf)
  {
    // 没有 framebuffer（或滚动带缓冲）：做不了局部刷新，退回按固定周期整屏重绘
    const uint32_t period = (info.playState == AMS_STATE_PLAYING) ? 250u : 1000u;
    if (needFull || now - _lastDrawMs >= period)
    {
      _forceDraw = false;
      _structHash = h;
      _lastDrawMs = now;
      render(info, link);
    }
    return;
  }

  if (needFull)
  {
    _forceDraw = false;
    _structHash = h;
    _lastDrawMs = now;
    _frameMs = now;
    render(info, link);
    return;
  }

  if (now - _frameMs < FRAME_MS)
    return;
  _frameMs = now;

  updateRing(info, link);
  updateTimeText(info, link);
  repaintMarquee();
}

bool MediaFace::onTouch(int16_t x, int16_t y)
{
  if (!_ams)
    return false;

  const AmsLinkState link = _ams->linkState();
  if (link != AMS_LINK_READY)
  {
    Serial.printf("[UI] touch ignored: link=%d\n", (int)link);
    return false;
  }

  const int16_t dxp = x - BTN_PLAY_X, dyp = y - CTRL_Y;
  if ((int32_t)dxp * dxp + (int32_t)dyp * dyp <= BTN_PLAY_R * BTN_PLAY_R)
  {
    const bool ok = _ams->sendCommand(AMS_CMD_TOGGLE);
    Serial.printf("[UI] toggle -> %d\n", ok);
    _pressedCmd = AMS_CMD_TOGGLE;
    _pressedUntil = millis() + 140;
    _forceDraw = true;
    return true;
  }

  const int16_t dxl = x - BTN_PREV_X, dyl = y - CTRL_Y;
  if ((int32_t)dxl * dxl + (int32_t)dyl * dyl <= BTN_SIDE_R * BTN_SIDE_R)
  {
    const bool ok = _ams->sendCommand(AMS_CMD_PREV);
    Serial.printf("[UI] prev -> %d\n", ok);
    _pressedCmd = AMS_CMD_PREV;
    _pressedUntil = millis() + 140;
    _forceDraw = true;
    return true;
  }

  const int16_t dxr = x - BTN_NEXT_X, dyr = y - CTRL_Y;
  if ((int32_t)dxr * dxr + (int32_t)dyr * dyr <= BTN_SIDE_R * BTN_SIDE_R)
  {
    const bool ok = _ams->sendCommand(AMS_CMD_NEXT);
    Serial.printf("[UI] next -> %d\n", ok);
    _pressedCmd = AMS_CMD_NEXT;
    _pressedUntil = millis() + 140;
    _forceDraw = true;
    return true;
  }

  return false;
}
