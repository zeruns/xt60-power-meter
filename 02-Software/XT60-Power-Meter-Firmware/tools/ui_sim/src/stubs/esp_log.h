/**
 * @file esp_log.h
 * @brief 模拟器桩：ESP-IDF 日志宏 → 标准输出（保留级别前缀与 tag 便于对照）
 *
 * 简化点：没有时间戳与颜色；ERROR 走 stderr，其余级别走 stdout，
 * DEBUG/VERBOSE 直接编译为空语句。
 */
#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stdout, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stdout, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) \
    do {                        \
    } while (0)
#define ESP_LOGV(tag, fmt, ...) \
    do {                        \
    } while (0)