/**
 * @file aht20.h
 * @brief AHT20 温湿度传感器驱动（I2C，7 位地址 0x38）
 *
 * 用途：采集功率计内部环境温湿度。
 *
 * 协议要点（详见 src/aht20.c）：
 *   上电 ≥40ms 后读状态，未校准则先发初始化命令 0xBE 0x08 0x00；
 *   每次测量先发触发命令 0xAC 0x33 0x00，等 ≥80ms 再读 7 字节，
 *   解析出湿度 20bit 与温度 20bit。
 *
 * 合理量程：湿度 0~100 %RH（驱动侧钳制）、温度 -40~85 °C（器件量程，按原值输出）。
 * 数据完整性由读回的第 7 字节 CRC8 把关，校验不过本次读数直接丢弃。
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
