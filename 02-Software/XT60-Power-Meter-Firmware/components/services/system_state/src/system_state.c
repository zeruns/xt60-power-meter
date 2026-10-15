/**
 * @file system_state.c
 * @brief 系统运行状态机实现
 *
 * 核心思路：
 *   外部只调用 report_fault / clear_fault 来"上报症状"，
 *   状态由 s_fault_mask 自动推导，避免各模块自行决定状态导致不一致。
 *
 * 为什么状态要"由故障位图推导"而不是"由各模块直接设置"？
 *   假如 LCD 驱动失败时自己调用"进入 DEGRADED"，WiFi 失败时也自己决定状态，
 *   两个模块同时出故障时该听谁的？判断规则会散落各处、彼此打架。
 *   这里把规则收敛到 derive_state() 一个函数：
 *   有致命故障位（ADC/INFRA）→ FAULT；有非致命故障位 → DEGRADED；否则 RUNNING。
 *   各模块只需"记一笔故障"，最终状态永远一致、可预测。
 *
 * 每次状态真正发生变化时，向 event_bus 广播一个
 * APP_EVENT_SYSTEM_STATE_CHANGED 事件，由订阅者（device_ctrl 等）响应。
 * 状态机本身不碰指示灯/蜂鸣器等硬件——那是订阅者的事（分层设计，
 * 详见 system_state.h 文件头）。
 *
 * INIT 期间的特殊处理：
 *   开机时各模块陆续初始化，此间上报的故障不应立刻把状态打成 FAULT，
 *   否则指示灯会先闪一下 FAULT 再回正常。所以启动完成前状态固定为 INIT，
 *   system_state_start_running() 时才一次性推导出真实状态。
 */
#include "system_state.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "event_bus.h"

#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "sys_state";

/** 状态总数，用于参数合法性检查 */
#define STATE_COUNT 4

/** 状态名称表，下标即 system_state_t */
static const char *const s_state_names[STATE_COUNT] = {
    [SYSTEM_STATE_INIT] = "INIT",
    [SYSTEM_STATE_RUNNING] = "RUNNING",
    [SYSTEM_STATE_DEGRADED] = "DEGRADED",
    [SYSTEM_STATE_FAULT] = "FAULT",
};

/** 故障位 → 可读名称表，用于把位图转成日志文本 */
typedef struct {
    uint32_t bit;     /**< 故障位 */
    const char *name; /**< 名称 */
} fault_name_t;

static const fault_name_t s_fault_names[] = {
    { SYSTEM_FAULT_ADC, "ADC" },       { SYSTEM_FAULT_LCD, "LCD" },
    { SYSTEM_FAULT_TOUCH, "TOUCH" },   { SYSTEM_FAULT_IMU, "IMU" },
    { SYSTEM_FAULT_ENV, "ENV" },       { SYSTEM_FAULT_BATTERY, "BATTERY" },
    { SYSTEM_FAULT_WIFI, "WIFI" },     { SYSTEM_FAULT_STORAGE, "STORAGE" },
    { SYSTEM_FAULT_BUTTON, "BUTTON" }, { SYSTEM_FAULT_BUZZER, "BUZZER" },
    { SYSTEM_FAULT_LED, "LED" },       { SYSTEM_FAULT_INFRA, "INFRA" },
};

/** 全部已定义故障位的并集，用于校验入参合法性 */
#define ALL_FAULT_BITS                                                                             \
    ((uint32_t)(SYSTEM_FAULT_ADC | SYSTEM_FAULT_LCD | SYSTEM_FAULT_TOUCH | SYSTEM_FAULT_IMU |      \
                SYSTEM_FAULT_ENV | SYSTEM_FAULT_BATTERY | SYSTEM_FAULT_WIFI |                      \
                SYSTEM_FAULT_STORAGE | SYSTEM_FAULT_BUTTON | SYSTEM_FAULT_BUZZER |                 \
                SYSTEM_FAULT_LED | SYSTEM_FAULT_INFRA))

/* ---------- 模块内部状态 ---------- */

/** 当前状态（volatile：可能被多个任务读取） */
static volatile system_state_t s_state = SYSTEM_STATE_INIT;

/** 当前故障位图 */
static volatile uint32_t s_fault_mask = SYSTEM_FAULT_NONE;

/** 是否已完成初始化 */
static bool s_ready = false;

/** 启动流程是否已完成（完成前状态固定为 INIT） */
static bool s_boot_done = false;

