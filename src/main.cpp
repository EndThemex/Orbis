#include <Arduino.h>
#include <Wire.h>

#include <TouchDrvCSTXXX.hpp>

#include "ams_client.h"
#include "board_pins.h"
#include "config.h"
#include "media_face.h"

// 显示
static Arduino_DataBus *bus = nullptr;
static Arduino_CO5300 *gfx = nullptr;

// 媒体页 + AMS
static AmsClient ams;
static MediaFace face;

// 触摸
static TouchDrvCST92xx touch;
static bool touchOk = false;
static bool touchDown = false;
static uint32_t lastTouchPollMs = 0;

static void initDisplay()
{
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
}

static Arduino_Canvas *createCanvas()
{
  // 466x466x2 = 434KB，放 PSRAM；有画布才能整帧重绘不闪
  Arduino_Canvas *canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, gfx);
  if (!canvas->begin(GFX_SKIP_OUTPUT_BEGIN) || !canvas->getFramebuffer())
  {
    Serial.println("[LCD] canvas alloc FAILED, 退回直绘模式");
    delete canvas;
    return nullptr;
  }
  Serial.println("[LCD] canvas ready (PSRAM framebuffer)");
  return canvas;
}

static void initTouch()
{
  touch.setPins(TP_RESET, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!touchOk)
  {
    Serial.println("[TOUCH] CST9217 init FAILED，屏幕按钮不可用");
    return;
  }
  touch.setMaxCoordinates(SCREEN_W, SCREEN_H);
#if TOUCH_SWAP_XY
  touch.setSwapXY(true);
#endif
#if TOUCH_MIRROR_X || TOUCH_MIRROR_Y
  touch.setMirrorXY(TOUCH_MIRROR_X, TOUCH_MIRROR_Y);
#endif
  Serial.printf("[TOUCH] %s ready\n", touch.getModelName());
}

static void pollTouch()
{
  if (!touchOk || millis() - lastTouchPollMs < 20)
    return;
  lastTouchPollMs = millis();

  int16_t x[1] = {0}, y[1] = {0};
  const uint8_t points = touch.getPoint(x, y, 1);

  if (points > 0)
  {
    // 只在按下的第一帧触发，避免长按连续发命令
    if (!touchDown)
    {
      touchDown = true;
      face.onTouch(x[0], y[0]);
    }
  }
  else
  {
    touchDown = false;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[Orbis] media remote boot");

  initDisplay();
  Arduino_Canvas *canvas = createCanvas();

  face.begin(gfx, canvas, &ams);
  initTouch();

  ams.begin();
  face.tick();   // 立刻出一帧「等待配对」，别让屏幕空着
}

void loop()
{
  ams.loop();
  pollTouch();
  face.tick();
  delay(10);
}
