/**
 * @file button.h
 * @brief 实体按键驱动（K1=IO0 兼 BOOT，K2=IO4，均为低电平有效）
 *
 * IO0 为 strapping 脚：复位期间须保持高电平，按住该键复位会进入下载模式，
 * 因此驱动须在启动早期忽略该键（仅记录，不触发业务动作）。
 *
 * 实现要点（详见 src/button.c）：
 *   用官方 esp_driver_gpio + esp_timer 周期轮询消抖，未使用托管组件
 *   espressif/button（其 button_event_t 与本文件同名类型冲突，且需为
 *   IO0 的 strapping 特性定制安全策略，理由见 src/button.c 文件头）。
 * 注意：本驱动只提供事件回调，没有可供查询的读取接口。
 */
#pragma once

#include "esp_err.h"

/** 按键编号 */
typedef enum {
    BUTTON_ID_BOOT = 0, /**< K1 / IO0，兼 BOOT 键 */
    BUTTON_ID_USER,     /**< K2 / IO4 */
    BUTTON_ID_MAX,      /**< 按键数量哨兵 */
} button_id_t;

/** 按键事件类型 */
typedef enum {
    BUTTON_EVENT_SHORT_PRESS = 0, /**< 短按（按下并释放） */
    BUTTON_EVENT_LONG_PRESS,      /**< 长按达到阈值 */
    BUTTON_EVENT_RELEASE,         /**< 释放 */
} button_event_t;

/**
 * @brief 按键事件回调
 * @param id       产生事件的按键
 * @param event    事件类型
 * @param user_ctx 注册时传入的用户上下文
 */
typedef void (*button_event_cb_t)(button_id_t id, button_event_t event, void *user_ctx);

/**
 * @brief 初始化按键驱动（配置 GPIO 输入与消抖）
 * @return ESP_OK 成功
 */
esp_err_t button_init(void);

/**
 * @brief 注册按键事件回调
 * @param id       目标按键
 * @param cb       回调函数，不可为 NULL
 * @param user_ctx 用户上下文，回调时原样传回
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t button_register_callback(button_id_t id, button_event_cb_t cb, void *user_ctx);
