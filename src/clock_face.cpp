#include "clock_face.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>

#include "board_pins.h"   // SCREEN_W / SCREEN_H
#include "config.h"       // CLOCK_ROTATE_DEG

// ---------------- 几何常量（466x466，圆心 233,233）----------------
static const int16_t CX = SCREEN_W / 2;   // 233
static const int16_t CY = SCREEN_H / 2;   // 233

static const int16_t RIM_R    = 231;  // 最外细圈
static const int16_t RING_OUT = 226;  // 秒进度环外径
static const int16_t RING_IN  = 219;  // 秒进度环内径
static const int16_t TICK_R   = 208;  // 刻度点半径
static const int16_t NUM_R    = 186;  // 小时数字中心半径
static const int16_t NUM_SIZE = 4;    // 数字点阵放大倍数（24x32）

static const int16_t HOUR_LEN = 130, HOUR_W = 9, HOUR_TAIL = 14;
static const int16_t MIN_LEN  = 178, MIN_W  = 6, MIN_TAIL  = 0;
static const int16_t SEC_LEN  = 200, SEC_W  = 2, SEC_TAIL  = 30;
static const int16_t HUB_R    = 11;

static const int16_t DATE_Y    = CY + 188;  // 日期放在 6 点位置（那里不画数字 6）
static const int16_t DATE_SIZE = 3;

// ---------------- 配色（RGB565）----------------
#define ORBIS_RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b) >> 3)))

static const uint16_t COL_BG       = ORBIS_RGB565(0, 0, 0);        // AMOLED 纯黑
static const uint16_t COL_RIM      = ORBIS_RGB565(48, 56, 74);
static const uint16_t COL_TRAIL    = ORBIS_RGB565(20, 24, 34);
static const uint16_t COL_TICK_MIN = ORBIS_RGB565(88, 99, 122);
static const uint16_t COL_TICK_MAJ = ORBIS_RGB565(176, 190, 214);
static const uint16_t COL_NUM      = ORBIS_RGB565(198, 210, 232);
static const uint16_t COL_HAND     = ORBIS_RGB565(255, 255, 255);
static const uint16_t COL_SEC      = ORBIS_RGB565(0, 214, 255);    // 青
static const uint16_t COL_HUB      = ORBIS_RGB565(235, 240, 255);
static const uint16_t COL_ACCENT   = ORBIS_RGB565(0, 214, 255);    // 已校时
static const uint16_t COL_WARN     = ORBIS_RGB565(255, 176, 32);   // 未校时（琥珀）
static const uint16_t COL_DATE     = ORBIS_RGB565(150, 162, 186);

// ---------------- 小工具 ----------------

// deg：以 12 点方向为 0°，顺时针为正
static void polarToXY(float r, float deg, int16_t &x, int16_t &y)
{
  float rad = deg * DEG_TO_RAD;
  x = CX + (int16_t)lroundf(r * sinf(rad));
  y = CY - (int16_t)lroundf(r * cosf(rad));
}

// 圆环扇区：自绘三角形，绕开 Arduino_GFX::fillArc 在 0/180 度的除零边界
static void drawRingSegment(Arduino_GFX *g, int16_t rIn, int16_t rOut,
                            float a0, float a1, uint16_t color)
{
  if (a1 - a0 < 0.2f)
    return;

  const float step = 1.0f;
  for (float a = a0; a < a1; a += step)
  {
    float b = (a + step < a1) ? (a + step) : a1;

    int16_t o1x, o1y, o2x, o2y, i1x, i1y, i2x, i2y;
    polarToXY(rOut, a, o1x, o1y);
    polarToXY(rOut, b, o2x, o2y);
    polarToXY(rIn, a, i1x, i1y);
    polarToXY(rIn, b, i2x, i2y);

    g->fillTriangle(o1x, o1y, o2x, o2y, i1x, i1y, color);
    g->fillTriangle(o2x, o2y, i2x, i2y, i1x, i1y, color);
  }
}

