/**
 * @file esp_err.h
 * @brief 模拟器桩：ESP-IDF 错误码（仅覆盖 UI / 服务层用到的符号）
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107

/** @brief 错误码 → 名字（stubs.c 实现，仅用于日志） */
const char *esp_err_to_name(esp_err_t code);

#ifdef __cplusplus
}
#endif