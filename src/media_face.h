#pragma once

#include <Arduino_GFX_Library.h>

#include "ams_client.h"
#include "cjk_font.h"

// ============================================================
// 媒体播放界面（466x466 圆屏）
//
//   tick()          内容变化时整屏重绘，其余时间只按 30fps 增量刷新
//                   「进度环扇区 / 时间文字 / 滚动文字带」
//   onTouch(x, y)   命中「上一首 / 播放暂停 / 下一首」时下发 AMS Remote Command
//
// 布局（圆心 233,233）：
//   r=224~230  进度环（播放进度）
//   y≈78       状态行（连接状态 + 播放器名）
//   y≈126~162  歌名（最多两行，30px）
//   y≈208/240  歌手 / 专辑（22px）
//   y≈352      三个圆形按钮
//   y≈408      底部居中「已播/总时长」（22px，如 1:11/4:30）
//
// 所有绘制都落在 Arduino_Canvas 的 framebuffer 上，字形按覆盖度混合、
// 圆环/圆形/三角形按边缘覆盖率抗锯齿，圆屏上不再有硬边台阶。
// 整屏 flush 要搬 434KB，偶尔做一次就够；进度和滚动都只把脏区域推给面板，
// 才不会每帧卡住几十毫秒（那就是「一顿一顿」的来源）。
// ============================================================
class MediaFace
{
public:
  void begin(Arduino_GFX *panel, Arduino_Canvas *canvas, AmsClient *ams);

  void tick();

  // 下一帧强制整屏重绘（息屏期间不 tick，点亮后用它补一帧）
  void invalidate() { _forceDraw = true; }

  // 返回 true 表示这次触摸落在按钮上并已下发命令
  bool onTouch(int16_t x, int16_t y);

private:
  void render(const MediaInfo &info, AmsLinkState link);
  void drawHeader(AmsLinkState link, const MediaInfo &info);
  void drawNowPlaying(const MediaInfo &info);
  void drawIdleScreen(AmsLinkState link);
  void drawControls(bool playing, uint8_t pressedCmd);
  void drawNoteIcon(int16_t cx, int16_t cy, int16_t scale, uint16_t color);

  // 增量刷新
  void updateRing(const MediaInfo &info, AmsLinkState link);
  void updateTimeText(const MediaInfo &info, AmsLinkState link);
  void flushRows(int16_t y, int16_t h);

  // 滚动文字：放得下就居中静态显示，放不下就在窗口内横向滚动
  void drawScrollingText(int slot, const CjkFont &font, int16_t yTop, uint16_t color,
                         const char *text);
  void paintMarquee(int slot, bool toPanel);
  void repaintMarquee();
  void clearMarquee();

  struct Marquee
  {
    char text[160];
    uint32_t startMs;
    const CjkFont *font;
    int16_t yTop;
    uint16_t color;
    int32_t lastPos16;   // 上一次推屏的滚动位置（1/16 像素），没动就不用重推
    bool active;
  };

  Arduino_GFX *_panel = nullptr;
  Arduino_GFX *_gfx = nullptr;
  Arduino_Canvas *_canvas = nullptr;
  CjkSurface _surf = {nullptr, 0, 0, nullptr};
  AmsClient *_ams = nullptr;

  uint32_t _structHash = 0;   // 影响整屏布局的内容指纹，变了才整屏重绘
  uint32_t _lastDrawMs = 0;   // 上一次整屏时间（无 framebuffer 时退回定时重绘）
  uint32_t _frameMs = 0;      // 上一帧增量刷新的时刻
  float _ringDeg = 0;         // 进度环上已画到的角度
  char _timeText[40] = "";    // 当前显示的「已播/总时长」
  bool _forceDraw = true;

  uint8_t _pressedCmd = 0xFF;
  uint32_t _pressedUntil = 0;

  Marquee _marquee[3] = {};
  uint16_t *_bandBuf = nullptr;   // 滚动带区渲染缓冲（PSRAM）
};
