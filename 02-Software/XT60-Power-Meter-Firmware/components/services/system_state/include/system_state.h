/**
 * @file system_state.h
 * @brief 系统运行状态机：用一个统一的状态描述"设备现在健不健康"
 *
 * ─────────────────────────────────────────────────────────────
 * 为什么需要状态机？
 * ─────────────────────────────────────────────────────────────
 * 测量仪器上有很多"输出设备"要反映系统健康状况：
 *   指示灯、蜂鸣器、屏幕上的提示条、网页控制台的状态页……
 * 如果让每个输出设备各自去判断"ADC 好不好、屏幕好不好"，
 * 判断逻辑会散落各处、彼此不一致，新增一种故障就要改很多地方。
 *
 * 状态机把这些信息收敛成一个变量：当前状态 + 故障位图。
 * 输出设备只需订阅「状态变化事件」即可，逻辑集中且一致。
 *
 * ─────────────────────────────────────────────────────────────
 * 四个状态
 * ─────────────────────────────────────────────────────────────
 *   INIT     ── 正在初始化（指示灯常亮）
 *   RUNNING  ── 一切正常，全部功能可用（指示灯慢闪 1Hz）
 *   DEGRADED ── 降级运行：部分非关键功能失效，但核心测量仍可用
 *               （指示灯双闪；例如屏幕坏了但测量正常）
 *   FAULT    ── 异常：关键功能失效，设备不可用（指示灯快闪 5Hz）
 *
 * 状态不是随便设置的，而是由「故障位图」自动推导：
 *   有致命故障位        → FAULT
 *   有非致命故障位      → DEGRADED
 *   没有任何故障位      → RUNNING
 * 这样各模块只需"上报自己坏了"，不必关心最终该是什么状态。
 *
 * ─────────────────────────────────────────────────────────────
 * 分层说明（本模块的重要设计变更）
 * ─────────────────────────────────────────────────────────────
 * 早期版本里，状态机直接调用 status_led_set_pattern() 去改灯。
 * 那违反了「业务层不应依赖驱动层」的分层规则。
 * 现在改为：状态机只通过 event_bus 广播「我的状态变了」，
 * 由业务层模块 device_ctrl 订阅并负责驱动指示灯。
 * 好处：以后要让"故障时蜂鸣器长鸣"，只需再加一个订阅者，
 *       状态机代码一行都不用改。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 系统运行状态
 *
 * 注意状态之间的正常流转：
 *   INIT → RUNNING / DEGRADED / FAULT
 *   RUNNING ⇄ DEGRADED ⇄ FAULT（随故障上报与恢复而自动变化）
 */
typedef enum {
    SYSTEM_STATE_INIT = 0, /**< 初始化中，尚未开始提供服务 */
    SYSTEM_STATE_RUNNING,  /**< 正常运行，全部功能可用 */
    SYSTEM_STATE_DEGRADED, /**< 降级运行，部分非关键功能失效 */
    SYSTEM_STATE_FAULT,    /**< 异常，关键功能失效 */
} system_state_t;

/**
 * @brief 故障位定义（可同时存在多个，按位或组合）
 *
 * 用位图而不是枚举，是为了表达"同时坏了两个模块"这种情况。
 * 每新增一个可失败的模块，就在这里加一位。
 */
typedef enum {
    SYSTEM_FAULT_NONE = 0,          /**< 无故障 */
    SYSTEM_FAULT_ADC = 1u << 0,     /**< ADS131M02 测量前端（致命：没它就没法测量） */
    SYSTEM_FAULT_LCD = 1u << 1,     /**< LCD 显示 */
    SYSTEM_FAULT_TOUCH = 1u << 2,   /**< 电容触摸 */
    SYSTEM_FAULT_IMU = 1u << 3,     /**< 加速度计（自动转屏功能） */
    SYSTEM_FAULT_ENV = 1u << 4,     /**< 温湿度传感器 */
    SYSTEM_FAULT_BATTERY = 1u << 5, /**< 电池电压采样 */
    SYSTEM_FAULT_WIFI = 1u << 6,    /**< WiFi / 网页控制台 */
    SYSTEM_FAULT_STORAGE = 1u << 7, /**< 配置存储（NVS） */
    SYSTEM_FAULT_BUTTON = 1u << 8,  /**< 按键 */
    SYSTEM_FAULT_BUZZER = 1u << 9,  /**< 蜂鸣器 */
    SYSTEM_FAULT_LED = 1u << 10,    /**< 状态指示灯（坏了只是看不到指示） */
    SYSTEM_FAULT_INFRA = 1u << 11,  /**< 软件基础设施（事件总线/状态机等） */
} system_fault_t;

