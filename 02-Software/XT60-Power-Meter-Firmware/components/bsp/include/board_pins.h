/**
 * @file board_pins.h
 * @brief XT60 功率计上板 V2 引脚映射（全工程唯一真相源）
 *
 * 依据：01-Hardware/02-上板原理图_V2.pdf（更新日期 2026-09-22）
 *
 * 约定：除本文件外，任何模块不得直接书写 GPIO 编号，必须引用 BOARD_PIN_* 。
 */
#pragma once

#include "driver/gpio.h"

/* ===================== 状态指示灯 ===================== */
/* 电路：3V3 -> R25 1kΩ -> LED2(KT-0603R) -> IO39，低电平点亮 */
#define BOARD_PIN_SYS_LED           GPIO_NUM_39
#define BOARD_SYS_LED_ACTIVE_LEVEL  0

/* ===================== 蜂鸣器（无源，PWM） ===================== */
/* 电路：IO1 -> BUZZER1(4kHz 无源蜂鸣器)，经 Q5(AO3400A) 低边 N-MOS 驱动
 *
 * 关于驱动极性的说明（容易搞错）：
 *   低边 N-MOS 的栅极由 IO1 控制，源极接 GND，蜂鸣器接在电源与漏极之间。
 *   因此 IO1 输出高电平 → MOS 导通 → 蜂鸣器得电发声。
 *   用 LEDC 输出方波驱动：占空比 50% 时声音最响，占空比 0 即静音。
 *
 * LEDC 资源分配（避免与 LCD 背光冲突，全工程统一规划）：
 *   蜂鸣器    → LEDC 通道 0 / 定时器 0（本文件）
 *   LCD 背光  → LEDC 通道 1 / 定时器 1（lcd_nv3030b 驱动使用）
 */
#define BOARD_PIN_BUZZER            GPIO_NUM_1

/* ===================== LCD 背光（PWM） ===================== */
/* 电路：IO45 -> BL_CTR，经 Q3(AO3400A) 低边驱动 LED- */
#define BOARD_PIN_LCD_BL            GPIO_NUM_45

/* ===================== 电池电压采样 ===================== */
/* 电路：VBAT -> R32 100kΩ -> BAT_ADC -> R38 100kΩ -> GND
 * 即 1:1 分压，实际电池电压 = ADC 引脚电压 × BOARD_BAT_ADC_DIVIDER_RATIO */
#define BOARD_PIN_BAT_ADC           GPIO_NUM_2
#define BOARD_BAT_ADC_DIVIDER_RATIO 2

/* ===================== 按键（低电平有效） ===================== */
#define BOARD_PIN_KEY_BOOT          GPIO_NUM_0 /* K1，兼 BOOT 键 */
#define BOARD_PIN_KEY_USER          GPIO_NUM_4 /* K2 */

/* ===================== LCD（QSPI，NV3030B，240x284） ===================== */
#define BOARD_PIN_LCD_CS            GPIO_NUM_15 /* 原理图网络 LCD_CS   */
#define BOARD_PIN_LCD_SCK           GPIO_NUM_18 /* LCD_SCK            */
#define BOARD_PIN_LCD_SDIO0         GPIO_NUM_16 /* LCD_MOSI           */
#define BOARD_PIN_LCD_SDIO1         GPIO_NUM_17 /* LCD_D1             */
#define BOARD_PIN_LCD_SDIO2         GPIO_NUM_3  /* LCD_D2             */
#define BOARD_PIN_LCD_SDIO3         GPIO_NUM_8  /* LCD_D3             */
#define BOARD_PIN_LCD_RST           GPIO_NUM_7  /* LCD_RST            */
#define BOARD_PIN_LCD_TE            GPIO_NUM_48 /* LCD_TE 撕裂同步     */

/* ===================== 电容触摸（CST816D，I2C） ===================== */
#define BOARD_PIN_TP_SCL            GPIO_NUM_5
#define BOARD_PIN_TP_SDA            GPIO_NUM_6
#define BOARD_PIN_TP_INT            GPIO_NUM_46
#define BOARD_TP_I2C_ADDR           0x15

/* ===================== ADS131M02（SPI，24bit ΔΣ ADC） ===================== */
#define BOARD_PIN_ADC_SCLK          GPIO_NUM_11 /* ADC_SCLK  */
#define BOARD_PIN_ADC_MOSI          GPIO_NUM_9  /* ADC_MOSI  (DIN)  */
#define BOARD_PIN_ADC_MISO          GPIO_NUM_10 /* ADC_MISO  (DOUT) */
#define BOARD_PIN_ADC_DRDY          GPIO_NUM_12 /* ADC_DRDY# 下降沿有效 */
#define BOARD_PIN_ADC_RST           GPIO_NUM_13 /* ADC_RST = SYNC#/RESET# */
#define BOARD_PIN_ADC_CLK           GPIO_NUM_14 /* ESP_CLK -> ADC_CLK (CLKIN) */

/* ===================== 传感器 I2C 总线 ===================== */
/* 上拉：2.2kΩ x N（R11/R19/R41/R43/R44/R45/R46） */
#define BOARD_PIN_I2C_SCL           GPIO_NUM_21
#define BOARD_PIN_I2C_SDA           GPIO_NUM_47
#define BOARD_AHT20_I2C_ADDR        0x38 /* 温湿度 */
#define BOARD_SC7A20_I2C_ADDR       0x19 /* 三轴加速度计（用于自动转屏） */

/* ===================== USB（原生 USB-Serial/JTAG） ===================== */
/* 电路：IO19/IO20 -> R34/R36 22Ω -> USB_D-/USB_D+ */
#define BOARD_PIN_USB_DM            GPIO_NUM_19
#define BOARD_PIN_USB_DP            GPIO_NUM_20

/* =====================================================================
 * Strapping 引脚约束（复位期间电平决定启动行为，硬件设计已保证，
 * 软件须避免在启动早期将其拉向非默认电平）
 *   IO0  -> K1/BOOT 键：复位期间必须为高，按住复位会进入下载模式
 *   IO3  -> LCD_D2    ：LCD 侧为输入，上电默认高
 *   IO45 -> BL_CTR    ：经 R10 10kΩ 下拉，上电默认低
 *   IO46 -> TP_INT    ：触摸中断，上电默认高
 * ===================================================================== */

/* =====================================================================
 * 未使用 / 不可用 IO
 *   IO35 ~ IO37  被模组内部八线 PSRAM 占用，不可作普通 IO
 *   IO38 / IO40 ~ IO44 悬空（IO43=TXD0、IO44=RXD0 未接，控制台走 USB-JTAG）
 *   IO33 / IO34  被模组内部八线 PSRAM 占用（SPIIO4/SPIIO5）
 * ===================================================================== */
