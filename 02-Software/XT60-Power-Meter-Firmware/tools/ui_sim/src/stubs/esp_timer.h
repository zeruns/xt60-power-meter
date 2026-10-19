/**
 * @file esp_timer.h（UI 模拟器桩，v0.6.0 新增）
 * @brief ESP-IDF 软件定时器接口的最小桩
 *
 * 屏幕代码用 esp_timer_get_time() 取"自启动以来的微秒时间戳"，
 * 模拟器用单调时钟返回同样的语义（见 stubs.c 的实现）。
 */
#pragma once

#include <stdint.h>

/** @brief 自启动以来的微秒数（单调递增，64 位） */
int64_t esp_timer_get_time(void);
