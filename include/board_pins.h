#pragma once

// ============================================================
// Waveshare ESP32-S3-Touch-AMOLED-1.75 引脚定义
// 来源：官方仓库 examples/arduino/libraries/Mylibrary/pin_config.h
// 注意：1.75C 版无 TF 卡槽，但屏幕/触摸引脚一致。
// ============================================================

// ---- CO5300 AMOLED（QSPI）----
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39

// 面板可视区域 466x466，控制器 480x480，列偏移 6
#define SCREEN_W 466
#define SCREEN_H 466
#define LCD_COL_OFFSET 6

// ---- CST9217 触摸（I2C）----
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40

// ---- AXP2101 电源管理（PWR 按键接在 PWRKEY 上，不是 GPIO）----
// 官方例程通过 AXP2101 的 PKEY 中断识别 PWR 短按，enable 寄存器 0x40~0x42 /
// 状态寄存器 0x48~0x4A 一一对应
#define IIC_ADDR_AXP2101 0x34

// ---- SD 卡（1-bit SDMMC，挂载期间勿复用 GPIO1/2/3）----
#define SDMMC_CLK 2
#define SDMMC_CMD 1
#define SDMMC_DATA 3
