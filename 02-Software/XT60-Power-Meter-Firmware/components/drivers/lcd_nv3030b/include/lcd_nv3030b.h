/**
 * @file lcd_nv3030b.h
 * @brief NV3030B LCD 面板驱动（240x284 IPS，QSPI）+ 背光 PWM
 *
 * 面板：TK018F3716 模组，QSPI 数据线 4bit（见 board_pins.h 的 BOARD_PIN_LCD_*）；
 * 背光：BOARD_PIN_LCD_BL（IO45）经 Q3 低边驱动，由 LEDC 输出 PWM 调光。
 * 本驱动只负责面板初始化、刷新与背光控制；LVGL 适配见 middleware/lvgl_port。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/** 面板分辨率 */
#define LCD_NV3030B_H_RES 240 /**< 水平像素数 */
#define LCD_NV3030B_V_RES 284 /**< 垂直像素数 */

/**
 * @brief 初始化 LCD 面板（QSPI 总线、复位时序、面板初始化命令、背光 PWM）
 * @return ESP_OK 成功
 */
esp_err_t lcd_nv3030b_init(void);

/**
 * @brief 设置背光亮度
 * @param percent 亮度百分比 0~100（0 为完全关闭）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 超出范围
 */
esp_err_t lcd_nv3030b_set_backlight(uint8_t percent);
