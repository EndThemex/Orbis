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

// 屏幕开关 / PWR 按键
static bool screenOn = true;
static bool pmuOk = false;
static uint32_t lastPwrPollMs = 0;
static uint32_t lastPwrEdgeMs = 0;

// AXP2101 寄存器（只用 PWRKEY 边沿中断，够用且不引第三方库）
#define AXP2101_REG_IC_TYPE 0x03
#define AXP2101_REG_IRQ_EN1 0x41    // bit0 PWRKEY 正边沿 / bit1 负边沿 / bit3 短按
#define AXP2101_REG_IRQ_ST1 0x49    // 与 IRQ_EN1 对应的状态寄存器
#define AXP2101_PKEY_EDGE_MASK 0x03 // 正 / 负边沿

// ---- AXP2101 最小 I2C 访问 ----
static bool pmuWriteReg(uint8_t reg, uint8_t val)
{
  Wire.beginTransmission(IIC_ADDR_AXP2101);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool pmuReadRegs(uint8_t reg, uint8_t *buf, uint8_t len)
{
  Wire.beginTransmission(IIC_ADDR_AXP2101);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)IIC_ADDR_AXP2101, (uint8_t)len) != len)
    return false;
  for (uint8_t i = 0; i < len; i++)
    buf[i] = Wire.read();
  return true;
}

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
  pinMode(TP_RESET, OUTPUT);
  pinMode(TP_INT, INPUT_PULLUP);
  touch.setPins(TP_RESET, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);

  // I2C 总线扫描：确认触摸芯片/PMIC 等是否在线
  Serial.print("[I2C] found:");
  uint8_t found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++)
  {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0)
    {
      Serial.printf(" 0x%02X", a);
      found++;
    }
  }
  Serial.printf(" (%u)\n", found);

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

  uint16_t rx = 0, ry = 0;
  touch.getResolution(rx, ry);
  Serial.printf("[TOUCH] %s ready, chip res=%ux%u, max=%ux%u, swap=%d\n",
                touch.getModelName(), rx, ry, SCREEN_W, SCREEN_H, TOUCH_SWAP_XY);
}

// ---------------- 屏幕开关 ----------------
// 官方例程（04_LVGL_AXP2101_ADC_Data 与综合项目）就是靠 AXP2101 的 PWRKEY 中断
// 触发 toggleBacklight：AMOLED 亮度写 0 即整屏黑，恢复时写回 CLOCK_BRIGHTNESS。
static void screenSleep()
{
  if (!screenOn)
    return;
  screenOn = false;
  gfx->setBrightness(0);
  Serial.println("[PWR] screen off");
}

static void screenWake()
{
  if (screenOn)
    return;
  screenOn = true;
  // 息屏期间没跑 face.tick()，先在黑屏下补画一帧再恢复亮度，避免闪出旧画面
  face.invalidate();
  face.tick();
  gfx->setBrightness(CLOCK_BRIGHTNESS);
  Serial.println("[PWR] screen on");
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
      if (!screenOn)
      {
        // 息屏时触摸只负责点亮，不再下发给界面
        screenWake();
        return;
      }
      const bool hit = face.onTouch(x[0], y[0]);
      Serial.printf("[TOUCH] down (%d,%d) pts=%u hit=%d\n", x[0], y[0], points, hit);
    }
  }
  else
  {
    touchDown = false;
  }
}

static void initPmu()
{
  uint8_t id = 0;
  if (!pmuReadRegs(AXP2101_REG_IC_TYPE, &id, 1))
  {
    Serial.println("[PMU] AXP2101 无响应，PWR 键不可用");
    return;
  }
  Serial.printf("[PMU] AXP2101 online, IC type=0x%02X\n", id);

  // 打开 PWRKEY 正/负边沿中断，状态寄存器才会锁存按键事件
  uint8_t en = 0;
  if (pmuReadRegs(AXP2101_REG_IRQ_EN1, &en, 1))
    pmuWriteReg(AXP2101_REG_IRQ_EN1, en | AXP2101_PKEY_EDGE_MASK);

  // 清掉上电过程中残留的中断，避免一开机就误判一次按键
  uint8_t st[3] = {0};
  if (pmuReadRegs(AXP2101_REG_IRQ_ST1 - 1, st, 3))
  {
    pmuWriteReg(AXP2101_REG_IRQ_ST1 - 1, st[0]);
    pmuWriteReg(AXP2101_REG_IRQ_ST1, st[1]);
    pmuWriteReg(AXP2101_REG_IRQ_ST1 + 1, st[2]);
  }
  pmuOk = true;
}

// PWR 键：一次单击会先后产生「按下」「松开」两个边沿，500ms 内只认第一次
static void pollPowerKey()
{
  if (!pmuOk || millis() - lastPwrPollMs < 20)
    return;
  lastPwrPollMs = millis();

  uint8_t st = 0;
  if (!pmuReadRegs(AXP2101_REG_IRQ_ST1, &st, 1))
    return;
  if (!(st & AXP2101_PKEY_EDGE_MASK))
    return;
  pmuWriteReg(AXP2101_REG_IRQ_ST1, st); // 写 1 清中断

  if (millis() - lastPwrEdgeMs < 500)
    return;
  lastPwrEdgeMs = millis();

  if (screenOn)
    screenSleep();
  else
    screenWake();
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
  initPmu();

  ams.begin();
  face.tick();   // 立刻出一帧「等待配对」，别让屏幕空着
}

void loop()
{
  ams.loop();
  pollPowerKey();
  pollTouch();
  if (screenOn)
    face.tick();
  delay(10);
}