/**
 * 致命故障掩码：只要命中其中任意一位，系统就直接进入 FAULT。
 *
 * 判断标准是「这个模块没了，设备还算不算一台能用的测量仪器」：
 *   · ADC     —— 采样前端坏掉，功率计完全失去意义；
 *   · INFRA   —— 事件总线/状态机等基础设施坏了，整个软件框架失效。
 * 屏幕、WiFi、传感器、指示灯坏掉都属于降级，设备仍能正常测量。
 *
 * 各模块的"重要程度"就是用它自己的故障位与这个掩码做按位与来判断的，
 * 因此初始化表里不需要额外写一个"是否关键"的布尔字段——
 * 重要程度的信息只存在一处，不会出现"表里说关键、掩码说非关键"的矛盾。
 */
#define SYSTEM_FAULT_CRITICAL_MASK ((uint32_t)(SYSTEM_FAULT_ADC | SYSTEM_FAULT_INFRA))

/**
 * @brief 初始化状态机，进入 INIT 状态
 *
 * 只初始化内部变量并广播一次初始状态，不直接操作任何硬件。
 * 需要 event_bus 已初始化，否则状态事件发不出去（会记录告警但不报错）。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 重复初始化
 */
esp_err_t system_state_init(void);

/**
 * @brief 宣告"启动流程已完成"，把状态从 INIT 推进到推导出的状态
 *
 * 若启动过程中已上报过故障，这里会直接落到 DEGRADED 或 FAULT；
 * 一切正常则进入 RUNNING。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t system_state_start_running(void);

/**
 * @brief 获取当前状态
 */
system_state_t system_state_get(void);

/**
 * @brief 获取当前故障位图
 *
 * @return 各位或的结果，可用 `mask & SYSTEM_FAULT_XXX` 判断某个模块是否故障
 */
uint32_t system_state_get_fault_mask(void);

/**
 * @brief 上报故障（累加位图并自动切换状态）
 *
 * 典型用法：某模块初始化失败时调用
 *     system_state_report_fault(SYSTEM_FAULT_LCD);
 * 它只负责"记一笔"，该进入 DEGRADED 还是 FAULT 由状态机自己判断。
 *
 * @param fault_mask 故障位（可多个位或）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG fault_mask 为 0 或含未定义的位
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t system_state_report_fault(uint32_t fault_mask);

/**
 * @brief 清除故障（用于故障恢复后把状态降回来）
 *
 * 例：WiFi 断线重连成功后调用 system_state_clear_fault(SYSTEM_FAULT_WIFI);
 *
 * @param fault_mask 要清除的故障位
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG fault_mask 为 0
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t system_state_clear_fault(uint32_t fault_mask);

/**
 * @brief 获取状态的可读名称（用于日志）
 *
 * @param state 状态值
 * @return 常量字符串；非法值返回 "UNKNOWN"
 */
const char *system_state_name(system_state_t state);

/**
 * @brief 把故障位图转成可读文本，写入调用方提供的缓冲区
 *
 * 提供缓冲区而不是返回静态变量，是为了保证线程安全
 * （静态缓冲在多任务下会被互相覆盖）。
 *
 * @param fault_mask 故障位图
 * @param buf        输出缓冲区
 * @param buf_len    缓冲区长度（建议至少 96 字节，能放下全部故障名）
 */
void system_state_fault_to_str(uint32_t fault_mask, char *buf, size_t buf_len);
