/**
 * @file lvgl_port.h
 * @brief LVGL 与 LCD / 触摸 / 节拍的适配层
 *
 * 职责：把 lcd_nv3030b 与 touch_cst816d 注册为 LVGL 显示与输入设备，
 * 提供 LVGL 心跳，并提供线程安全的锁，供非 UI 任务访问 LVGL。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief 初始化 LVGL 适配层（注册显示/输入设备并启动心跳）
 * @return ESP_OK 成功
 */
esp_err_t lvgl_port_init(void);

/**
 * @brief 获取 LVGL 互斥锁
 *
 * 任何非 UI 任务在调用 LVGL API 前必须先加锁，操作完成后调用
 * lvgl_port_unlock() 释放，避免与 UI 任务并发访问。
 *
 * @param timeout_ms 等待超时（毫秒），0 表示不等待
 * @return true 加锁成功；false 超时未获取到锁
 */
bool lvgl_port_lock(uint32_t timeout_ms);

/**
 * @brief 释放 LVGL 互斥锁
 */
void lvgl_port_unlock(void);
