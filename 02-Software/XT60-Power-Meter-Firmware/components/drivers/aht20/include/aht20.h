/**
 * @file aht20.h
 * @brief AHT20 温湿度传感器驱动（I2C，7 位地址 0x38）
 *
 * 用途：采集功率计内部环境温湿度。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/** 温湿度读数 */
typedef struct {
    float temperature_c; /**< 温度，摄氏度 */
    float humidity_pct;  /**< 相对湿度，百分比 0~100 */
} aht20_reading_t;

/**
 * @brief 初始化 AHT20（上电校准与首次测量触发）
 * @return ESP_OK 成功
 */
esp_err_t aht20_init(void);

/**
 * @brief 读取一次温湿度
 * @param out 输出：温湿度读数，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_TIMEOUT 器件未响应
 */
esp_err_t aht20_read(aht20_reading_t *out);
