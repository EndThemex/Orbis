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
static const int16_t TEXT_MAX_W = 330;

static const int16_t CTRL_Y = 352;
static const int16_t TIME_Y = 408;
static const int16_t BTN_PREV_X = 132;
static const int16_t BTN_NEXT_X = 334;
static const int16_t BTN_PLAY_X = 233;
static const int16_t BTN_SIDE_R = 37;
static const int16_t BTN_PLAY_R = 42;

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
static void ringSpan(const CjkSurface &s, int16_t y, float dy, int16_t xFrom, int16_t xTo,
                     float a0, float a1, bool full, uint16_t color)
{
  for (int16_t x = xFrom; x <= xTo; x++)
  {
    const float dx = x + 0.5f - CX;
    const float dist = sqrtf(dx * dx + dy * dy);

    if (!full)
    {
      // 角度自 12 点起顺时针，用来切出进度弧的两个端面
      float ang = atan2f(dx, -dy) * RAD_TO_DEG;
      if (ang < 0.0f)
        ang += 360.0f;
      if (ang < a0 || ang > a1)
        continue;
    }

    cjkBlendPixel(s, x, y, color, coverage(fminf(RING_R_OUT - dist, dist - RING_R_IN)));
  }
}

static void drawRingSegmentAA(const CjkSurface &s, float a0, float a1, uint16_t color)
{
  if (a1 - a0 < 0.05f)
    return;

  const bool full = (a1 - a0) >= 359.9f;
  const float ro2 = (RING_R_OUT + 0.5f) * (RING_R_OUT + 0.5f);
  const float ri2 = (RING_R_IN - 0.5f) * (RING_R_IN - 0.5f);
  const int16_t yr = (int16_t)ceilf(RING_R_OUT + 0.5f);

  for (int16_t y = CY - yr; y <= CY + yr; y++)
  {
    const float dy = y + 0.5f - CY;
    const float dy2 = dy * dy;
    if (dy2 > ro2)
      continue;

    const int16_t xo = (int16_t)sqrtf(ro2 - dy2);
    const int16_t xi = (dy2 < ri2) ? (int16_t)sqrtf(ri2 - dy2) : 0;
    if (xi > 0)
    {
      ringSpan(s, y, dy, CX - xo, CX - xi, a0, a1, full, color);
      ringSpan(s, y, dy, CX + xi, CX + xo, a0, a1, full, color);
    }
    else
    {
      // 内圆还没张开（环的上下两端），整行只画一次，避免中心像素被混合两遍
      ringSpan(s, y, dy, CX - xo, CX + xo, a0, a1, full, color);
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

void MediaFace::drawProgressRing(float ratio)
{
  // 底环
  drawRingSegmentAA(_surf, 0, 360, COL_RING);
  if (ratio <= 0)
    return;
  if (ratio > 1)
    ratio = 1;
  drawRingSegmentAA(_surf, 0, ratio * 360.0f, COL_ACCENT);
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

void MediaFace::drawNowPlaying(const MediaInfo &info)
{
  // 歌名：最多两行
  char lines[2][160];
  const int lineCount = cjkWrapText(FONT_TITLE, info.title, TITLE_MAX_W, 2,
                                    &lines[0][0], sizeof(lines[0]));

  if (lineCount >= 2)
  {
    drawCentered(_surf, FONT_TITLE, TITLE_Y1, COL_TEXT, lines[0]);
    drawCentered(_surf, FONT_TITLE, TITLE_Y2, COL_TEXT, lines[1]);
  }
  else
  {
    drawCentered(_surf, FONT_TITLE, TITLE_Y1 + 12, COL_TEXT, lines[0]);
  }

  if (info.artist[0])
  {
    const int w = cjkTextWidth(FONT_BODY, info.artist);
    if (w > TEXT_MAX_W)
      cjkDrawTextClipped(_surf, (SCREEN_W - TEXT_MAX_W) / 2, ARTIST_Y, FONT_BODY,
                         COL_TEXT, info.artist, TEXT_MAX_W);
    else
      cjkDrawText(_surf, (SCREEN_W - w) / 2, ARTIST_Y, FONT_BODY, COL_TEXT, info.artist);
  }

  if (info.album[0])
  {
    const int w = cjkTextWidth(FONT_BODY, info.album);
    if (w > TEXT_MAX_W)
      cjkDrawTextClipped(_surf, (SCREEN_W - TEXT_MAX_W) / 2, ALBUM_Y, FONT_BODY,
                         COL_DIM, info.album, TEXT_MAX_W);
    else
      cjkDrawText(_surf, (SCREEN_W - w) / 2, ALBUM_Y, FONT_BODY, COL_DIM, info.album);
  }

  // 时间：底部居中「已播/总时长」
  char elapsed[16], total[16], timeText[40];
  formatTime(AmsClient::elapsedNow(info), elapsed, sizeof(elapsed));
  formatTime(info.duration > 0 ? info.duration : 0, total, sizeof(total));
  snprintf(timeText, sizeof(timeText), "%s/%s", elapsed, total);
  drawCentered(_surf, FONT_BODY, TIME_Y, COL_DIM, timeText);
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

void MediaFace::render()
{
  MediaInfo info;
  _ams->snapshot(info);
  const AmsLinkState link = _ams->linkState();

  _gfx->fillScreen(COL_BG);

  float ratio = 0;
  if (link == AMS_LINK_READY && info.hasTrack && info.duration > 0)
    ratio = AmsClient::elapsedNow(info) / info.duration;
  drawProgressRing(ratio);

  drawHeader(link, info);

  if (link == AMS_LINK_READY && info.hasTrack)
    drawNowPlaying(info);
  else
    drawIdleScreen(link);

  if (link == AMS_LINK_READY)
    drawControls(info.playState == AMS_STATE_PLAYING, _pressedCmd);

  if (_canvas)
    _canvas->flush();
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

  const bool playing = (info.playState == AMS_STATE_PLAYING);
  const uint32_t period = playing ? 250 : 1000;

  const bool hasPasskey = _ams->hasPasskey();
  const uint32_t passkey = _ams->passkey();

  const bool changed = _forceDraw || info.generation != _lastGen ||
                       (uint32_t)link != _lastLink ||
                       hasPasskey != _lastHasPasskey || passkey != _lastPasskey ||
                       now - _lastDrawMs >= period;
  if (!changed)
    return;

  _forceDraw = false;
  _lastGen = info.generation;
  _lastLink = link;
  _lastHasPasskey = hasPasskey;
  _lastPasskey = passkey;
  _lastDrawMs = now;
  render();
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
