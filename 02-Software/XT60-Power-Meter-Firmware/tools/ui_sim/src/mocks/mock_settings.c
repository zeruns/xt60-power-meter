/**
 * @file mock_settings.c
 * @brief 配置服务 mock：内存缓存（模拟"防抖落盘"的最终效果）
 *
 * 与真机 settings 的行为差异：不做防抖定时器与 NVS，set 直接改内存并打日志；
 * UI 层的调用方式（set_xxx + save_now）与真机完全一致。
 */
#include "settings.h"

#include <stdbool.h>

#include "esp_log.h"

static const char *TAG = "settings_sim";

static settings_t s_cache = {
    .buzzer_volume = 40,
    .lcd_brightness = 80,
    .display_rate_hz = 5,
    .orientation_auto = 0,
    .orientation_angle = 0,
};
static bool s_ready = false;

esp_err_t settings_init(void)
{
    s_ready = true;
    ESP_LOGI(TAG, "初始化（模拟内存缓存：响度 %u%%，亮度 %u%%）", s_cache.buzzer_volume,
             s_cache.lcd_brightness);
    return ESP_OK;
}

esp_err_t settings_get(settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_cache;
    return ESP_OK;
}

esp_err_t settings_set_buzzer_volume(uint8_t percent)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 1 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cache.buzzer_volume = percent;
    ESP_LOGI(TAG, "响度 → %u%%（模拟持久化，防抖写 NVS）", (unsigned)percent);
    return ESP_OK;
}

esp_err_t settings_set_lcd_brightness(uint8_t percent)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 10 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cache.lcd_brightness = percent;
    ESP_LOGI(TAG, "亮度 → %u%%（模拟持久化，防抖写 NVS）", (unsigned)percent);
    return ESP_OK;
}

esp_err_t settings_save_now(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "立即落盘（模拟）");
    return ESP_OK;
}

esp_err_t settings_set_display_rate_hz(uint8_t hz)
{
    s_cache.display_rate_hz = hz;
    return ESP_OK;
}

esp_err_t settings_set_orientation(uint8_t auto_on, uint16_t angle)
{
    s_cache.orientation_auto = auto_on;
    s_cache.orientation_angle = angle;
    return ESP_OK;
}