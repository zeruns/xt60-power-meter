/**
 * @file touch_cst816d.h
 * @brief CST816D 电容触摸驱动（I2C，7 位地址 0x15）
 *
 * 引脚：BOARD_PIN_TP_SCL / BOARD_PIN_TP_SDA / BOARD_PIN_TP_INT。
 * 中断 IO46 为 strapping 脚，初始化时须避免将其拉低。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 触摸点坐标 */
typedef struct {
    uint16_t x;   /**< 横坐标（像素） */
    uint16_t y;   /**< 纵坐标（像素） */
    bool pressed; /**< true 表示当前有按下 */
} touch_cst816d_point_t;

/**
 * @brief 初始化 CST816D（配置 I2C 与中断输入）
 * @return ESP_OK 成功
 */
esp_err_t touch_cst816d_init(void);

/**
 * @brief 读取当前触摸点
 * @param out 输出：触摸点信息，不可为 NULL
 * @return ESP_OK 成功
 */
esp_err_t touch_cst816d_read(touch_cst816d_point_t *out);
