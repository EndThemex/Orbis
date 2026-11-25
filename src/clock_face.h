#pragma once

#include <Arduino_GFX_Library.h>
#include <time.h>

// ============================================================
// 圆屏表盘（整帧重绘版）
//
//   begin(target, canvas)  target = 面板驱动；canvas = PSRAM 离屏画布（可空）
//   tick(now)              每秒重画一帧：底色 → 外圈 → 秒环 → 刻度 → 数字
//                          → 日期 → 指针 → 轴心 → flush
//
// 为什么整帧重绘：擦除式增量绘制要求指针短于所有静态元素（否则会擦掉刻度/数字），
// 那样指针只能做到半径的 40%，实机上显得“缩在中间”。改成画布后指针可以压到
// 刻度和数字上（真实手表就是这么长的），代价是每秒一次 466x466x2 = 434KB 的推送。
// ============================================================
class ClockFace
{
public:
  void begin(Arduino_GFX *panel, Arduino_Canvas *canvas = nullptr);
  void setSynced(bool synced);      // 已校时=青，未校时=琥珀
  bool tick(const struct tm &now);  // 每秒调用；返回 true 表示这一帧真的画了

private:
  void drawDial(const struct tm &now);
  void drawProgress(int sec);
  void drawDate(const struct tm &now);
  void drawHands(float hourDeg, float minDeg, float secDeg);

  Arduino_GFX *_panel = nullptr;     // 真实面板（推屏用）
  Arduino_GFX *_gfx = nullptr;       // 实际绘制目标（画布或面板）
  Arduino_Canvas *_canvas = nullptr; // 非空则每帧 flush 一次
  bool _synced = true;
  int _lastSec = -1;
};
