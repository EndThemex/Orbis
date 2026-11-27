#include "ams_client.h"

#include <BLE2902.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteService.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <esp_gap_ble_api.h>

#include <stdio.h>
#include <string.h>

#include "config.h"

static const char *AMS_SERVICE_UUID = "89D3502B-0F36-433A-8EF4-C502AD55F8DC";
static const char *AMS_REMOTE_COMMAND_UUID = "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2";
static const char *AMS_ENTITY_UPDATE_UUID = "2F7CABCE-808D-411F-9A0C-BB92BA96C102";

// ---------------- 共享状态（BLE 任务 <-> 主循环）----------------
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static MediaInfo s_info;
static uint32_t s_gen = 1;

static volatile bool s_linked = false;
static volatile bool s_authenticated = false;
static volatile bool s_amsReady = false;
static volatile bool s_peerValid = false;
static volatile bool s_hasPasskey = false;
static volatile uint32_t s_passkey = 0;
static volatile uint32_t s_authMs = 0;

static char s_status[64] = "";          // 仅由 BLE 回调/主循环写，UI 读取时拷贝
static BLEAddress s_peer("00:00:00:00:00:00");
static esp_ble_addr_type_t s_peerType = BLE_ADDR_TYPE_PUBLIC;

static BLEClient *s_client = nullptr;
static BLERemoteCharacteristic *s_entityUpdate = nullptr;
static BLERemoteCharacteristic *s_remoteCommand = nullptr;
static uint32_t s_lastTryMs = 0;
static uint8_t s_tryCount = 0;
static volatile bool s_clientStale = false;   // 新链路建立，旧的虚拟连接要重建

// ---------------- 小工具 ----------------

static void setStatus(const char *text)
{
  portENTER_CRITICAL(&s_mux);
  snprintf(s_status, sizeof(s_status), "%s", text);
  portEXIT_CRITICAL(&s_mux);
}

// 拷贝字符串到定长缓冲，超长时保证不切断 UTF-8；内容没变返回 false
static bool setText(char *dst, size_t cap, const char *src)
{
  size_t n = strlen(src);
  if (n >= cap)
  {
    n = cap - 1;
    while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80)
      n--;
  }
  if (strncmp(dst, src, n) == 0 && dst[n] == 0)
    return false;
  memcpy(dst, src, n);
  dst[n] = 0;
  return true;
}

static void clearMedia()
{
  portENTER_CRITICAL(&s_mux);
  s_info.title[0] = s_info.artist[0] = s_info.album[0] = s_info.app[0] = 0;
  s_info.duration = s_info.elapsed = s_info.rate = 0;
  s_info.playState = AMS_STATE_PAUSED;
  s_info.elapsedAtMs = 0;
  s_info.volume = -1;
  s_info.hasTrack = false;
  s_info.generation = ++s_gen;
  portEXIT_CRITICAL(&s_mux);
}

// ---------------- AMS 通知解析 ----------------

// PlaybackInfo 有两种历史格式：
//   新版：字符串 "state,rate,elapsed"（Apple 文档）
//   旧版：二进制 state(1) + rate(int8) + elapsed(double, LE)
struct Playback
{
  uint8_t state;
  float rate;
  float elapsed;
};

static bool parsePlayback(const uint8_t *payload, size_t len, Playback &out)
{
  if (len == 0)
    return false;

  char buf[48];
  const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = 0;

  if (strchr(buf, ',') != nullptr)
  {
    int state = 0;
    float rate = 0, elapsed = 0;
    if (sscanf(buf, "%d,%f,%f", &state, &rate, &elapsed) != 3)
      return false;
    out.state = (uint8_t)state;
    out.rate = rate;
    out.elapsed = elapsed;
    return true;
  }

  if (len >= 10)
  {
    double elapsed = 0;
    memcpy(&elapsed, payload + 2, sizeof(elapsed));
    out.state = payload[0];
    out.rate = (float)(int8_t)payload[1];
    out.elapsed = (float)elapsed;
    return true;
  }
  return false;
}

