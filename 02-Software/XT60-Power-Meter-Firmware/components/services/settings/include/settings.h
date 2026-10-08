/**
 * @file settings.h
 * @brief 配置持久化服务（NVS）
 *
 * 保存标定参数、WiFi 凭据、背光亮度、界面偏好等。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 初始化配置服务（初始化 NVS 分区）
 * @return ESP_OK 成功
 */
esp_err_t settings_init(void);

/**
 * @brief 从 NVS 载入全部配置到内存缓存（缺失项使用默认值）
 * @return ESP_OK 成功
 */
esp_err_t settings_load(void);

/**
 * @brief 将内存缓存中的配置写回 NVS
 * @return ESP_OK 成功
 */
esp_err_t settings_save(void);