/**
 * @brief 获取状态的可读名称（用于日志）
 *
 * @param state 状态值
 * @return 常量字符串；非法值返回 "UNKNOWN"
 */
const char *system_state_name(system_state_t state)
{
    /* 越界返回固定字符串，避免调用方拿到野指针 */
    if ((size_t)state >= STATE_COUNT) {
        return "UNKNOWN";
    }
    return s_state_names[state];
}

/**
 * @brief 把故障位图转换为可读字符串
 *
 * 输出形如 "ADC|LCD|WIFI"，无故障时输出 "无"。
 * 提供调用方缓冲区而不是返回静态变量，是为了多任务安全
 * （静态缓冲会被并发调用互相覆盖）。
 *
 * @param fault_mask 故障位图
 * @param buf      输出缓冲区
 * @param buf_len  缓冲区大小（包含 '\0'）
 */
void system_state_fault_to_str(uint32_t fault_mask, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return;
    }

    if (fault_mask == SYSTEM_FAULT_NONE) {
        /* snprintf 会自动补 '\0'，并保证不会写出 buf_len 之外 */
        snprintf(buf, buf_len, "无");
        return;
    }

    buf[0] = '\0';
    size_t used = 0;

    /* 遍历故障名表，把命中的位逐个拼进缓冲区 */
    for (size_t i = 0; i < sizeof(s_fault_names) / sizeof(s_fault_names[0]); i++) {
        if ((fault_mask & s_fault_names[i].bit) == 0) {
            continue;
        }

        /* snprintf(目标, 剩余空间, 格式, ...)：安全版 sprintf，返回"本想写入的字符数"。
         * 首个名字前不加分隔符，其余用 '|' 连接 */
        int written = snprintf(buf + used, buf_len - used, "%s%s", (used == 0) ? "" : "|",
                               s_fault_names[i].name);
        if (written < 0 || (size_t)written >= buf_len - used) {
            break; /* 缓冲区放不下了，停止追加（剩下的位不再显示） */
        }
        used += (size_t)written;
    }
}

/**
 * @brief 获取当前状态
 *
 * @return 当前系统状态（启动完成前恒为 SYSTEM_STATE_INIT）
 */
system_state_t system_state_get(void)
{
    return s_state;
}

/**
 * @brief 获取当前故障位图
 *
 * @return 各位或的结果，可用 `mask & SYSTEM_FAULT_XXX` 判断某个模块是否故障
 */
uint32_t system_state_get_fault_mask(void)
{
    return s_fault_mask;
}

/**
 * @brief 由故障位图推导出应当处于的状态
 *
 * 这是状态机的"规则中心"，读代码时只需看这一个函数就能明白
 * 什么情况算什么状态。
 *
 * @param fault_mask 当前故障位图
 * @return 应当进入的状态
 *
 * 为什么判断顺序是"先致命、后非致命"？
 *   故障位可以同时存在多个，必须先判最严重的：
 *   只要命中 SYSTEM_FAULT_CRITICAL_MASK 中任意一位（ADC/INFRA），
 *   无论还有多少非致命故障，都直接 FAULT。
 */
static system_state_t derive_state(uint32_t fault_mask)
{
    if ((fault_mask & SYSTEM_FAULT_CRITICAL_MASK) != 0) {
        return SYSTEM_STATE_FAULT; /* 关键模块坏了 */
    }
    if (fault_mask != SYSTEM_FAULT_NONE) {
        return SYSTEM_STATE_DEGRADED; /* 有非关键故障 → 降级 */
    }
    return SYSTEM_STATE_RUNNING; /* 一切正常 */
}

/**
 * @brief 向事件总线广播当前状态
 *
 * 用 timeout = 0 投递：状态机不应因为事件队列拥塞而被阻塞。
 * 投递失败只记录告警，不向上报错——状态本身已经更新，
 * 丢一个通知最多导致指示灯晚一拍变化，不影响系统正确性。
 */
static void broadcast_state(void)
{
    /* 组一个状态变化事件：把当前状态与故障位图一起带给订阅者，
     * 订阅者（如 device_ctrl）据此决定指示灯怎么闪 */
    app_event_t event = { 0 };
    event.type = APP_EVENT_SYSTEM_STATE_CHANGED;
    event.data.sys_state.state = (uint8_t)s_state;
    event.data.sys_state.fault_mask = s_fault_mask;

    /* event_bus_post(&事件, 超时毫秒)：把事件按值投进总线的 FreeRTOS 队列，
     * 由分发任务异步调用各订阅者回调。timeout=0 表示"队列满就立刻放弃" */
    esp_err_t err = event_bus_post(&event, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "状态事件投递失败: %s（指示灯将保持上一状态）", esp_err_to_name(err));
    }
}

