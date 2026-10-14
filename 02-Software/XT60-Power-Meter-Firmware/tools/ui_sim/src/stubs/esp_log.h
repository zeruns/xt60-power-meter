/**
 * @file esp_log.h
 * @brief 模拟器桩：ESP-IDF 日志宏 → 标准输出（保持与真机相同的格式便于对照）
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