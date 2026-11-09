/**
 * @file event_bus.h
 * @brief 应用事件总线：让模块之间「发通知」而不「直接互相调用」
 *
 * ─────────────────────────────────────────────────────────────
 * 这个文件在整个系统中扮演什么角色？
 * ─────────────────────────────────────────────────────────────
 * 假设「系统状态机」发生变化（比如从"初始化中"变成"正常运行"），
 * 它需要通知别人去改变指示灯。有两种做法：
 *
 *   做法一（直接调用，耦合）：
 *       状态机代码里写 status_led_set_pattern(...)
 *       → 问题：状态机（业务层）被迫认识指示灯（驱动层），
 *               以后想再加"蜂鸣器响一声"就得改状态机代码。
 *
 *   做法二（事件总线，解耦）：★ 本工程采用
 *       状态机只喊一句"我的状态变了"，把消息丢进总线；
 *       谁关心这件事谁就事先"订阅"。状态机完全不知道订阅者是谁。
 *       → 好处：加一个订阅者（比如蜂鸣器）不需要改状态机一行代码。
 *
 * 这就是设计模式里的「观察者模式」，也是嵌入式里做「依赖倒置」的常用手段。
 *
 * ─────────────────────────────────────────────────────────────
 * 工作机制（配合 event_bus.c 阅读）
 * ─────────────────────────────────────────────────────────────
 *   发布者 --event_bus_post()--> [FreeRTOS 队列] --分发任务取出--> 逐个调用订阅者的回调
 *
 * 三个要点：
 *   1. 发布是「异步」的：post 只是把消息塞进队列就返回，不等待处理完成。
 *      好处是发布者不会被慢订阅者拖住；代价是需要一块队列内存。
 *   2. 回调在「分发任务」的上下文里执行，不是中断里。
 *      所以回调里可以调用 FreeRTOS 的普通 API，但绝不能长时间阻塞
 *      （否则会堵住后面所有事件的分发）。
 *   3. 队列里存的是事件结构体的「副本」，不是指针。
 *      所以发布者不需要保证内存一直有效，避免悬空指针。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 事件类型
 *
 * 每新增一种事件，都要在 APP_EVENT_MAX 之前添加，
 * 因为 APP_EVENT_MAX 被当作「类型总数」用来做参数合法性检查。
 */
typedef enum {
    APP_EVENT_SYSTEM_STATE_CHANGED = 0, /**< 系统运行状态发生变化（状态灯、蜂鸣器等关心） */
    APP_EVENT_BUTTON,                   /**< 按键事件（短按/长按） */
    APP_EVENT_ORIENTATION_CHANGED,      /**< 屏幕朝向变化（加速度计检测到，UI 需要转屏） */
    APP_EVENT_ENV_UPDATED,              /**< 温湿度数据更新 */
    APP_EVENT_MEASURE_ALERT,            /**< 测量告警（过压/过流/过功率/过温，见 DATA_ALERT_*） */
    APP_EVENT_WIFI_STATE_CHANGED,       /**< WiFi 连接状态变化 */
    APP_EVENT_MAX,                      /**< 哨兵：事件类型总数，必须放在最后 */
} app_event_type_t;

/** 系统状态变化事件的负载 */
typedef struct {
    uint8_t
        state; /**< 新状态，取值对应 system_state_t（此处用 uint8_t 以避免本文件依赖状态机模块） */
    uint32_t fault_mask; /**< 当前故障位图，取值对应 system_fault_t 的位或 */
} app_event_sys_state_t;

/** 按键事件负载 */
typedef struct {
    uint8_t id;     /**< 哪个按键，对应 button_id_t */
    uint8_t action; /**< 什么动作，对应 button_event_t */
} app_event_button_t;

/** 温湿度事件负载 */
typedef struct {
    float temperature_c; /**< 温度（摄氏度） */
    float humidity_pct;  /**< 相对湿度（百分比） */
} app_event_env_t;

/** 测量告警负载 */
typedef struct {
    float voltage_v;     /**< 触发告警时的电压（V） */
    float current_a;     /**< 触发告警时的电流（A） */
    uint32_t alert_mask; /**< 哪些告警被触发（位图） */
} app_event_alert_t;

/**
 * @brief 事件对象
 *
 * type 决定 data 联合体里哪个成员有效。
 * 用联合体（union）而不是各类型独立结构体，是为了让队列只存一种元素类型，
 * 且队列每个元素大小固定、可预期。
 */
typedef struct {
    app_event_type_t type; /**< 事件类型，决定 data 的解读方式 */
    union {
        app_event_sys_state_t sys_state; /**< type == APP_EVENT_SYSTEM_STATE_CHANGED 时有效 */
        app_event_button_t button;       /**< type == APP_EVENT_BUTTON 时有效 */
        app_event_env_t env;             /**< type == APP_EVENT_ENV_UPDATED 时有效 */
        app_event_alert_t alert;         /**< type == APP_EVENT_MEASURE_ALERT 时有效 */
        uint32_t raw[3];                 /**< 通用后备区：把联合体固定为 12 字节（整个 app_event_t 为 16 字节） */
    } data;
} app_event_t;

/**
 * @brief 事件回调函数类型
 *
 * @param event    事件内容。注意：它的生命周期仅限本次回调，
 *                 如果需要保存，请自行把内容拷贝走（不要保存这个指针）。
 * @param user_ctx 订阅时传入的用户上下文，回调时原样传回，用来区分不同订阅者。
 */
typedef void (*app_event_handler_t)(const app_event_t *event, void *user_ctx);

/**
 * @brief 初始化事件总线（创建队列与分发任务）
 *
 * 必须在任何 event_bus_post() / event_bus_subscribe() 之前调用，
 * 且应在 app_main() 的最前面执行——它是其他模块解耦通信的基础设施。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_NO_MEM 队列或任务创建失败（内存不足）
 */
esp_err_t event_bus_init(void);

/**
 * @brief 发布一个事件（把消息投递到总线）
 *
 * 本函数不会等待订阅者处理完，投递成功即返回。
 *
 * @param event      事件内容，不可为 NULL（内部会做副本）
 * @param timeout_ms 队列满时的等待时间（毫秒）。
 *                   传 0 表示"不等待，队列满就放弃"——定时器/回调里应传 0，
 *                   避免阻塞在通信路径上。
 *
 * @return ESP_OK 投递成功
 *         ESP_ERR_INVALID_ARG 参数非法
 *         ESP_ERR_INVALID_STATE 总线尚未初始化
 *         ESP_ERR_TIMEOUT 队列已满且等待超时（事件被丢弃，可用
 *                          event_bus_dropped_count() 观察是否发生）
 */
esp_err_t event_bus_post(const app_event_t *event, uint32_t timeout_ms);

/**
 * @brief 订阅某类事件
 *
 * 订阅应在系统启动阶段完成（各模块的 xxx_init 里），
 * 这样进入正常运行后就不会漏掉事件。
 *
 * @param type     关心的事件类型
 * @param handler  回调函数，不可为 NULL
 * @param user_ctx 用户上下文，回调时原样传回
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 参数非法
 *         ESP_ERR_NO_MEM 订阅表已满（见 event_bus.c 中的容量常量）
 */
esp_err_t event_bus_subscribe(app_event_type_t type, app_event_handler_t handler, void *user_ctx);

/**
 * @brief 查询因队列满而被丢弃的事件总数
 *
 * 用于诊断：如果这个数字持续增长，说明有订阅者处理太慢，
 * 或者队列长度设置得太小。
 *
 * @return 累计丢弃的事件数
 */
uint32_t event_bus_dropped_count(void);
