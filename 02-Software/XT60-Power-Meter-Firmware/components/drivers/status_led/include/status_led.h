/**
 * @file status_led.h
 * @brief 系统运行状态指示灯（SYS_LED / IO39）
 *
 * 硬件：3V3 -> R25 1kΩ -> LED2 -> IO39，低电平点亮。
 * 驱动在内部启动一个低优先级任务按当前模式循环翻转电平，
 * 上层只需通过 status_led_set_pattern() 切换模式。
 */
#pragma once

#include "esp_err.h"

/** 指示灯闪烁模式 */
typedef enum {
    STATUS_LED_PATTERN_SOLID = 0, /**< 常亮：系统初始化中 */
    STATUS_LED_PATTERN_SLOW,      /**< 慢闪 1 Hz：正常运行 */
    STATUS_LED_PATTERN_FAST,      /**< 快闪 5 Hz：异常状态 */
} status_led_pattern_t;

/**
 * @brief 初始化状态指示灯（配置 GPIO 并启动闪烁任务）
 * @return ESP_OK 成功
 */
esp_err_t status_led_init(void);

/**
 * @brief 切换闪烁模式
 *
 * 新模式在当前闪烁周期结束后生效，调用方无需等待。
 *
 * @param pattern 目标模式
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 模式非法；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t status_led_set_pattern(status_led_pattern_t pattern);
