/**
 * @file orientation.h
 * @brief 屏幕重力自动旋转服务
 *
 * 周期性读取 SC7A20H 加速度计姿态（约 200ms 一次），经去抖（连续 6 次
 * 同向 ≈ 1.2 秒）后，把检测到的握持方向换算成屏幕角度（0/90/180/270），
 * 通过 lvgl_port_set_rotation_angle() 切换显示与触摸方向。
 *
 * 手动/自动：
 *   · 用户在「显示设置」点具体角度 → 关自动 + 应用该角度（持久化 settings）；
 *   · 打开「自动旋转」开关 → 本服务按重力接管；
 *   · 开机时由 app 层按 settings 恢复初始模式。
 *
 * IMU 朝向 → 角度的映射表在 .c 内，**依赖 IMU 与屏幕的实际装配方向**，
 * 若上板发现横竖颠倒，只改那张表（imu 驱动头文件中也有同样提示）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 启动方向监测任务（IMU 与 LVGL 适配层须已初始化）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 重复初始化；ESP_ERR_NO_MEM 建任务失败
 */
esp_err_t orientation_init(void);

/**
 * @brief 开机恢复：读 NVS 方向设置 → 应用手动角度 / 启动自动模式 + 监测任务
 *
 * 供 app_init 启动表调用（settings/IMU/LVGL 均已就绪）。幂等。
 */
esp_err_t orientation_boot_restore(void);

/**
 * @brief 启停重力自动旋转
 *
 * 启用后立即按当前姿态校正一次；停用后保持当前角度不动（由手动选择接管）。
 */
void orientation_set_auto(bool enable);

/**
 * @brief 查询当前是否处于自动旋转模式
 */
bool orientation_get_auto(void);
