/**
 * @file stubs.c
 * @brief 模拟器桩实现：错误码名字 + 应用描述
 */
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

#include <string.h>
#include <time.h>

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
        case ESP_OK:
            return "ESP_OK";
        case ESP_FAIL:
            return "ESP_FAIL";
        case ESP_ERR_NO_MEM:
            return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG:
            return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE:
            return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_NOT_FOUND:
            return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_TIMEOUT:
            return "ESP_ERR_TIMEOUT";
        default:
            return "ESP_ERR_UNKNOWN";
    }
}

const esp_app_desc_t *esp_app_get_description(void)
{
    /* 版本号可在构建时用 -DSIM_FW_VERSION="x.y.z" 覆盖，默认与 version.txt 对齐 */
#ifndef SIM_FW_VERSION
#define SIM_FW_VERSION "0.1.0"
#endif
    static esp_app_desc_t desc;
    static int inited = 0;
    if (!inited) {
        desc.magic_word = 0xABCD5432u;
        strncpy(desc.version, SIM_FW_VERSION, sizeof(desc.version) - 1);
        strncpy(desc.project_name, "XT60-Power-Meter", sizeof(desc.project_name) - 1);
        strncpy(desc.idf_ver, "sim", sizeof(desc.idf_ver) - 1);
        inited = 1;
    }
    return &desc;
}

/* ---- UI 刷新率（真机由 ui.c 实现；模拟器用独立变量供菜单联调） ---- */
static uint8_t s_sim_rate_hz = 5;

uint8_t ui_get_display_rate_hz(void)
{
    return s_sim_rate_hz;
}

void ui_set_display_rate_hz(uint8_t hz)
{
    s_sim_rate_hz = hz;
}

/* ---- ESP-IDF 软件定时器桩：与真机同语义的"单调微秒时间戳" ----
 * 真机上 esp_timer_get_time() 返回自启动以来的微秒数；模拟器用
 * timespec_get(CLOCK_MONOTONIC) 换算，保证单调递增、量级一致。 */
int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}