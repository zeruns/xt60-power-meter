/**
 * @file mock_orientation.c
 * @brief orientation 服务模拟器 mock：只记录自动开关（无 IMU，角度切换由
 *        手动按钮触发，模拟器场景可直接调 lvgl_port_set_rotation_angle 验证）。
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "orientation_sim";
static bool s_auto = false;

esp_err_t orientation_init(void)
{
    return ESP_OK;
}

esp_err_t orientation_boot_restore(void)
{
    return ESP_OK;
}

void orientation_set_auto(bool enable)
{
    s_auto = enable;
    ESP_LOGI(TAG, "重力自动旋转（模拟）：%s", enable ? "开" : "关");
}

bool orientation_get_auto(void)
{
    return s_auto;
}
