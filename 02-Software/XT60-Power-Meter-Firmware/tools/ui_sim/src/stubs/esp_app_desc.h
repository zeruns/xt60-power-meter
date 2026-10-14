/**
 * @file esp_app_desc.h
 * @brief 模拟器桩：应用描述（UI 用 esp_app_get_description()->version 显示版本号）
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t magic_word;
    uint32_t secure_version;
    uint32_t reserv1[2];
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint32_t reserv2[20];
} esp_app_desc_t;

/** @brief 取应用描述（stubs.c 返回固定的模拟值） */
const esp_app_desc_t *esp_app_get_description(void);

#ifdef __cplusplus
}
#endif