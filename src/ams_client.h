#pragma once

#include <Arduino.h>

// ============================================================
// Apple Media Service (AMS) 客户端
//
// 流程（和 Moddable / MusicStation 一致）：
//   1. ESP32 作 BLE 外设，广播里带 AMS 128-bit Service Solicitation，
//      iPhone 才会把它当成媒体遥控器并主动连接、配对（配对由 iPhone 发起）。
//   2. 加密完成后，ESP32 以 GATT Client 身份在「同一条物理链路」上
//      发现 iPhone 的 AMS 服务（esp_ble_gattc_open 对已有 ACL 只建虚拟连接）。
//   3. 订阅 Entity Update 通知并登记关注的实体/属性，之后由 iPhone 推送变化。
//
// 通知格式：[EntityID][AttributeID][Flags][UTF-8 Value...]
// ============================================================

enum AmsLinkState : uint8_t
{
  AMS_LINK_ADVERTISING = 0,   // 广播中，等 iPhone 连接
  AMS_LINK_PAIRING,           // 已连接，正在配对/加密
  AMS_LINK_CONNECTED,         // 已加密，正在找 AMS 服务
  AMS_LINK_READY,             // 已订阅，能收到媒体信息
};

// AMS 实体 ID
enum : uint8_t
{
  AMS_ENTITY_PLAYER = 0,
  AMS_ENTITY_QUEUE = 1,
  AMS_ENTITY_TRACK = 2,
};

// Player 属性 ID
enum : uint8_t
{
  AMS_PLAYER_NAME = 0,
  AMS_PLAYER_PLAYBACK_INFO = 1,
  AMS_PLAYER_VOLUME = 2,
};

// Track 属性 ID
enum : uint8_t
{
  AMS_TRACK_ARTIST = 0,
  AMS_TRACK_ALBUM = 1,
  AMS_TRACK_TITLE = 2,
  AMS_TRACK_DURATION = 3,
};

// RemoteCommandID
enum : uint8_t
{
  AMS_CMD_PLAY = 0,
  AMS_CMD_PAUSE = 1,
  AMS_CMD_TOGGLE = 2,
  AMS_CMD_NEXT = 3,
  AMS_CMD_PREV = 4,
};

// PlaybackState
enum : uint8_t
{
  AMS_STATE_PAUSED = 0,
  AMS_STATE_PLAYING = 1,
  AMS_STATE_REWINDING = 2,
  AMS_STATE_FAST_FORWARD = 3,
};

struct MediaInfo
{
  char title[128];
  char artist[96];
  char album[96];
  char app[48];
  float duration;       // 秒，<=0 表示未知
  float elapsed;        // 收到 PlaybackInfo 那一刻的播放位置
  float rate;           // 播放速率（暂停时为 0）
  uint8_t playState;    // AMS_STATE_*
  uint32_t elapsedAtMs; // 收到 PlaybackInfo 的本地时刻
  int8_t volume;        // 0-100，-1 表示未知
  bool hasTrack;
  uint32_t generation;  // 每次内容变化 +1，UI 用它判断要不要重绘
};

class AmsClient
{
public:
  void begin();
  void loop();

  AmsLinkState linkState() const;
  bool hasPasskey() const;
  uint32_t passkey() const;
  void statusText(char *out, size_t cap) const;   // 出错原因等附加说明，可为空串

  void snapshot(MediaInfo &out) const;
  uint32_t generation() const;

  // 依据本地时钟推算当前播放位置（AMS 不需要每秒推送）
  static float elapsedNow(const MediaInfo &info);

  bool sendCommand(uint8_t cmd);
};
