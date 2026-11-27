#pragma once

#include <Arduino_GFX_Library.h>

#include "ams_client.h"
#include "cjk_font.h"

// ============================================================
// 媒体播放界面（466x466 圆屏）
//
//   tick()          按需重绘：内容变化、播放中每 250ms（走进度环）
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
  void render();
  void drawHeader(AmsLinkState link, const MediaInfo &info);
  void drawProgressRing(float ratio);
  void drawNowPlaying(const MediaInfo &info);
  void drawIdleScreen(AmsLinkState link);
  void drawControls(bool playing, uint8_t pressedCmd);
  void drawNoteIcon(int16_t cx, int16_t cy, int16_t scale, uint16_t color);

  Arduino_GFX *_panel = nullptr;
  Arduino_GFX *_gfx = nullptr;
  Arduino_Canvas *_canvas = nullptr;
  CjkSurface _surf = {nullptr, 0, 0, nullptr};
  AmsClient *_ams = nullptr;

  uint32_t _lastGen = 0;
  uint32_t _lastLink = 0xFF;
  uint32_t _lastDrawMs = 0;
  uint32_t _lastPasskey = 0;
  bool _lastHasPasskey = false;
  bool _forceDraw = true;

  uint8_t _pressedCmd = 0xFF;
  uint32_t _pressedUntil = 0;
};
