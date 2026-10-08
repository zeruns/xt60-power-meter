/**
 * @file web_console.h
 * @brief 网页控制台（HTTP 静态页 + WebSocket 实时数据 / 参数下发）
 *
 * 数据来源：services/power_measure 的快照与 middleware/data_store；
 * 参数设置经 services/settings 落 NVS。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 启动网页控制台（HTTP 服务器与 WebSocket 推送任务）
 * @return ESP_OK 成功
 */
esp_err_t web_console_start(void);

/**
 * @brief 停止网页控制台
 * @return ESP_OK 成功
 */
esp_err_t web_console_stop(void);