// Entity Update 通知：[entity][attr][flags][value...]
static void onEntityUpdate(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
  if (len < 3)
    return;

  const uint8_t entity = data[0];
  const uint8_t attr = data[1];
  const uint8_t *payload = data + 3;
  const size_t plen = len - 3;

  char text[160];
  const size_t n = plen < sizeof(text) - 1 ? plen : sizeof(text) - 1;
  memcpy(text, payload, n);
  text[n] = 0;

  Playback pb;
  const bool havePlayback = (entity == AMS_ENTITY_PLAYER && attr == AMS_PLAYER_PLAYBACK_INFO)
                                ? parsePlayback(payload, plen, pb)
                                : false;

  bool changed = false;
  portENTER_CRITICAL(&s_mux);
  if (entity == AMS_ENTITY_PLAYER)
  {
    if (attr == AMS_PLAYER_NAME)
    {
      changed = setText(s_info.app, sizeof(s_info.app), text);
    }
    else if (attr == AMS_PLAYER_VOLUME)
    {
      const float v = strtof(text, nullptr);
      const int8_t pct = (int8_t)(v <= 0 ? 0 : (v >= 1 ? 100 : v * 100.0f + 0.5f));
      if (pct != s_info.volume)
      {
        s_info.volume = pct;
        changed = true;
      }
    }
    else if (attr == AMS_PLAYER_PLAYBACK_INFO && havePlayback)
    {
      s_info.playState = pb.state;
      s_info.rate = pb.rate;
      s_info.elapsed = pb.elapsed;
      s_info.elapsedAtMs = millis();
      changed = true;
    }
  }
  else if (entity == AMS_ENTITY_TRACK)
  {
    if (attr == AMS_TRACK_TITLE)
    {
      changed |= setText(s_info.title, sizeof(s_info.title), text);
      s_info.hasTrack = (s_info.title[0] != 0);
    }
    else if (attr == AMS_TRACK_ARTIST)
    {
      changed |= setText(s_info.artist, sizeof(s_info.artist), text);
    }
    else if (attr == AMS_TRACK_ALBUM)
    {
      changed |= setText(s_info.album, sizeof(s_info.album), text);
    }
    else if (attr == AMS_TRACK_DURATION)
    {
      const float d = strtof(text, nullptr);
      if (d != s_info.duration)
      {
        s_info.duration = d;
        changed = true;
      }
    }
  }

  if (changed)
    s_info.generation = ++s_gen;
  portEXIT_CRITICAL(&s_mux);
}

// ---------------- BLE 回调 ----------------

class AmsServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override
  {
    s_peer = BLEAddress(param->connect.remote_bda);
    s_peerType = param->connect.ble_addr_type;
    s_peerValid = true;
    s_linked = true;
    s_authenticated = false;
    s_amsReady = false;
    s_tryCount = 0;
    s_lastTryMs = 0;
    s_hasPasskey = false;
    s_passkey = 0;
    setStatus("");

    s_clientStale = true;   // 旧链路上的虚拟客户端连接已失效，交给 loop 重建

    // 主动发起加密 → 未配对时 iPhone 会弹配对框
    esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
    Serial.printf("[AMS] connected, addr=%s\n", s_peer.toString().c_str());
  }

  void onDisconnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override
  {
    s_linked = false;
    s_authenticated = false;
    s_amsReady = false;
    s_peerValid = false;
    s_entityUpdate = nullptr;
    s_remoteCommand = nullptr;
    s_hasPasskey = false;
    clearMedia();
    setStatus("");

    // 不在这里调 esp_ble_gattc_close：回调跑在 BT 任务里，交给 loop 处理更安全
    if (s_client)
      s_clientStale = true;

    Serial.println("[AMS] disconnected, resume advertising");
    BLEDevice::startAdvertising();
  }
};

