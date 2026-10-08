/**
 * @file imu_sc7a20h.h
 * @brief SC7A20HTR 三轴加速度计驱动（I2C，7 位地址 0x19）
 *
 * 用途：检测握持方向，实现屏幕内容自动旋转。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/** 屏幕朝向 */
typedef enum {
    IMU_SC7A20H_ORIENT_PORTRAIT = 0,  /**< 竖屏 */
    IMU_SC7A20H_ORIENT_LANDSCAPE,     /**< 横屏 */
    IMU_SC7A20H_ORIENT_PORTRAIT_INV,  /**< 竖屏（倒置） */
    IMU_SC7A20H_ORIENT_LANDSCAPE_INV, /**< 横屏（倒置） */
} imu_sc7a20h_orientation_t;

/**
 * @brief 初始化 SC7A20HTR（配置量程与输出数据率）
 * @return ESP_OK 成功
 */
esp_err_t imu_sc7a20h_init(void);

/**
 * @brief 读取当前设备朝向
 * @param out 输出：朝向，不可为 NULL
 * @return ESP_OK 成功
 */
esp_err_t imu_sc7a20h_get_orientation(imu_sc7a20h_orientation_t *out);
