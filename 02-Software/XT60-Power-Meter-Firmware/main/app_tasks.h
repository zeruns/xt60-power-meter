/**
 * @file app_tasks.h
 * @brief 应用任务编排与双核分配
 *
 * 依据：ESP-IDF 默认把 WiFi 任务钉在 Core 0，因此按“通信/交互与采样分离”划分：
 *
 *   Core 0 (PRO_CPU) —— 交互与通信核
 *   Core 1 (APP_CPU) —— 测量核（ADS131M02 采样与功率计算独占，避免被通信任务抢占）
 *
 * 任务规划表：
 * | 任务              | 核 | 周期      | 优先级 | 状态   |
 * |-------------------|----|-----------|--------|--------|
 * | task_sys_monitor  | 0  | 5 s       | 3      | 已实现 |
 * | task_ui           | 0  | 事件/33ms | 12     | 待实现 |
 * | task_input        | 0  | 事件      | 10     | 待实现 |
 * | task_env_sensor   | 0  | 1 s       | 5      | 待实现 |
 * | task_net          | 0  | 事件      | 5      | 待实现 |
 * | task_power_sample | 1  | 1 ms      | 22     | 待实现 |
 * | task_power_calc   | 1  | 10 ms     | 18     | 待实现 |
 * | task_battery_mon  | 1  | 1 s       | 5      | 待实现 |
 *
 * 跨核数据只允许通过 data_store（无锁快照）与 event_bus（事件）传递，
 * 禁止跨核直接共享可变状态。
 */
#pragma once

#include "esp_err.h"

#define APP_CORE_UI      0 /**< PRO_CPU：UI / 触摸 / 传感器 / WiFi / 网页 */
#define APP_CORE_MEASURE 1 /**< APP_CPU：ADS131M02 采样与计算 */

/**
 * @brief 创建全部应用任务
 *
 * 由 app_main() 在板级与外设初始化完成后调用。
 *
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t app_tasks_start(void);