class AmsSecurityCallbacks : public BLESecurityCallbacks
{
  uint32_t onPassKeyRequest() override { return 123456; }

  void onPassKeyNotify(uint32_t passkey) override
  {
    s_passkey = passkey;
    s_hasPasskey = true;
    Serial.printf("[AMS] passkey = %06lu\n", (unsigned long)passkey);
  }

  bool onSecurityRequest() override { return true; }

  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override
  {
    s_hasPasskey = false;
    if (cmpl.success)
    {
      s_authenticated = true;
      s_authMs = millis();
      s_tryCount = 0;
      setStatus("");
      Serial.println("[AMS] pairing/encryption ok");
    }
    else
    {
      s_authenticated = false;
      setStatus("配对失败，请在 iPhone 上重试");
      Serial.println("[AMS] pairing FAILED");
    }
  }

  bool onConfirmPIN(uint32_t pin) override { return true; }
};

// ---------------- AMS 客户端（GATT Client）----------------

static bool startAmsClient()
{
  if (!s_peerValid)
    return false;

  if (!s_client)
  {
    s_client = BLEDevice::createClient();
    if (!s_client)
    {
      setStatus("创建 GATT 客户端失败");
      return false;
    }
  }

  // 新链路：先关掉旧链路上的虚拟连接（残留的服务缓存由下面 getServices() 重建）
  if (s_clientStale)
  {
    if (s_client->isConnected())
      s_client->disconnect();
    s_clientStale = false;
  }

  if (!s_client->isConnected() && !s_client->connect(s_peer, s_peerType))
  {
    setStatus("连接 iPhone 的 GATT 失败");
    return false;
  }

  // 服务发现：getServices() 内部会先清空旧链路的服务缓存再重新发现；
  // 加密后 iPhone 才会把 AMS 发布出来
  s_client->getServices();
  BLERemoteService *ams = s_client->getService(BLEUUID(AMS_SERVICE_UUID));
  if (!ams)
  {
    setStatus("未发现 AMS 服务（手机已解锁？）");
    return false;
  }

  BLERemoteCharacteristic *upd =
      ams->getCharacteristic(BLEUUID(AMS_ENTITY_UPDATE_UUID));
  BLERemoteCharacteristic *cmd =
      ams->getCharacteristic(BLEUUID(AMS_REMOTE_COMMAND_UUID));
  if (!upd || !cmd)
  {
    setStatus("AMS 特征缺失");
    return false;
  }

  upd->registerForNotify(onEntityUpdate);

  // 登记关注的实体/属性（Entity Update 必须先订阅，否则返回 0xA0）
  const uint8_t player[] = {AMS_ENTITY_PLAYER, AMS_PLAYER_NAME,
                            AMS_PLAYER_PLAYBACK_INFO, AMS_PLAYER_VOLUME};
  const uint8_t queue[] = {AMS_ENTITY_QUEUE, 0 /*Index*/, 1 /*Count*/};
  const uint8_t track[] = {AMS_ENTITY_TRACK, AMS_TRACK_ARTIST, AMS_TRACK_ALBUM,
                           AMS_TRACK_TITLE, AMS_TRACK_DURATION};

  upd->writeValue((uint8_t *)player, sizeof(player), true);
  upd->writeValue((uint8_t *)queue, sizeof(queue), true);
  upd->writeValue((uint8_t *)track, sizeof(track), true);

  s_entityUpdate = upd;
  s_remoteCommand = cmd;
  s_amsReady = true;
  setStatus("");
  Serial.println("[AMS] subscribed");
  return true;
}

// ---------------- 对外接口 ----------------

