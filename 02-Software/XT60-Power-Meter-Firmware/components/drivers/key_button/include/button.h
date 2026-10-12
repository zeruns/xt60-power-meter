/**
 * @file button.h
 * @brief 实体按键驱动（K1=IO0 兼 BOOT，K2=IO4，均为低电平有效）
 *
 * IO0 为 strapping 脚：复位期间须保持高电平，按住该键复位会进入下载模式，
 * 因此驱动须在启动早期忽略该键（仅记录，不触发业务动作）。
 *
 * 实现要点（详见 src/button.c）：
 *   用官方 esp_driver_gpio + esp_timer 周期轮询（10ms），语义判定
 *   （消抖/单击/双击/长按）由纯逻辑状态机 button_fsm.h 完成，未使用托管
 *   组件 espressif/button（其 button_event_t 与本文件同名类型冲突，且需为
 *   IO0 的 strapping 特性定制安全策略，理由见 src/button.c 文件头）。
 *
 * 事件时序（详见 button_fsm.h）：
 *   · SINGLE_CLICK 在释放后 300ms 双击窗口超时后才派发（有意延迟）；
 *   · DOUBLE_CLICK 为第二次释放后窗口超时派发；
 *   · LONG_PRESS_START 按住 800ms 派发一次，松开派发 LONG_PRESS_RELEASE。
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

/** 按键事件类型（与 button_fsm 的语义事件一一对应） */
typedef enum {
    BUTTON_EVENT_SINGLE_CLICK = 0,  /**< 单击（双击窗口超时后确认） */
    BUTTON_EVENT_DOUBLE_CLICK,      /**< 双击 */
    BUTTON_EVENT_LONG_PRESS_START,  /**< 长按达到阈值（仅一次） */
    BUTTON_EVENT_LONG_PRESS_RELEASE,/**< 长按后松开 */
} button_event_t;

/**
 * @brief 按键事件回调
 * @param id       产生事件的按键
 * @param event    事件类型
 * @param user_ctx 注册时传入的用户上下文
 *
 * 注意：回调运行在 esp_timer 任务上下文，禁止阻塞/长耗时操作，
 * 也不要直接操作 LVGL；请向业务任务投递消息。
 */
typedef void (*button_event_cb_t)(button_id_t id, button_event_t event, void *user_ctx);

/**
 * @brief 初始化按键驱动（配置 GPIO 输入、消抖轮询与状态机）
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
