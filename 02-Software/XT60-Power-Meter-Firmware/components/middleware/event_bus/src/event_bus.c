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
 *
 * 4. 队列满时「丢弃并计数」而不是让发布者无限等待：发布方往往是
 *    状态机、按键扫描这类实时路径，被慢订阅者拖住会拖垮整个系统。
 *    被丢弃的事件数量记在 s_dropped_count 里，供调试时观察
 *    （event_bus_dropped_count() 可查）。
 */
#include "event_bus.h"

#include "esp_log.h"           /* ESP-IDF 日志库：ESP_LOGx 宏按级别打印带标签的日志 */
#include "freertos/FreeRTOS.h" /* FreeRTOS 核心头文件（portMUX_TYPE、pdMS_TO_TICKS 等） */
#include "freertos/queue.h"    /* FreeRTOS 队列：线程安全的 FIFO 消息容器 */
#include "freertos/task.h"     /* FreeRTOS 任务：xTaskCreatePinnedToCore 等 */

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

/** 当前已注册的订阅者数量（同时是订阅表的"下一个空位"下标） */
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

/** 事件队列句柄（event_bus_init 成功后才有效） */
static QueueHandle_t s_queue = NULL;

/** 因队列满而丢弃的事件计数（诊断用，只增不减） */
static volatile uint32_t s_dropped_count = 0;

/**
 * @brief 查询因队列满而被丢弃的事件总数
 *
 * @return 累计丢弃的事件数；持续增长说明有订阅者处理太慢或队列太小
 */
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
 *
 * @param event 待分发的事件（只需在本函数执行期间有效）
 */
static void event_bus_dispatch(const app_event_t *event)
{
    /* 局部快照数组：临界区只做"筛选 + 复制"，真正的回调在锁外执行 */
    app_event_handler_t handlers[EVENT_BUS_MAX_SUBSCRIBERS];
    void *contexts[EVENT_BUS_MAX_SUBSCRIBERS];
    size_t count = 0;

    /* portENTER_CRITICAL/EXIT：进/出临界区（短暂关中断），保护订阅表
     * 不被并发的 subscribe 与本次遍历同时改读 */
    portENTER_CRITICAL(&s_subscriber_lock);
    for (size_t i = 0; i < s_subscriber_count; i++) {
        /* 只挑出"订阅了本事件类型"的订阅者 */
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
 *
 * @param arg xTaskCreate 传入的任务参数（本模块未使用）
 */
static void event_bus_task(void *arg)
{
    (void)arg; /* 未使用参数，显式丢弃以避免编译告警 */

    app_event_t event;

    for (;;) {
        /* xQueueReceive(队列, &接收缓冲, 最大等待)：从队列头取出一个元素
         * （按值拷贝进 event）。队列为空时任务睡眠，有事件时被内核唤醒；
         * portMAX_DELAY 表示无限等待，直到收到事件 */
        if (xQueueReceive(s_queue, &event, portMAX_DELAY) == pdTRUE) {
            event_bus_dispatch(&event);
        }
    }
}

/**
 * @brief 初始化事件总线（创建队列与分发任务）
 *
 * @return ESP_OK 成功
 *         ESP_ERR_NO_MEM 队列或任务创建失败（内存不足）
 */
esp_err_t event_bus_init(void)
{
    /* 防止重复初始化：重复创建队列会造成内存泄漏与状态混乱 */
    if (s_queue != NULL) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_OK;
    }

    /* xQueueCreate(队列长度, 单个元素字节数)：创建一个 FreeRTOS 队列，
     * 内部线程安全，发布者与分发任务之间不需要额外加锁 */
    s_queue = xQueueCreate(EVENT_BUS_QUEUE_LEN, sizeof(app_event_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "创建事件队列失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    /* xTaskCreatePinnedToCore(任务函数, 任务名, 栈字节数, 参数, 优先级, 句柄, 核心号)：
     * 创建 FreeRTOS 任务并绑定到指定 CPU 核。这里绑到 Core 0（交互/通信核），
     * 与测量核（Core 1）隔离，避免事件处理干扰采样时序 */
    BaseType_t ok = xTaskCreatePinnedToCore(event_bus_task, "event_bus", EVENT_BUS_TASK_STACK, NULL,
                                            EVENT_BUS_TASK_PRIO, NULL, 0);
    if (ok != pdPASS) {
        /* 任务没建起来，把队列删掉回滚，避免留下半初始化状态 */
        vQueueDelete(s_queue);
        s_queue = NULL;
        ESP_LOGE(TAG, "创建分发任务失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "初始化完成（队列长度 %d，订阅上限 %d）", EVENT_BUS_QUEUE_LEN,
             EVENT_BUS_MAX_SUBSCRIBERS);
    return ESP_OK;
}

/**
 * @brief 发布一个事件（把消息投递到总线）
 *
 * 本函数不会等待订阅者处理完，投递成功即返回（异步解耦，见文件头要点 1）。
 *
 * @param event      事件内容，不可为 NULL（内部会按值做副本）
 * @param timeout_ms 队列满时的等待时间（毫秒）。传 0 表示"不等待，队列满就放弃"，
 *                   定时器/回调等实时路径里应传 0
 * @return ESP_OK 投递成功
 *         ESP_ERR_INVALID_ARG event 为 NULL
 *         ESP_ERR_INVALID_STATE 总线尚未初始化
 *         ESP_ERR_TIMEOUT 队列已满且等待超时（事件被丢弃并计数）
 */
esp_err_t event_bus_post(const app_event_t *event, uint32_t timeout_ms)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* xQueueSend(队列, &数据, 最大等待)：把整个结构体「按值拷贝」进队列，
     * 因此调用方无需在函数返回后保持 event 有效（没有悬空指针问题）。
     * pdMS_TO_TICKS()：把毫秒换算成 FreeRTOS 的时钟节拍（tick）数 */
    if (xQueueSend(s_queue, event, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        /* 队列满：记录并上报。丢弃而不阻塞，是为了保护发布方的实时性
         * （见文件头要点 4）；丢失情况可用 event_bus_dropped_count() 观察 */
        s_dropped_count++;
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/**
 * @brief 订阅某类事件
 *
 * 订阅应在系统启动阶段完成（各模块的 xxx_init 里），
 * 这样进入正常运行后就不会漏掉事件。
 *
 * 注意：没有"取消订阅"接口，订阅者一旦注册就永久有效，
 * 因此回调函数与 user_ctx 指向的对象必须在整个运行期都有效。
 *
 * @param type     关心的事件类型
 * @param handler  回调函数，不可为 NULL
 * @param user_ctx 用户上下文，回调时原样传回
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 参数非法
 *         ESP_ERR_NO_MEM 订阅表已满（上限 EVENT_BUS_MAX_SUBSCRIBERS）
 */
esp_err_t event_bus_subscribe(app_event_type_t type, app_event_handler_t handler, void *user_ctx)
{
    /* 参数校验：回调必须非空；(size_t)type >= APP_EVENT_MAX 用来拦住越界的枚举值
     * （APP_EVENT_MAX 是"类型总数"哨兵，见 event_bus.h） */
    if (handler == NULL || (size_t)type >= APP_EVENT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    /* 进临界区追加表项：与分发任务的遍历互斥，防止遍历到"写了一半"的条目 */
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