/**
 * @brief 重新计算并应用状态；若状态发生变化则广播事件
 *
 * 所有"改故障位图"的入口最终都汇聚到这里，保证
 * "算状态 → 记日志 → 广播事件"这套动作只有一份实现。
 *
 * @return true 表示状态发生了变化
 */
static bool refresh_state(void)
{
    /* 启动流程未完成前，状态固定为 INIT，不随故障变化 */
    system_state_t target = s_boot_done ? derive_state(s_fault_mask) : SYSTEM_STATE_INIT;

    /* 状态没变就什么都不做：避免每次故障上报都重复广播同一种状态 */
    if (target == s_state) {
        return false;
    }

    /* 先存旧状态用于日志对比，再更新——只在"真正变化"时才打印和广播 */
    system_state_t previous = s_state;
    s_state = target;

    char fault_text[96];
    system_state_fault_to_str(s_fault_mask, fault_text, sizeof(fault_text));

    ESP_LOGW(TAG, "状态迁移: %s -> %s（故障: %s）", system_state_name(previous),
             system_state_name(target), fault_text);

    broadcast_state();
    return true;
}

/**
 * @brief 初始化状态机，进入 INIT 状态
 *
 * 只初始化内部变量并广播一次初始状态，不直接操作任何硬件。
 * 需要 event_bus 已初始化，否则状态事件发不出去（会记录告警但不报错）。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 重复初始化
 */
esp_err_t system_state_init(void)
{
    if (s_ready) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    /* 全部内部状态复位：故障位图清零，启动流程标记为未完成 */
    s_state = SYSTEM_STATE_INIT;
    s_fault_mask = SYSTEM_FAULT_NONE;
    s_boot_done = false;
    s_ready = true;

    ESP_LOGI(TAG, "初始化完成，当前状态: %s", system_state_name(s_state));

    /* 广播一次初始状态，让早于状态机启动的订阅者也能同步上 */
    broadcast_state();

    return ESP_OK;
}

/**
 * @brief 宣告"启动流程已完成"，把状态从 INIT 推进到推导出的状态
 *
 * 若启动过程中已上报过故障，这里会直接落到 DEGRADED 或 FAULT；
 * 一切正常则进入 RUNNING。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t system_state_start_running(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 打开"按故障推导状态"的开关，refresh_state() 会立刻推导一次：
     * 若此时已有故障位，直接落到 DEGRADED/FAULT；否则进入 RUNNING */
    s_boot_done = true;
    refresh_state();

    char fault_text[96];
    system_state_fault_to_str(s_fault_mask, fault_text, sizeof(fault_text));
    ESP_LOGI(TAG, "启动流程结束，进入状态 %s（故障: %s）", system_state_name(s_state), fault_text);

    return ESP_OK;
}

/**
 * @brief 上报故障（累加位图并自动切换状态）
 *
 * 典型用法：某模块初始化失败时调用 system_state_report_fault(SYSTEM_FAULT_LCD)。
 * 它只负责"记一笔"，该进入 DEGRADED 还是 FAULT 由状态机自己判断。
 *
 * @param fault_mask 故障位（可多个位或）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG fault_mask 为 0 或含未定义的位
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t system_state_report_fault(uint32_t fault_mask)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 必须是非零且只包含已定义的位，防止误传数值污染状态判断 */
    if (fault_mask == SYSTEM_FAULT_NONE || (fault_mask & ~ALL_FAULT_BITS) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 按位或"累加"故障：多个模块可以同时坏，各自的位互不覆盖 */
    s_fault_mask |= fault_mask;
    refresh_state();
    return ESP_OK;
}

/**
 * @brief 清除故障（用于故障恢复后把状态降回来）
 *
 * 例：WiFi 断线重连成功后调用 system_state_clear_fault(SYSTEM_FAULT_WIFI)。
 *
 * @param fault_mask 要清除的故障位
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG fault_mask 为 0
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t system_state_clear_fault(uint32_t fault_mask)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fault_mask == SYSTEM_FAULT_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 按位与非"抹掉"指定的位：只清除恢复了的模块，不影响其他故障位 */
    s_fault_mask &= ~fault_mask;
    refresh_state();
    return ESP_OK;
}
