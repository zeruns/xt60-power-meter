/**
 * @file buzzer.h
 * @brief 无源蜂鸣器驱动（BOARD_PIN_BUZZER，经 Q5 低边驱动）
 *
 * 无源蜂鸣器须由方波驱动，使用 LEDC 产生约 4 kHz（谐振点）载波，
 * 通过启停与频率变化实现不同提示音。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/** 提示音类型 */
typedef enum {
    BUZZER_TONE_CLICK = 0, /**< 短促按键音 */
    BUZZER_TONE_OK,        /**< 操作成功 */
    BUZZER_TONE_ERROR,     /**< 操作失败 / 告警 */
} buzzer_tone_t;

/**
 * @brief 初始化蜂鸣器（配置 LEDC 通道，默认静音）
 * @return ESP_OK 成功
 */
esp_err_t buzzer_init(void);

/**
 * @brief 播放一次提示音（非阻塞，播放完毕自动停止）
 * @param tone 提示音类型
 * @return ESP_OK 成功
 */
esp_err_t buzzer_play(buzzer_tone_t tone);

/**
 * @brief 立即停止发声
 * @return ESP_OK 成功
 */
esp_err_t buzzer_stop(void);
