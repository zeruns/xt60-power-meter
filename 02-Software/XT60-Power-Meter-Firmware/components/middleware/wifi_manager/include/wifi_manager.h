/**
 * @file wifi_manager.h
 * @brief WiFi 连接管理（STA / AP 模式与配网状态机）
 *
 * 说明：ESP-IDF 默认把 WiFi 任务钉在 Core 0，与本工程
 *       “Core 0 负责交互与通信、Core 1 负责 ADS131M02 采样” 的划分一致，
 *       无需修改 CONFIG_WIFI_TASK_CORE_ID。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/** WiFi 工作状态 */
typedef enum {
    WIFI_MANAGER_STATE_IDLE = 0,   /**< 未启动 */
    WIFI_MANAGER_STATE_CONNECTING, /**< 正在连接热点 */
    WIFI_MANAGER_STATE_CONNECTED,  /**< 已连接并获取到 IP */
    WIFI_MANAGER_STATE_AP_MODE,    /**< 工作在配网热点模式 */
    WIFI_MANAGER_STATE_FAILED,     /**< 连接失败 */
} wifi_manager_state_t;

/**
 * @brief 启动 WiFi 管理（读取 NVS 凭据，按需进入 STA 或 AP 模式）
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief 停止 WiFi 并释放相关资源
 * @return ESP_OK 成功
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief 获取当前 WiFi 状态
 * @return 当前状态
 */
wifi_manager_state_t wifi_manager_get_state(void);
