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
static const int16_t TIME_Y = 300;
static const int16_t TEXT_MAX_W = 330;

static const int16_t CTRL_Y = 398;
static const int16_t BTN_PREV_X = 132;
static const int16_t BTN_NEXT_X = 334;
static const int16_t BTN_PLAY_X = 233;
static const int16_t BTN_SIDE_R = 30;
static const int16_t BTN_PLAY_R = 42;

// ---------------- 配色 ----------------
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b) >> 3)))

static const uint16_t COL_BG = RGB565(0, 0, 0);
static const uint16_t COL_RING = RGB565(38, 46, 60);
static const uint16_t COL_ACCENT = RGB565(0, 214, 255);
static const uint16_t COL_TEXT = RGB565(232, 238, 248);
static const uint16_t COL_DIM = RGB565(138, 150, 170);
static const uint16_t COL_DIMMER = RGB565(74, 84, 100);
static const uint16_t COL_WARN = RGB565(255, 176, 32);

// ---------------- 基础绘制 ----------------

static void polarToXY(float r, float deg, int16_t &x, int16_t &y)
{
  const float rad = deg * DEG_TO_RAD;
  x = CX + (int16_t)lroundf(r * sinf(rad));
  y = CY - (int16_t)lroundf(r * cosf(rad));
}

// 圆环扇区：自绘三角形，绕开 fillArc 在 0/180 度的除零边界
static void drawRingSegment(Arduino_GFX *g, int16_t rIn, int16_t rOut,
                            float a0, float a1, uint16_t color)
{
  if (a1 - a0 < 0.2f)
    return;

  const float step = 1.0f;
  for (float a = a0; a < a1; a += step)
  {
    const float b = (a + step < a1) ? (a + step) : a1;
    int16_t o1x, o1y, o2x, o2y, i1x, i1y, i2x, i2y;
    polarToXY(rOut, a, o1x, o1y);
    polarToXY(rOut, b, o2x, o2y);
    polarToXY(rIn, a, i1x, i1y);
    polarToXY(rIn, b, i2x, i2y);
    g->fillTriangle(o1x, o1y, o2x, o2y, i1x, i1y, color);
    g->fillTriangle(o2x, o2y, i2x, i2y, i1x, i1y, color);
  }
}

static int drawCentered(Arduino_GFX *g, const CjkFont &font, int yTop,
                        uint16_t color, const char *text)
{
  const int w = cjkTextWidth(font, text);
  return cjkDrawText(g, (SCREEN_W - w) / 2, yTop, font, color, text);
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
  _forceDraw = true;
}

void MediaFace::drawNoteIcon(int16_t cx, int16_t cy, int16_t scale, uint16_t color)
{
  // 简化的八分音符：符头 + 符干 + 旗
  _gfx->fillCircle(cx - scale, cy + scale, scale, color);
  _gfx->fillRect(cx - scale / 2, cy - scale * 2, scale / 2 + 1, scale * 3, color);
  _gfx->fillTriangle(cx - scale / 2, cy - scale * 2,
                     cx - scale / 2, cy - scale,
                     cx + scale, cy - scale * 2 + scale / 2, color);
}

void MediaFace::drawProgressRing(float ratio)
{
  // 底环
  drawRingSegment(_gfx, RING_R_IN, RING_R_OUT, 0, 360, COL_RING);
  if (ratio <= 0)
    return;
  if (ratio > 1)
    ratio = 1;
  drawRingSegment(_gfx, RING_R_IN, RING_R_OUT, 0, ratio * 360.0f, COL_ACCENT);
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
  cjkDrawText(_gfx, x, HEADER_Y, FONT_BODY, COL_DIM, text);
  _gfx->fillCircle(x - 13, HEADER_Y + FONT_BODY.ascent / 2, 4, dot);
}

void MediaFace::drawNowPlaying(const MediaInfo &info)
{
  // 歌名：最多两行
  char lines[2][160];
  const int lineCount = cjkWrapText(FONT_TITLE, info.title, TITLE_MAX_W, 2,
                                    &lines[0][0], sizeof(lines[0]));

  if (lineCount >= 2)
  {
    drawCentered(_gfx, FONT_TITLE, TITLE_Y1, COL_TEXT, lines[0]);
    drawCentered(_gfx, FONT_TITLE, TITLE_Y2, COL_TEXT, lines[1]);
  }
  else
  {
    drawCentered(_gfx, FONT_TITLE, TITLE_Y1 + 12, COL_TEXT, lines[0]);
  }

  if (info.artist[0])
  {
    const int w = cjkTextWidth(FONT_BODY, info.artist);
    if (w > TEXT_MAX_W)
      cjkDrawTextClipped(_gfx, (SCREEN_W - TEXT_MAX_W) / 2, ARTIST_Y, FONT_BODY,
                         COL_TEXT, info.artist, TEXT_MAX_W);
    else
      cjkDrawText(_gfx, (SCREEN_W - w) / 2, ARTIST_Y, FONT_BODY, COL_TEXT, info.artist);
  }

  if (info.album[0])
  {
    const int w = cjkTextWidth(FONT_BODY, info.album);
    if (w > TEXT_MAX_W)
      cjkDrawTextClipped(_gfx, (SCREEN_W - TEXT_MAX_W) / 2, ALBUM_Y, FONT_BODY,
                         COL_DIM, info.album, TEXT_MAX_W);
    else
      cjkDrawText(_gfx, (SCREEN_W - w) / 2, ALBUM_Y, FONT_BODY, COL_DIM, info.album);
  }

  // 时间
  char left[16], right[16];
  formatTime(AmsClient::elapsedNow(info), left, sizeof(left));
  formatTime(info.duration > 0 ? info.duration : 0, right, sizeof(right));
  cjkDrawText(_gfx, 104, TIME_Y, FONT_BODY, COL_DIM, left);
  cjkDrawText(_gfx, SCREEN_W - 104 - cjkTextWidth(FONT_BODY, right), TIME_Y,
              FONT_BODY, COL_DIM, right);
}

