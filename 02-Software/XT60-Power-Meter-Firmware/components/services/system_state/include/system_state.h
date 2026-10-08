/**
 * @file system_state.h
 * @brief 系统运行状态机
 *
 * 三态模型，状态变化统一驱动状态指示灯：
 *   INIT    -> 常亮      （上电初始化中）
 *   RUNNING -> 慢闪 1 Hz （全部服务就绪，正常运行）
 *   FAULT   -> 快闪 5 Hz （出现不可自恢复错误）
 *
 * 说明：FAULT 为设计上的终态，只允许通过 system_state_set() 显式恢复为
 * RUNNING；后续引入具体故障码时在本模块内扩展。
 */
#pragma once

#include "esp_err.h"

/** 系统运行状态 */
typedef enum {
    SYSTEM_STATE_INIT = 0, /**< 初始化中 */
    SYSTEM_STATE_RUNNING,  /**< 正常运行 */
    SYSTEM_STATE_FAULT,    /**< 异常 */
} system_state_t;

/**
 * @brief 初始化状态机并进入 INIT 状态（指示灯常亮）
 * @return ESP_OK 成功
 */
esp_err_t system_state_init(void);

/**
 * @brief 获取当前状态
 */
system_state_t system_state_get(void);

/**
 * @brief 状态迁移
 *
 * 迁移成功时同步更新状态指示灯，并打印迁移日志。
 * 目标状态与当前状态相同时直接返回 ESP_OK。
 *
 * @param state 目标状态
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 状态非法；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t system_state_set(system_state_t state);

/**
 * @brief 获取状态名称（用于日志）
 */
const char *system_state_name(system_state_t state);
