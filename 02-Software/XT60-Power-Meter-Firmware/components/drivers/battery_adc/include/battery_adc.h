/**
 * @file battery_adc.h
 * @brief 电池电压采样驱动（BOARD_PIN_BAT_ADC / IO2）
 *
 * 电路：VBAT -> R32 100kΩ -> IO2 -> R38 100kΩ -> GND，为 1:1 分压，
 * 因此实际电池电压 = 引脚电压 × BOARD_BAT_ADC_DIVIDER_RATIO(=2)。
 * 建议多次采样取均值，并按实际分压电阻误差引入标定系数。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 初始化电池采样通道（ADC 单次采样模式）
 * @return ESP_OK 成功
 */
esp_err_t battery_adc_init(void);

/**
 * @brief 读取电池电压
 * @param voltage_mv 输出：电池电压（毫伏），不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t battery_adc_read(uint32_t *voltage_mv);
