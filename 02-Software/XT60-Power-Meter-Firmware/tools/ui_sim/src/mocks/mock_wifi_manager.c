/**
 * @file mock_wifi_manager.c
 * @brief WiFi 管理的模拟器替身（v0.6.0 新增）
 *
 * 为什么需要：屏幕代码（screen_settings.c 的 WiFi 页 / ui_nav.c 的状态栏
 * WiFi 图标）现在直接依赖 wifi_manager 的状态、模式、RSSI 与扫描结果。
 * 模拟器没有真 WiFi，这里用一组可控的假数据把这些接口补齐，
 * 让"模式三档 / 扫描列表 / 密码键盘 / 状态栏四态"都能在 PC 上走查。
 *
 * 可控开关（供场景脚本调用）：
 *   mock_wifi_set_state()/mock_wifi_set_mode()/mock_wifi_set_rssi()
 *   mock_wifi_set_scan_list() —— 预置热点列表（默认 5 条）
 *   mock_wifi_scan_finish()   —— 结束"扫描中"状态（模拟 SCAN_DONE）
 */
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** 当前假状态（默认"已连接"，便于看到状态栏亮绿条） */
static wifi_manager_state_t s_state = WIFI_MANAGER_STATE_CONNECTED;
static wifi_manager_mode_t s_mode = WIFI_MANAGER_MODE_AUTO;
static int8_t s_rssi = -52;
static bool s_scan_busy = false;

/** 预置扫描结果（默认 5 条，覆盖"强/中/弱/开放"各种形态） */
static wifi_ap_record_t s_aps[8] = {
    { .ssid = "HomeWiFi-5G", .rssi = -45, .authmode = WIFI_AUTH_WPA2_PSK, .primary = 6 },
    { .ssid = "XT60-Lab", .rssi = -58, .authmode = WIFI_AUTH_WPA2_PSK, .primary = 1 },
    { .ssid = "Guest-Open", .rssi = -66, .authmode = WIFI_AUTH_OPEN, .primary = 11 },
    { .ssid = "TP-LINK_2.4G", .rssi = -74, .authmode = WIFI_AUTH_WPA2_PSK, .primary = 6 },
    { .ssid = "FarRoom", .rssi = -85, .authmode = WIFI_AUTH_WPA2_PSK, .primary = 3 },
};
static uint16_t s_ap_count = 5;

/* ---- 场景脚本用的可控接口 ---- */

void mock_wifi_set_state(wifi_manager_state_t st)
{
    s_state = st;
}

void mock_wifi_set_mode(wifi_manager_mode_t m)
{
    s_mode = m;
}

void mock_wifi_set_rssi(int8_t rssi)
{
    s_rssi = rssi;
}

/** @brief 预置扫描列表；n=0 表示"扫不到热点"（用于走查空列表形态） */
void mock_wifi_set_scan_list(uint16_t n)
{
    s_ap_count = (n > 8) ? 8 : n;
    printf("[sim] mock_wifi 扫描列表设置为 %u 条\n", (unsigned)s_ap_count);
}

/* ---- wifi_manager 公开接口的模拟实现 ---- */

esp_err_t wifi_manager_start(void)
{
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    return ESP_OK;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    return s_state;
}

wifi_manager_mode_t wifi_manager_get_mode(void)
{
    return s_mode;
}

esp_err_t wifi_manager_set_mode(wifi_manager_mode_t mode)
{
    printf("[sim] wifi_manager_set_mode(%d)\n", (int)mode);
    s_mode = mode;
    /* 模拟真实语义：切到热点模式 → 状态变 AP_MODE；切回 STA/AUTO → 连接中 */
    if (mode == WIFI_MANAGER_MODE_AP) {
        s_state = WIFI_MANAGER_STATE_AP_MODE;
    } else {
        s_state = WIFI_MANAGER_STATE_CONNECTING;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *pass)
{
    printf("[sim] wifi_manager_connect(SSID=%s, 密码长度=%u)\n", ssid ? ssid : "-",
           (unsigned)(pass ? strlen(pass) : 0u));
    s_state = WIFI_MANAGER_STATE_CONNECTED;
    return ESP_OK;
}

esp_err_t wifi_manager_forget(void)
{
    printf("[sim] wifi_manager_forget()\n");
    s_state = WIFI_MANAGER_STATE_AP_MODE;
    return ESP_OK;
}

esp_err_t wifi_manager_get_ip(char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < 16)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, out_len, "192.168.1.12");
    return (s_state == WIFI_MANAGER_STATE_CONNECTED) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_manager_get_ssid(char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < WIFI_MANAGER_SSID_MAX_LEN + 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, out_len, "HomeWiFi-5G");
    return ESP_OK;
}

esp_err_t wifi_manager_get_ap_ssid(char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < WIFI_MANAGER_AP_SSID_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, out_len, "XT60-Meter-A1B2");
    return ESP_OK;
}

/* v0.6.2 需求 4：WiFi 设置页在配网模式下显示热点密码，需该接口 */
esp_err_t wifi_manager_get_ap_pass(char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < 12)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, out_len, "xt60config");
    return ESP_OK;
}

esp_err_t wifi_manager_get_rssi(int8_t *out_rssi)
{
    if (out_rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != WIFI_MANAGER_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_rssi = s_rssi;
    return ESP_OK;
}

esp_err_t wifi_manager_scan_start(void)
{
    printf("[sim] wifi_manager_scan_start()\n");
    s_scan_busy = true; /* 真机是异步的；模拟器下一步由场景脚本显式结束扫描 */
    return ESP_OK;
}

bool wifi_manager_scan_is_running(void)
{
    return s_scan_busy;
}

/** @brief 供场景脚本结束"扫描中"状态（模拟 SCAN_DONE 事件） */
void mock_wifi_scan_finish(void)
{
    s_scan_busy = false;
}

esp_err_t wifi_manager_scan_get_ap_list(wifi_ap_record_t *out, uint16_t *inout_count)
{
    if ((out == NULL) || (inout_count == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t n = (*inout_count < s_ap_count) ? *inout_count : s_ap_count;
    memcpy(out, s_aps, (size_t)n * sizeof(wifi_ap_record_t));
    *inout_count = n;
    return (n > 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* ---- 桩：任务创建/删除（见 stubs/freertos/task.h 的语义说明） ---- */

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                                   void *arg, UBaseType_t prio, TaskHandle_t *out_handle, int core)
{
    (void)stack_bytes;
    (void)prio;
    (void)core;
    if (out_handle != NULL) {
        *out_handle = NULL;
    }
    printf("[sim] xTaskCreatePinnedToCore(%s) → 同步执行一次\n", name ? name : "-");
    if (fn != NULL) {
        fn(arg);
    }
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task; /* 模拟器无线程：空操作 */
}

TaskHandle_t xTaskGetHandle(const char *name)
{
    (void)name;
    return NULL;
}
