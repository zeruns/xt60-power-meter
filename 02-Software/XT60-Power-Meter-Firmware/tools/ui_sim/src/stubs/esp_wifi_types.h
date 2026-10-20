/**
 * @file esp_wifi_types.h（UI 模拟器桩，v0.6.0 新增）
 * @brief esp_wifi 类型定义的最小桩
 *
 * 真机 wifi_manager.h 里直接透传 wifi_ap_record_t（扫描结果），
 * 模拟器需要同名同形的结构体才能编译同一个 UI 源码。
 * 只保留 UI 真正读取的字段：ssid / rssi / authmode / primary。
 */
#pragma once

#include <stdint.h>

/** 认证方式（仅保留界面用到的两个：开放 / WPA2-PSK） */
typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WEP,
    WIFI_AUTH_WPA_PSK,
    WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA2_ENTERPRISE,
    WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK,
    WIFI_AUTH_MAX,
} wifi_auth_mode_t;

/** 扫描到的热点信息（真机为同名结构体，字段更多；此处只需界面用到的） */
typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[33]; /**< 热点名（UTF-8，含结尾 0） */
    uint8_t primary;  /**< 主信道 */
    int8_t rssi;      /**< 信号强度（dBm） */
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;
