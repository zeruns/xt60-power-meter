/**
 * @file device_ctrl.h
 * @brief 设备控制服务：按键语义、提示音与状态灯业务映射
 *
 * 把驱动层的原始事件（按键按下、测量告警）翻译为面向用户的行为：
 * 按键动作分发、蜂鸣器提示、指示灯状态切换。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 初始化设备控制服务（订阅按键与告警事件）
 *
 * 依赖 middleware/event_bus 已初始化。
 *
 * @return ESP_OK 成功
 */
esp_err_t device_ctrl_init(void);
