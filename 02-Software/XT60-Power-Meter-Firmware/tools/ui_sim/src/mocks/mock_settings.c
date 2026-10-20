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
    .wave_smooth = 0,
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

esp_err_t settings_set_wave_smooth(uint8_t on)
{
    s_cache.wave_smooth = on ? 1u : 0u;
    return ESP_OK;
}

/* ============================================================
 * 以下为 v0.5.0 / v0.6.0 补齐的接口
 * （模拟器此前与真机 API 脱节，链接时报"无法解析的外部符号"，
 *  这里一次性补齐，恢复"模拟 = 真机"的回归闭环）
 * ============================================================ */

/** 告警阈值的模拟缓存（与真机一样是"内存 + 防抖落盘"） */
static settings_alarm_t s_alarm = {
    .v_en = 1u,
    .i_en = 0u,
    .p_en = 0u,
    .v_limit = 15.0f,
    .i_limit = 5.0f,
    .p_limit = 50.0f,
};

esp_err_t settings_get_alarm(settings_alarm_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_alarm;
    return ESP_OK;
}

esp_err_t settings_set_alarm(const settings_alarm_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 与真机同样的校验口径：阈值必须是有限正数，否则整体拒绝 */
    if (!(in->v_limit > 0.0f) || !(in->i_limit > 0.0f) || !(in->p_limit > 0.0f)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_alarm = *in;
    ESP_LOGI(TAG, "告警阈值 → V %.2f(%u) / I %.2f(%u) / P %.2f(%u)（模拟落盘）",
             (double)s_alarm.v_limit, (unsigned)s_alarm.v_en, (double)s_alarm.i_limit,
             (unsigned)s_alarm.i_en, (double)s_alarm.p_limit, (unsigned)s_alarm.p_en);
    return ESP_OK;
}

esp_err_t settings_set_wave_window(uint8_t window)
{
    ESP_LOGI(TAG, "波形时间窗 → %u（模拟落盘）", (unsigned)window);
    return ESP_OK;
}

esp_err_t settings_set_wave_mode(uint8_t mode)
{
    ESP_LOGI(TAG, "波形显示模式 → %u（模拟落盘）", (unsigned)mode);
    return ESP_OK;
}

esp_err_t settings_set_screen_off(uint8_t level)
{
    ESP_LOGI(TAG, "自动息屏档位 → %u（模拟落盘）", (unsigned)level);
    return ESP_OK;
}

esp_err_t settings_restore_factory(void)
{
    ESP_LOGI(TAG, "恢复出厂设置（模拟：只打日志，不改内存缓存）");
    return ESP_OK;
}

esp_err_t settings_request_save(void)
{
    /* v0.6.0：真机是"置脏 + 唤醒后台落盘任务"；模拟器没有后台任务，
     * 直接打日志表示"已请求落盘"，语义对 UI 而言等价。 */
    ESP_LOGI(TAG, "请求落盘（模拟，非阻塞）");
    return ESP_OK;
}