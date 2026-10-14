/**
 * @file mock_buzzer.c
 * @brief 蜂鸣器 mock：记录响度/报警状态并打日志（模拟器无声）
 */
#include "buzzer.h"

#include "esp_log.h"

static const char *TAG = "buzzer_sim";

static bool s_inited = false;
static uint8_t s_volume = 40;
static bool s_alarm = false;

esp_err_t buzzer_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t buzzer_set_volume(uint8_t percent)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 1 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_volume = percent;
    ESP_LOGI(TAG, "响度 → %u%%（下一声生效）", (unsigned)percent);
    return ESP_OK;
}

esp_err_t buzzer_get_volume(uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    *percent = s_volume;
    return ESP_OK;
}

esp_err_t buzzer_play(buzzer_tone_t tone)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    static const char *names[] = { "CLICK", "OK", "ERROR" };
    const char *n = ((int)tone >= 0 && (int)tone < 3) ? names[tone] : "?";
    ESP_LOGI(TAG, "♪ PLAY %s（响度 %u%%）", n, (unsigned)s_volume);
    return ESP_OK;
}

esp_err_t buzzer_set_alarm(bool enable)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_alarm = enable;
    ESP_LOGI(TAG, "报警 %s", enable ? "开启" : "关闭");
    return ESP_OK;
}

esp_err_t buzzer_stop(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_alarm = false;
    ESP_LOGI(TAG, "静音");
    return ESP_OK;
}