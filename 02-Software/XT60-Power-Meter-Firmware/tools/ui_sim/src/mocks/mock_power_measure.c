/**
 * @file mock_power_measure.c
 * @brief 测量服务 mock：确定性假波形（与真机同一套 API 与命名表）
 *
 * 假数据围绕 19.9767 V / 0.01886 A / 0.3768 W 波动（与真机台架测试值一致），
 * 保证波形页与测量页截图上的数字与现实观感相同；波形生成完全确定性
 * （正弦叠加），同一场景每次渲染逐像素一致。
 */
#include "power_measure.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "sim.h"

static const char *TAG = "pm_sim";

static power_measure_calib_t s_calib = {
    .voltage_gain = 1.250000e-05f,
    .voltage_offset = 0.0f,
    .current_gain = 3.05175781e-06f,
    .current_offset = 0.0f,
};

/** 各窗口时长（毫秒）——与真机 s_window_map 口径一致 */
static const uint32_t s_win_ms[PM_WAVE_WINDOW_MAX] = { 1000, 10000, 30000, 60000, 120000, 600000 };

esp_err_t power_measure_init(void)
{
    return ESP_OK;
}

esp_err_t power_measure_start(void)
{
    return ESP_OK;
}

esp_err_t power_measure_set_calibration(const power_measure_calib_t *calib)
{
    if (calib == NULL || calib->voltage_gain <= 0.0f || calib->current_gain <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    s_calib = *calib;
    ESP_LOGI(TAG, "标定已更新（模拟持久化）：V gain=%.6e / I gain=%.6e",
             (double)calib->voltage_gain, (double)calib->current_gain);
    return ESP_OK;
}

/**
 * @brief 异步版标定写入的模拟实现（v0.6.0 新增）
 *
 * 真机：只改内存 + 置"待落盘"标志，NVS 写入交给 task_aux（校准页的卡死修复）。
 * 模拟器：没有 NVS，直接改内存并打日志，让校准页的"应用校准"流程可完整走查。
 */
esp_err_t power_measure_set_calibration_deferred(const power_measure_calib_t *calib)
{
    if (calib == NULL || calib->voltage_gain <= 0.0f || calib->current_gain <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    s_calib = *calib;
    ESP_LOGI(TAG, "标定已更新（模拟：内存即时生效，落盘异步）：V gain=%.6e / I gain=%.6e",
             (double)calib->voltage_gain, (double)calib->current_gain);
    return ESP_OK;
}

void power_measure_get_calibration(power_measure_calib_t *out)
{
    if (out != NULL) {
        *out = s_calib;
    }
}

esp_err_t power_measure_reset_energy(void)
{
    sim_zero_energy();
    ESP_LOGI(TAG, "电能与运行时间已清零（模拟：立即生效）");
    return ESP_OK;
}

uint32_t power_measure_sample_rate_hz(void)
{
    return 4000;
}

const char *pm_wave_window_name(pm_wave_window_t win)
{
    static const char *names[PM_WAVE_WINDOW_MAX] = {
        "1s", "10s", "30s", "60s", "120s", "10min",
    };
    return (win < PM_WAVE_WINDOW_MAX) ? names[win] : "?";
}

const char *pm_wave_mode_name(pm_wave_mode_t mode)
{
    static const char *names[PM_WAVE_MODE_MAX] = {
        "功率", "电流+功率", "电压+功率", "电压+电流", "电能",
    };
    return (mode < PM_WAVE_MODE_MAX) ? names[mode] : "?";
}

/** @brief 给定「窗口内相对时刻 t（秒）」的假数据 */
static void fake_point_at(float t, float *v, float *i, float *p, float *e)
{
    *v = 19.97670f + 0.0040f * sinf(6.2831853f * 0.5f * t);
    *i = 0.018860f + 0.00040f * sinf(6.2831853f * 0.7f * t + 1.0f);
    *p = (*v) * (*i);
    *e = 0.11800f + 0.0000005f * (t * 4000.0f);
}

static void mode_fields(pm_wave_mode_t mode, int *fa, int *fb, bool *has_b)
{
    switch (mode) {
    case PM_WAVE_MODE_P:  *fa = 2; *fb = 0; *has_b = false; break;
    case PM_WAVE_MODE_IP: *fa = 1; *fb = 2; *has_b = true;  break;
    case PM_WAVE_MODE_VP: *fa = 0; *fb = 2; *has_b = true;  break;
    case PM_WAVE_MODE_VI: *fa = 0; *fb = 1; *has_b = true;  break;
    case PM_WAVE_MODE_E:  *fa = 3; *fb = 0; *has_b = false; break;
    default:              *fa = 2; *fb = 0; *has_b = false; break;
    }
}

int pm_wave_copy(pm_wave_window_t win, pm_wave_mode_t mode, float *a, float *b, int max_points)
{
    if (a == NULL || max_points <= 0 || win >= PM_WAVE_WINDOW_MAX || mode >= PM_WAVE_MODE_MAX) {
        return 0;
    }
    int fa, fb;
    bool has_b;
    mode_fields(mode, &fa, &fb, &has_b);

    /* 与真机同一时间槽契约：槽 j = 屏幕第 j 列，右端(max_points-1)=现在，
     * 每槽固定 = 窗长/槽数。模拟器假定窗口已填满，故每槽都有数据。 */
    const float step_s = ((float)s_win_ms[win] / 1000.0f) / (float)max_points;
    for (int j = 0; j < max_points; j++) {
        const float t = (float)(max_points - 1 - j) * step_s;
        float vals[4];
        fake_point_at(t, &vals[0], &vals[1], &vals[2], &vals[3]);
        a[j] = vals[fa];
        if (has_b && b != NULL) {
            b[j] = vals[fb];
        }
    }
    return max_points;
}

bool pm_wave_get_range(pm_wave_window_t win, pm_wave_mode_t mode, float *a_min, float *a_max,
                       float *b_min, float *b_max)
{
    (void)win;
    if (a_min == NULL || a_max == NULL) {
        return false;
    }
    switch (mode) {
    case PM_WAVE_MODE_P:
        *a_min = 0.35f; *a_max = 0.41f;
        return false;
    case PM_WAVE_MODE_IP:
        *a_min = 0.018f; *a_max = 0.0195f;
        if (b_min) *b_min = 0.35f;
        if (b_max) *b_max = 0.41f;
        return true;
    case PM_WAVE_MODE_VP:
        *a_min = 19.95f; *a_max = 20.00f;
        if (b_min) *b_min = 0.35f;
        if (b_max) *b_max = 0.41f;
        return true;
    case PM_WAVE_MODE_VI:
        *a_min = 19.95f; *a_max = 20.00f;
        if (b_min) *b_min = 0.018f;
        if (b_max) *b_max = 0.0195f;
        return true;
    case PM_WAVE_MODE_E:
        *a_min = 0.118f; *a_max = 0.130f;
        return false;
    default:
        *a_min = 0.0f; *a_max = 1.0f;
        return false;
    }
}