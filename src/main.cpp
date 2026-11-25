#include <Arduino.h>
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>

#include "board_pins.h"
#include "clock_face.h"
#include "config.h"

// 显示
static Arduino_DataBus *bus = nullptr;
static Arduino_CO5300 *gfx = nullptr;
static ClockFace face;

// 时间状态
static bool timeSynced = false;        // 是否已经从 NTP 拿到真实时间
static uint32_t lastWifiTryMs = 0;     // 上次尝试连 WiFi
static uint32_t lastSyncTryMs = 0;     // 上次尝试校时

// ============================================================
// 时间工具
// ============================================================

// 用编译时间兜底：没有 WiFi 也能看到钟在走，只是不准
static time_t buildEpoch()
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int day = 1, year = 2024;
  sscanf(__DATE__, "%3s %d %d", mon, &day, &year);

  int monIdx = 0;
  const char *p = strstr(months, mon);
  if (p)
    monIdx = (int)(p - months) / 3;

  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = monIdx;
  t.tm_mday = day;
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  t.tm_isdst = -1;
  return mktime(&t);
}

static void applyEpoch(time_t epoch)
{
  struct timeval tv = {};
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

// ============================================================
// WiFi / NTP
// ============================================================

static bool waitForWiFi(uint32_t timeoutMs)
{
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - start > timeoutMs)
      return false;
    Serial.print('.');
    delay(300);
  }
  Serial.println();
  return true;
}

// 等 SNTP 把系统时间改写掉（时间变了就说明同步成功）
static bool waitForNtpSync(time_t before, uint32_t timeoutMs)
{
  uint32_t start = millis();
  while (millis() - start < timeoutMs)
  {
    if (time(nullptr) != before)
      return true;
    delay(200);
  }
  return false;
}

static void startWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // 关省电，避免时钟走时抖动
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiTryMs = millis();
}

static bool syncFromNtp(uint32_t waitMs)
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  lastSyncTryMs = millis();
  time_t before = time(nullptr);
  configTzTime(TZ_STRING, NTP_SERVER_1, NTP_SERVER_2);   // 同时设置 TZ + 起 SNTP
  return waitForNtpSync(before, waitMs);
}

// ============================================================
// setup / loop
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[Orbis] round clock boot");

  // 屏幕
  // 注意：Arduino_GFX 1.6.0 的 CO5300 构造函数多一个 bool ips 参数（1.6.4 才去掉），
  //       漏掉它会让后续参数整体左移 → w/h/offset 全错，屏幕画不出东西。
  bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  gfx = new Arduino_CO5300(bus, LCD_RESET, 0 /* rotation */, false /* ips */,
                           SCREEN_W, SCREEN_H, LCD_COL_OFFSET, 0, 0, 0);
  if (!gfx->begin())
    Serial.println("[LCD] CO5300 init FAILED, 检查排线/供电");
  else
    Serial.printf("[LCD] CO5300 ready %dx%d\n", gfx->width(), gfx->height());

  gfx->fillScreen(0);
  gfx->setBrightness(CLOCK_BRIGHTNESS);
  Serial.printf("[MEM] PSRAM total %u KB, free %u KB\n",
                ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);

  // ---- 离屏画布：466x466x2 = 434KB，放 PSRAM ----
  // 有画布时每秒"整帧重绘 + 一次推送"，指针可以压在刻度和数字上且不闪；
  // 分配失败则退回直绘（会有轻微闪烁），功能不受影响。
  Arduino_Canvas *canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, gfx);
  bool canvasOk = canvas->begin(GFX_SKIP_OUTPUT_BEGIN) && canvas->getFramebuffer();
  if (!canvasOk)
  {
    Serial.println("[LCD] canvas alloc FAILED, 退回直绘模式");
    canvas = nullptr;
  }
  else
  {
    Serial.println("[LCD] canvas ready (PSRAM framebuffer)");
  }

  // ---- 时区 + 兜底时间 ----
  setenv("TZ", TZ_STRING, 1);
  tzset();
  time_t fallback = buildEpoch();
  applyEpoch(fallback);           // 以编译时间起步
  Serial.printf("[TIME] fallback = %s", ctime(&fallback));

  // ---- 表盘 ----
  face.begin(gfx, canvas);        // gfx = 面板，canvas 非空则离屏重绘 + 旋转推屏
  face.setSynced(false);          // 未校时 → 琥珀色秒针/秒环

  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  face.tick(tmv);

  // ---- 联网校时（失败不影响出画面）----
  startWifi();
  Serial.printf("[WiFi] connecting to %s\n", WIFI_SSID);
  if (waitForWiFi(15000))
  {
    Serial.printf("[WiFi] connected, IP = %s\n", WiFi.localIP().toString().c_str());
    if (syncFromNtp(10000))
    {
      timeSynced = true;
      Serial.println("[NTP] time synced");
    }
    else
    {
      Serial.println("[NTP] sync timeout");
    }
  }
  else
  {
    Serial.println("[WiFi] connect timeout, 先用编译时间运行");
  }

  face.setSynced(timeSynced);
  now = time(nullptr);
  localtime_r(&now, &tmv);
  face.tick(tmv);
}

void loop()
{
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);

  uint32_t t0 = micros();
  bool drew = face.tick(tmv);      // 内部按秒去重，只有秒变化才重绘
  static uint32_t lastPerfMs = 0;
  if (drew && millis() - lastPerfMs > 30000UL)
  {
    Serial.printf("[PERF] frame = %lu us\n", micros() - t0);
    lastPerfMs = millis();
  }

  // 断网重连
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTryMs > WIFI_RETRY_MS)
  {
    Serial.println("[WiFi] retry...");
    WiFi.disconnect();
    startWifi();
  }

  // 定期重新校时
  if (millis() - lastSyncTryMs > NTP_RESYNC_MS || (!timeSynced && millis() - lastSyncTryMs > 60000UL))
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("[NTP] resync...");
      bool ok = syncFromNtp(8000);
      if (ok != timeSynced)
        face.setSynced(ok);
      timeSynced = ok;
    }
    lastSyncTryMs = millis();
  }

  delay(80);
}