void AmsClient::begin()
{
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setPower(ESP_PWR_LVL_P9);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new AmsServerCallbacks());

  // 安全参数：Secure Connections + MITM + 绑定，本机只负责显示配对码
  BLESecurity *security = new BLESecurity();
  security->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  security->setCapability(ESP_IO_CAP_OUT);
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setKeySize(16);
  BLEDevice::setSecurityCallbacks(new AmsSecurityCallbacks());

  // 广播：标志 + AMS 128-bit Service Solicitation（0x15）
  // iOS 只对「主动索取 AMS」的外设连接并配对，没有这段就不会暴露服务。
  BLEUUID amsUuid(AMS_SERVICE_UUID);
  std::string solicitation;
  solicitation.reserve(18);
  solicitation += (char)17;
  solicitation += (char)ESP_BLE_AD_TYPE_128SOL_SRV_UUID;
  solicitation.append((const char *)amsUuid.getNative()->uuid.uuid128, 16);

  BLEAdvertisementData adv;
  adv.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  adv.addData(solicitation);

  BLEAdvertisementData scanResp;
  scanResp.setName(BLE_DEVICE_NAME);

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->setAdvertisementData(adv);
  advertising->setScanResponse(true);
  advertising->setScanResponseData(scanResp);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  advertising->start();

  clearMedia();
  Serial.printf("[AMS] advertising as \"%s\"\n", BLE_DEVICE_NAME);
}

void AmsClient::loop()
{
  // 链路已断：把旧链路上的虚拟 GATT 客户端连接收掉（不能在 BT 回调里做）
  if (s_clientStale && !s_linked && s_client)
  {
    if (s_client->isConnected())
      s_client->disconnect();
    s_clientStale = false;
  }

  if (!s_linked)
    return;

  if (!s_authenticated)
    return;

  if (s_amsReady)
  {
    if (!s_client || !s_client->isConnected())
    {
      s_amsReady = false;
      s_entityUpdate = nullptr;
      s_remoteCommand = nullptr;
    }
    return;
  }

  // 加密后给 iOS 一点时间发布服务，再开始发现；失败按 2s 间隔重试
  const uint32_t now = millis();
  if (now - s_authMs < 600)
    return;
  if (s_lastTryMs && now - s_lastTryMs < 2000)
    return;

  s_lastTryMs = now;
  s_tryCount++;
  if (!startAmsClient() && s_tryCount > 8)
    setStatus("AMS 服务不可用，请确认 iPhone 解锁后重连");
}

AmsLinkState AmsClient::linkState() const
{
  if (!s_linked)
    return AMS_LINK_ADVERTISING;
  if (!s_authenticated)
    return AMS_LINK_PAIRING;
  if (!s_amsReady)
    return AMS_LINK_CONNECTED;
  return AMS_LINK_READY;
}

bool AmsClient::hasPasskey() const { return s_hasPasskey; }

uint32_t AmsClient::passkey() const { return s_passkey; }

void AmsClient::statusText(char *out, size_t cap) const
{
  portENTER_CRITICAL(&s_mux);
  snprintf(out, cap, "%s", s_status);
  portEXIT_CRITICAL(&s_mux);
}

void AmsClient::snapshot(MediaInfo &out) const
{
  portENTER_CRITICAL(&s_mux);
  out = s_info;
  portEXIT_CRITICAL(&s_mux);

  if (!s_authenticated)
  {
    // 断开/配对中不显示上一次的内容
    out.hasTrack = false;
  }
}

uint32_t AmsClient::generation() const { return s_gen; }

float AmsClient::elapsedNow(const MediaInfo &info)
{
  float value = info.elapsed;
  if (info.playState == AMS_STATE_PLAYING && info.rate > 0)
    value += info.rate * (float)(millis() - info.elapsedAtMs) / 1000.0f;
  if (info.duration > 0 && value > info.duration)
    value = info.duration;
  return value < 0 ? 0 : value;
}

bool AmsClient::sendCommand(uint8_t cmd)
{
  if (!s_remoteCommand)
    return false;
  s_remoteCommand->writeValue((uint8_t *)&cmd, 1, true);
  return true;
}
