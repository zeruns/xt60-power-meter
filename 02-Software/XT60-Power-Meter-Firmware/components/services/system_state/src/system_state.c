/**
 * @file system_state.c
 * @brief 系统运行状态机实现
 *
 * 状态与指示灯模式通过两张并列的查找表映射，新增状态时只需同时补齐
 * s_state_names 与 s_state_pattern 两张表即可，逻辑代码无需改动。
 */
#include "system_state.h"

#include "status_led.h"

#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "sys_state";

/** 状态总数，用于参数合法性检查 */
#define STATE_COUNT 3

/** 状态名称表，下标即 system_state_t */
static const char *const s_state_names[STATE_COUNT] = {
    [SYSTEM_STATE_INIT] = "INIT",       /* 初始化中 */
    [SYSTEM_STATE_RUNNING] = "RUNNING", /* 正常运行 */
    [SYSTEM_STATE_FAULT] = "FAULT",     /* 异常 */
};

/** 状态 -> 指示灯模式映射表，下标即 system_state_t */
static const status_led_pattern_t s_state_pattern[STATE_COUNT] = {
    [SYSTEM_STATE_INIT] = STATUS_LED_PATTERN_SOLID,   /* 常亮 */
    [SYSTEM_STATE_RUNNING] = STATUS_LED_PATTERN_SLOW, /* 慢闪 1 Hz */
    [SYSTEM_STATE_FAULT] = STATUS_LED_PATTERN_FAST,   /* 快闪 5 Hz */
};

/** 当前状态 */
static system_state_t s_state = SYSTEM_STATE_INIT;

/** 是否已完成初始化（未初始化时拒绝状态迁移） */
static bool s_ready = false;

const char *system_state_name(system_state_t state)
{
    /* 越界时返回固定字符串，避免调用方拿到野指针 */
    if ((size_t)state >= STATE_COUNT) {
        return "UNKNOWN";
    }
    return s_state_names[state];
}

system_state_t system_state_get(void)
{
    return s_state;
}

esp_err_t system_state_init(void)
{
    /* 初始状态为 INIT：指示灯常亮，提示系统正在启动 */
    esp_err_t err = status_led_set_pattern(s_state_pattern[SYSTEM_STATE_INIT]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "指示灯模式设置失败: %s", esp_err_to_name(err));
        return err;
    }

    s_state = SYSTEM_STATE_INIT;
    s_ready = true;
    ESP_LOGI(TAG, "状态机就绪，当前状态: %s", system_state_name(s_state));
    return ESP_OK;
}

esp_err_t system_state_set(system_state_t state)
{
    /* 入参合法性检查 */
    if ((size_t)state >= STATE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 未初始化时不允许迁移，防止指示灯模式表被提前使用 */
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 状态未变化视为成功，避免重复设置指示灯模式与日志刷屏 */
    if (state == s_state) {
        return ESP_OK;
    }

    /* 先切换指示灯，成功后再更新状态，保证两者始终一致 */
    esp_err_t err = status_led_set_pattern(s_state_pattern[state]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "指示灯模式设置失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "状态迁移: %s -> %s", system_state_name(s_state), system_state_name(state));

    s_state = state;
    return ESP_OK;
}
