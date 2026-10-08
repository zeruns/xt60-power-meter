/**
 * @file event_bus.h
 * @brief 应用事件总线（跨任务 / 跨核的事件分发）
 *
 * 用于把按键、传感器、测量告警等事件从产生方投递到消费方，
 * 是跨核通信的两条允许通道之一（另一条为 data_store 快照）。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/** 应用事件类型 */
typedef enum {
    APP_EVENT_BUTTON = 0,          /**< 按键事件 */
    APP_EVENT_ORIENTATION_CHANGED, /**< 屏幕朝向变化 */
    APP_EVENT_ENV_UPDATED,         /**< 温湿度更新 */
    APP_EVENT_MEASURE_ALERT,       /**< 测量告警（过压/过流等） */
    APP_EVENT_WIFI_STATE_CHANGED,  /**< WiFi 状态变化 */
    APP_EVENT_MAX,                 /**< 事件类型数量哨兵 */
} app_event_type_t;

/** 事件载体：type 决定 data 中哪个成员有效 */
typedef struct {
    app_event_type_t type; /**< 事件类型 */
    union {
        int32_t i32[2]; /**< 整型负载（如按键编号、状态码） */
        float f32[2];   /**< 浮点负载（如电压、电流） */
    } data;
} app_event_t;

/**
 * @brief 事件处理回调（在总线任务上下文执行，须快速返回，不可阻塞）
 * @param event    事件内容，生命周期仅限回调内
 * @param user_ctx 订阅时传入的用户上下文
 */
typedef void (*app_event_handler_t)(const app_event_t *event, void *user_ctx);

/**
 * @brief 初始化事件总线（创建内部队列）
 * @return ESP_OK 成功
 */
esp_err_t event_bus_init(void);

/**
 * @brief 发布一个事件
 * @param event      事件内容，不可为 NULL
 * @param timeout_ms 队列满时的等待超时（毫秒）
 * @return ESP_OK 成功；ESP_ERR_TIMEOUT 队列满且等待超时
 */
esp_err_t event_bus_post(const app_event_t *event, uint32_t timeout_ms);

/**
 * @brief 订阅某类事件
 * @param type     关注的事件类型
 * @param handler  回调函数，不可为 NULL
 * @param user_ctx 用户上下文，回调时原样传回
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 订阅表已满
 */
esp_err_t event_bus_subscribe(app_event_type_t type, app_event_handler_t handler, void *user_ctx);
