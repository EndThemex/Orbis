#pragma once

// ============================================================
// 本地配置：改这里就行，不要提交真实密码到公开仓库
// ============================================================

#if __has_include("secrets.h")
#include "secrets.h"   // 可选：把 WIFI_SSID / WIFI_PASSWORD 放这里
#endif

// ---- WiFi（连不上也能走，只是用编译时间当起点）----
#ifndef WIFI_SSID
#define WIFI_SSID "JCYJ102803"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "jcyj102803"
#endif

// ---- 时区：CST-8 = 中国标准时间 UTC+8 ----
#ifndef TZ_STRING
#define TZ_STRING "CST-8"
#endif

// ---- NTP ----
#define NTP_SERVER_1 "ntp.aliyun.com"
#define NTP_SERVER_2 "cn.pool.ntp.org"

// ---- 显示 ----
#define CLOCK_BRIGHTNESS 170   // 0-255，AMOLED 别开太满

// 整屏旋转角度（度，顺时针为正，0 = 12 点在正上方）
// 面板的 MADCTL 只支持 90° 递增，45° 这类角度在像素层做（PSRAM 里反算取样）
// 0 = 关闭旋转，走原来的直推路径（省一块 434KB 缓冲 + 约 28ms 帧耗时）
#define CLOCK_ROTATE_DEG 0

// ---- 校时周期 ----
#define WIFI_RETRY_MS (30UL * 1000UL)       // 断网重连间隔
#define NTP_RESYNC_MS (60UL * 60UL * 1000UL) // 每小时重新校时

// ---- BLE / AMS ----
// iPhone「设置 → 蓝牙」里显示的设备名
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "Orbis"
#endif

// ---- 触摸校准（默认与屏幕同向；坐标不对时改这三项）----
// 实测本板 CST9217 的触摸坐标系相对屏幕是 180° 翻转的（X/Y 都要镜像）：
//   Y：点屏幕底部会上报 y≈40，故镜像 Y
//   X：点左边「上一首」会上报 x≈334（右边），结果触发成下一首，故镜像 X
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 1
#define TOUCH_MIRROR_Y 1
