/**
 * @file stubs.c
 * @brief 模拟器桩实现：错误码名字、应用描述、UI 刷新率、用户活动上报、时间戳
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
    /* 版本号由构建参数 -DSIM_FW_VERSION="x.y.z" 注入（tools/ui_sim/CMakeLists.txt
     * 当前写的是 0.2.0，并未从 version.txt 自动取值）；未注入时用下面的默认值。
     * 注意：它与固件版本（version.txt）不是同一来源。 */
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

/* ---- 用户活动上报（v0.6.2 需求 11）----
 * 真机由 ui.c 实现（重置自动降亮计时 / 恢复亮度）；模拟器不跑背光逻辑，
 * 这里给个无副作用的空实现，只为让同一份 ui_nav.c 能在两种环境编译链接。 */
void ui_note_user_activity(void)
{
}

/* ---- ESP-IDF 软件定时器桩：微秒时间戳 ----
 * 真机上 esp_timer_get_time() 返回自启动以来的微秒数（单调递增）；
 * 模拟器用 timespec_get(TIME_UTC) 取系统时间换算成微秒，量级一致，
 * 但不保证严格单调（只用于超时/耗时比较，不作为计时基准）。 */
int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}