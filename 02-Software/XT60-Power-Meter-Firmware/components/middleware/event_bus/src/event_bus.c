/**
 * @file event_bus.c
 * @brief 事件总线实现：FreeRTOS 队列 + 静态订阅表 + 分发任务
 *
 * 实现要点与「为什么这样做」：
 *
 * 1. 用 FreeRTOS 队列而不是链表：xQueueSend/xQueueReceive 自带线程安全
 *    与阻塞/唤醒机制，发布者与分发任务之间不需要额外加锁。
 *
 * 2. 订阅表用静态数组而不是动态链表：嵌入式设备最怕运行期内存碎片，
 *    订阅者数量在编译期就能确定上限，用数组最省心。
 *
 * 3. 分发时「先复制回调指针、再在临界区外调用」：因为回调函数内部
 *    可能又调用 event_bus_subscribe()（比如某模块初始化时），
 *    如果在持锁状态下调用回调，就会自己等自己，造成死锁。
 */
#include "event_bus.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/** 日志标签：ESP_LOGx 宏会把它打印在每行日志的开头，便于定位来源 */
static const char *TAG = "event_bus";

/** 事件队列长度（能同时积压多少个未处理事件） */
#define EVENT_BUS_QUEUE_LEN       16

/** 订阅者数量上限（内存受限设备上做静态分配，避免运行期碎片） */
#define EVENT_BUS_MAX_SUBSCRIBERS 12

/** 分发任务的栈大小（字节）与优先级 */
#define EVENT_BUS_TASK_STACK      3072
#define EVENT_BUS_TASK_PRIO       6

/**
 * 订阅者表项
 *
 * 一条订阅 = (关心的事件类型, 回调函数, 用户上下文)
 * 同一个事件类型可以有多条订阅（一对多广播）。
 */
typedef struct {
    app_event_type_t type;       /**< 关心的事件类型 */
    app_event_handler_t handler; /**< 回调函数 */
    void *user_ctx;              /**< 用户上下文 */
} subscriber_t;

/** 订阅表（静态分配，容量固定） */
static subscriber_t s_subscribers[EVENT_BUS_MAX_SUBSCRIBERS];

/** 当前已注册的订阅者数量 */
static size_t s_subscriber_count = 0;

/**
 * 保护订阅表的自旋锁
 *
 * portMUX_TYPE + portENTER_CRITICAL 是 FreeRTOS 提供的最轻量互斥手段：
 * 它通过「短暂关闭中断/调度」来保证临界区不被打断，开销极小。
 * 适用于「只做几个赋值就退出」的极短临界区——本文件正是这种场景。
 *
 * 注意：临界区内绝不能做耗时操作（如打印日志、阻塞等待），
 *       否则会严重影响系统实时性。
 */
static portMUX_TYPE s_subscriber_lock = portMUX_INITIALIZER_UNLOCKED;

/** 事件队列句柄 */
static QueueHandle_t s_queue = NULL;

/** 因队列满而丢弃的事件计数（诊断用） */
static volatile uint32_t s_dropped_count = 0;

uint32_t event_bus_dropped_count(void)
{
    return s_dropped_count;
}

/**
 * @brief 把一个事件分发给所有匹配的订阅者
 *
 * 执行流程：
 *   ① 在临界区内，把所有匹配的 (回调, 上下文) 复制到局部数组，然后立刻退出临界区；
 *   ② 在临界区外逐个调用回调。
 *
 * 为什么必须先复制？见文件头「实现要点 3」。
 * 局部数组大小与订阅表上限一致，所以复制过程绝不会越界。
 */
static void event_bus_dispatch(const app_event_t *event)
{
    app_event_handler_t handlers[EVENT_BUS_MAX_SUBSCRIBERS];
    void *contexts[EVENT_BUS_MAX_SUBSCRIBERS];
    size_t count = 0;

    portENTER_CRITICAL(&s_subscriber_lock);
    for (size_t i = 0; i < s_subscriber_count; i++) {
        if (s_subscribers[i].type == event->type) {
            handlers[count] = s_subscribers[i].handler;
            contexts[count] = s_subscribers[i].user_ctx;
            count++;
        }
    }
    portEXIT_CRITICAL(&s_subscriber_lock);

    /* 在临界区之外调用回调：回调里可以安全地再次订阅/发布事件 */
    for (size_t i = 0; i < count; i++) {
        handlers[i](event, contexts[i]);
    }
}

/**
 * @brief 分发任务主体
 *
 * 永久阻塞等待队列，收到一个事件就分发一个。
 * portMAX_DELAY 表示「没有事件就一直睡」，不占用 CPU——
 * 这是 FreeRTOS 任务的标准写法：阻塞式等待而不是忙等轮询。
 */
static void event_bus_task(void *arg)
{
    (void)arg; /* 未使用参数，显式丢弃以避免编译告警 */

    app_event_t event;

    for (;;) {
        /* 队列为空时任务睡眠，有事件时被内核唤醒 */
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) == pdTRUE) {
            event_bus_dispatch(&event);
        }
    }
}

esp_err_t event_bus_init(void)
{
    /* 防止重复初始化：重复创建队列会造成内存泄漏与状态混乱 */
    if (s_queue != NULL) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_OK;
    }

    /* 创建事件队列：元素大小 = 一个 app_event_t，长度为 EVENT_BUS_QUEUE_LEN */
    s_queue = xQueueCreate(EVENT_BUS_QUEUE_LEN, sizeof(app_event_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "创建事件队列失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    /* 创建分发任务并绑定到 Core 0（交互/通信核），与测量核隔离 */
    BaseType_t ok = xTaskCreatePinnedToCore(event_bus_task, "event_bus", EVENT_BUS_TASK_STACK, NULL,
                                            EVENT_BUS_TASK_PRIO, NULL, 0);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "创建分发任务失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "初始化完成（队列长度 %d，订阅上限 %d）", EVENT_BUS_QUEUE_LEN,
             EVENT_BUS_MAX_SUBSCRIBERS);
    return ESP_OK;
}

esp_err_t event_bus_post(const app_event_t *event, uint32_t timeout_ms)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* xQueueSend 会把整个结构体「按值拷贝」进队列，因此调用方无需保持 event 有效 */
    if (xQueueSend(s_queue, event, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        /* 队列满：记录并上报。丢弃而不阻塞，是为了保护发布方的实时性 */
        s_dropped_count++;
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t event_bus_subscribe(app_event_type_t type, app_event_handler_t handler, void *user_ctx)
{
    if (handler == NULL || (size_t)type >= APP_EVENT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    portENTER_CRITICAL(&s_subscriber_lock);
    if (s_subscriber_count >= EVENT_BUS_MAX_SUBSCRIBERS) {
        err = ESP_ERR_NO_MEM;
    } else {
        s_subscribers[s_subscriber_count].type = type;
        s_subscribers[s_subscriber_count].handler = handler;
        s_subscribers[s_subscriber_count].user_ctx = user_ctx;
        s_subscriber_count++;
    }
    portEXIT_CRITICAL(&s_subscriber_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "订阅失败：订阅表已满（上限 %d）", EVENT_BUS_MAX_SUBSCRIBERS);
    }

    return err;
}
