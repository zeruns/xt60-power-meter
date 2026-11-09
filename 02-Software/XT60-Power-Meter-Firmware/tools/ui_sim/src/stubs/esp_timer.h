/**
 * @file esp_timer.h（UI 模拟器桩，v0.6.0 新增）
 * @brief ESP-IDF 软件定时器接口的最小桩
 *
 * 屏幕代码用 esp_timer_get_time() 取"自启动以来的微秒时间戳"，
 * 模拟器用系统时间换算成微秒近似同一量级（见 stubs.c 的实现；
 * 不保证严格单调，仅用于超时/耗时比较）。
 */
#pragma once

#include <stdint.h>

/** @brief 微秒时间戳（64 位；模拟器为系统时间换算的近似值） */
int64_t esp_timer_get_time(void);