// 以 (cx,cy) 为中心输出文字（字号 / 颜色需事先设好）
static void drawTextCentered(Arduino_GFX *g, const char *s, int16_t cx, int16_t cy)
{
  int16_t x1, y1;
  uint16_t w, h;
  g->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  g->setCursor(cx - (int16_t)w / 2 - x1, cy - (int16_t)h / 2 - y1);
  g->print(s);
}

// 两个三角形 + 轴心圆拼一根指针
static void drawHand(Arduino_GFX *g, float deg, int16_t len, int16_t tail,
                     int16_t halfW, uint16_t color)
{
  float rad = deg * DEG_TO_RAD;
  float dx = sinf(rad), dy = -cosf(rad);

  int16_t tipX = CX + (int16_t)lroundf(dx * len);
  int16_t tipY = CY + (int16_t)lroundf(dy * len);
  int16_t tailX = CX - (int16_t)lroundf(dx * tail);
  int16_t tailY = CY - (int16_t)lroundf(dy * tail);

  int16_t px = (int16_t)lroundf(-dy * halfW);
  int16_t py = (int16_t)lroundf(dx * halfW);

  g->fillTriangle(tipX, tipY, CX + px, CY + py, CX - px, CY - py, color);
  if (tail > 0)
    g->fillTriangle(tailX, tailY, CX + px, CY + py, CX - px, CY - py, color);
  g->fillCircle(CX, CY, halfW, color);
}

// ---------------- 整屏旋转推屏 ----------------

// 面板的 MADCTL 只能整 90° 转，45° 这类角度只能在像素层做：
// 目标像素 (x,y) 反算源坐标（R(-θ)），最近邻取样，再一次性推屏。
// 旋转绕屏幕中心进行，表盘本身是个圆，所以内容不会被裁掉。
static uint16_t *_rotBuf = nullptr;

static bool flushRotated(Arduino_GFX *panel, const uint16_t *src, float deg)
{
  const int32_t total = (int32_t)SCREEN_W * SCREEN_H;

  if (!_rotBuf)
    _rotBuf = (uint16_t *)heap_caps_malloc(total * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!_rotBuf)
    return false;   // 分不到 PSRAM 就由调用方退回直推

  // Q16.16 定点，每像素只要两次加法（浮点版在 240MHz 上要慢一倍多）
  const float rad = -deg * DEG_TO_RAD;            // dest → src 用反角
  const int32_t c = (int32_t)lroundf(cosf(rad) * 65536.0f);
  const int32_t s = (int32_t)lroundf(sinf(rad) * 65536.0f);

  for (int16_t y = 0; y < SCREEN_H; y++)
  {
    const int32_t dy = y - CY;
    int32_t sx = (-CX) * c + dy * s;   // dx = -CX 时的初值
    int32_t sy = CX * s + dy * c;      // -dx * s，dx = -CX
    uint16_t *dst = _rotBuf + (int32_t)y * SCREEN_W;

    for (int16_t x = 0; x < SCREEN_W; x++)
    {
      const int16_t u = (int16_t)(sx >> 16);
      const int16_t v = (int16_t)(sy >> 16);
      dst[x] = ((uint16_t)u < SCREEN_W && (uint16_t)v < SCREEN_H)
                   ? src[(int32_t)v * SCREEN_W + u]
                   : 0;
      sx += c;
      sy -= s;
    }
  }

  panel->draw16bitRGBBitmap(0, 0, _rotBuf, SCREEN_W, SCREEN_H);
  return true;
}

// ---------------- ClockFace ----------------

void ClockFace::begin(Arduino_GFX *panel, Arduino_Canvas *canvas)
{
  _panel = panel;
  _canvas = canvas;
  _gfx = canvas ? (Arduino_GFX *)canvas : panel;   // 有画布就画在画布上
  _lastSec = -1;
}