void MediaFace::drawIdleScreen(AmsLinkState link)
{
  if (link == AMS_LINK_ADVERTISING)
  {
    drawNoteIcon(CX, 176, 9, COL_RING);
    drawCentered(_gfx, FONT_TITLE, 214, COL_TEXT, "Orbis");
    drawCentered(_gfx, FONT_BODY, 264, COL_DIM, "等待 iPhone 配对");
    drawCentered(_gfx, FONT_BODY, 296, COL_DIMMER, "设置 → 蓝牙 → Orbis");
    return;
  }

  if (link == AMS_LINK_PAIRING)
  {
    drawCentered(_gfx, FONT_BODY, 150, COL_DIM, "配对码");

    char code[16];
    if (_ams->hasPasskey())
    {
      const uint32_t pk = _ams->passkey();
      snprintf(code, sizeof(code), "%03lu %03lu",
               (unsigned long)(pk / 1000), (unsigned long)(pk % 1000));
      _gfx->setTextSize(6);
      _gfx->setTextColor(COL_TEXT);
      int16_t x1, y1;
      uint16_t w, h;
      _gfx->getTextBounds(code, 0, 0, &x1, &y1, &w, &h);
      _gfx->setCursor((SCREEN_W - (int16_t)w) / 2 - x1, 196 - y1);
      _gfx->print(code);
      _gfx->setTextSize(1);
      drawCentered(_gfx, FONT_BODY, 264, COL_DIM, "在 iPhone 上输入此配对码");
    }
    else
    {
      drawCentered(_gfx, FONT_BODY, 200, COL_DIM, "请在 iPhone 上确认配对");
    }
    return;
  }

  if (link == AMS_LINK_CONNECTED)
  {
    drawCentered(_gfx, FONT_TITLE, 190, COL_TEXT, "已连接 iPhone");
    drawCentered(_gfx, FONT_BODY, 250, COL_DIM, "正在获取播放信息…");
  }
  else
  {
    drawNoteIcon(CX, 186, 9, COL_RING);
    drawCentered(_gfx, FONT_BODY, 244, COL_DIM, "iPhone 当前没有播放内容");
  }

  char hint[64];
  _ams->statusText(hint, sizeof(hint));
  if (hint[0])
    drawCentered(_gfx, FONT_BODY, 300, COL_WARN, hint);
}

void MediaFace::drawControls(bool playing, uint8_t pressedCmd)
{
  const bool active = (_ams->linkState() == AMS_LINK_READY);

  // 上一首
  {
    const bool pressed = (pressedCmd == AMS_CMD_PREV);
    const uint16_t fg = pressed ? COL_BG : (active ? COL_TEXT : COL_DIMMER);
    if (pressed)
      _gfx->fillCircle(BTN_PREV_X, CTRL_Y, BTN_SIDE_R, COL_ACCENT);
    else
      _gfx->drawCircle(BTN_PREV_X, CTRL_Y, BTN_SIDE_R, active ? COL_RING : COL_DIMMER);
    _gfx->fillRect(BTN_PREV_X - 9, CTRL_Y - 8, 3, 17, fg);
    _gfx->fillTriangle(BTN_PREV_X + 9, CTRL_Y - 8, BTN_PREV_X + 9, CTRL_Y + 9,
                       BTN_PREV_X - 3, CTRL_Y, fg);
  }

  // 播放 / 暂停
  {
    const bool pressed = (pressedCmd == AMS_CMD_TOGGLE);
    const uint16_t fill = active ? COL_ACCENT : COL_DIMMER;
    _gfx->fillCircle(BTN_PLAY_X, CTRL_Y, BTN_PLAY_R, fill);
    if (pressed)
      _gfx->drawCircle(BTN_PLAY_X, CTRL_Y, BTN_PLAY_R + 4, COL_TEXT);

    const uint16_t fg = COL_BG;   // 青色圆底上图标用底色反白
    if (playing)
    {
      _gfx->fillRect(BTN_PLAY_X - 9, CTRL_Y - 12, 7, 24, fg);
      _gfx->fillRect(BTN_PLAY_X + 2, CTRL_Y - 12, 7, 24, fg);
    }
    else
    {
      _gfx->fillTriangle(BTN_PLAY_X - 7, CTRL_Y - 13, BTN_PLAY_X - 7, CTRL_Y + 13,
                         BTN_PLAY_X + 14, CTRL_Y, fg);
    }
  }

  // 下一首
  {
    const bool pressed = (pressedCmd == AMS_CMD_NEXT);
    const uint16_t fg = pressed ? COL_BG : (active ? COL_TEXT : COL_DIMMER);
    if (pressed)
      _gfx->fillCircle(BTN_NEXT_X, CTRL_Y, BTN_SIDE_R, COL_ACCENT);
    else
      _gfx->drawCircle(BTN_NEXT_X, CTRL_Y, BTN_SIDE_R, active ? COL_RING : COL_DIMMER);
    _gfx->fillRect(BTN_NEXT_X + 6, CTRL_Y - 8, 3, 17, fg);
    _gfx->fillTriangle(BTN_NEXT_X - 9, CTRL_Y - 8, BTN_NEXT_X - 9, CTRL_Y + 9,
                       BTN_NEXT_X + 3, CTRL_Y, fg);
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