void ClockFace::setSynced(bool synced)
{
  _synced = synced;   // 下一帧生效
}

void ClockFace::drawDial(const struct tm &now)
{
  _gfx->fillScreen(COL_BG);

  // 外圈 + 秒环底
  _gfx->drawCircle(CX, CY, RIM_R, COL_RIM);
  for (int16_t r = RING_IN; r <= RING_OUT; r++)
    _gfx->drawCircle(CX, CY, r, COL_TRAIL);

  // 60 个刻度点，每 5 分钟一个大的
  for (int i = 0; i < 60; i++)
  {
    int16_t x, y;
    polarToXY(TICK_R, i * 6.0f, x, y);
    bool major = (i % 5 == 0);
    _gfx->fillCircle(x, y, major ? 4 : 2, major ? COL_TICK_MAJ : COL_TICK_MIN);
  }

  // 1-12 小时数字（跳过 6，那个位置留给日期）
  _gfx->setTextSize(NUM_SIZE);
  _gfx->setTextColor(COL_NUM);
  char buf[4];
  for (int h = 1; h <= 12; h++)
  {
    if (h == 6)
      continue;
    snprintf(buf, sizeof(buf), "%d", h);
    int16_t x, y;
    polarToXY(NUM_R, h * 30.0f, x, y);
    drawTextCentered(_gfx, buf, x, y);
  }

  drawDate(now);
}

void ClockFace::drawDate(const struct tm &now)
{
  static const char *WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  char buf[16];
  snprintf(buf, sizeof(buf), "%s %02d-%02d", WDAY[now.tm_wday % 7],
           now.tm_mon + 1, now.tm_mday);

  _gfx->setTextSize(DATE_SIZE);
  _gfx->setTextColor(COL_DATE);
  drawTextCentered(_gfx, buf, CX, DATE_Y);
}

void ClockFace::drawProgress(int sec)
{
  if (sec <= 0)
    return;
  // polarToXY 的约定是 0°=12 点、顺时针为正，所以起点必须是 0 而不是 -90
  drawRingSegment(_gfx, RING_IN, RING_OUT, 0.0f, sec * 6.0f,
                  _synced ? COL_ACCENT : COL_WARN);
}

void ClockFace::drawHands(float hourDeg, float minDeg, float secDeg)
{
  drawHand(_gfx, hourDeg, HOUR_LEN, HOUR_TAIL, HOUR_W, COL_HAND);
  drawHand(_gfx, minDeg, MIN_LEN, MIN_TAIL, MIN_W, COL_HAND);
  drawHand(_gfx, secDeg, SEC_LEN, SEC_TAIL, SEC_W, COL_SEC);

  // 轴心最后画，盖住三根指针的根部
  _gfx->fillCircle(CX, CY, HUB_R, COL_HUB);
  _gfx->fillCircle(CX, CY, 4, _synced ? COL_ACCENT : COL_WARN);
}

bool ClockFace::tick(const struct tm &now)
{
  if (!_gfx)
    return false;
  if (now.tm_sec == _lastSec)
    return false;   // 同一秒内不重复渲染

  drawDial(now);
  drawProgress(now.tm_sec);

  float hourDeg = (now.tm_hour % 12) * 30.0f + now.tm_min * 0.5f;
  float minDeg = now.tm_min * 6.0f + now.tm_sec * 0.1f;
  float secDeg = now.tm_sec * 6.0f;
  drawHands(hourDeg, minDeg, secDeg);

  if (_canvas)
  {
#if CLOCK_ROTATE_DEG == 0
    _canvas->flush();   // 整帧一次推送，避免中间态闪烁
#else
    // 整屏旋转后再推（旋转失败则退回原样直推，画面不至于全黑）
    if (!flushRotated(_panel, _canvas->getFramebuffer(), (float)CLOCK_ROTATE_DEG))
      _canvas->flush();
#endif
  }

  _lastSec = now.tm_sec;
  return true;
}
