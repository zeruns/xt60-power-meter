/**
 * @file button_fsm.h
 * @brief 按键状态机（纯逻辑，不依赖任何硬件/OS，可在 PC 上单测）
 *
 * 把"原始电平序列 → 语义事件"的判定逻辑从 ESP-IDF 驱动里剥离出来：
 *   button.c 只负责每 10ms 采样 GPIO 并调用 button_fsm_update()；
 *   消抖、单击、双击、长按的时序判定全部在本文件，且可用 gcc 在主机上
 *   直接编译测试（见 test/host/test_button_fsm.c）。
 *
 * 状态模型：
 *
 *   RELEASED ──按下稳定──> PRESSED ──释放(未达长按)──> WAIT_DBL
 *      ▲                     │                          │
 *      │                     └──长按阈值──> LONG_PRESSED│
 *      │                                      │        │ 双击窗口内
 *      └────────────释放/窗口超时出事件────────┴────────┘
 *
 * 事件语义：
 *   · SINGLE_CLICK：释放后经过 DOUBLE_GAP_MS 没有第二次按下才确认（有意延迟），
 *     这是识别双击的必要代价（手机同样如此）；
 *   · DOUBLE_CLICK：第二次释放后的双击窗口超时后确认；
 *   · LONG_PRESS_START：按下保持达到 LONG_PRESS_MS（不重复）；
 *   · LONG_PRESS_RELEASE：长按之后释放。
 *   长按期间不产生 click 事件；三连击按双击处理（不做更高阶）。
 *
 * 时间基准：调用方传入单调毫秒时间戳（uint32_t，允许 49.7 天回绕，
 * 内部一律用 (int32_t)(a-b) 的有符号差值判定先后）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 状态机输出事件（一次 update 至多一个事件） */
typedef enum {
    BUTTON_FSM_NONE = 0,            /**< 无事件 */
    BUTTON_FSM_SINGLE_CLICK,       /**< 单击（双击窗口超时后确认） */
    BUTTON_FSM_DOUBLE_CLICK,       /**< 双击 */
    BUTTON_FSM_LONG_PRESS_START,   /**< 长按达到阈值（仅一次） */
    BUTTON_FSM_LONG_PRESS_RELEASE, /**< 长按后释放 */
} button_fsm_event_t;

/** 默认时序参数（毫秒） */
#define BUTTON_FSM_DEBOUNCE_MS   20  /**< 消抖：连续稳定 20ms 才认 */
#define BUTTON_FSM_DOUBLE_GAP_MS 300 /**< 双击窗口：两次按下间隔 ≤ 300ms 为双击 */
#define BUTTON_FSM_LONG_MS       800 /**< 长按阈值：按下保持 ≥ 800ms */

/** 状态机内部状态（字段对外可见仅为了静态分配，不要直接读写） */
typedef enum {
    BFS_RELEASED = 0, /**< 未按下 */
    BFS_PRESSED,      /**< 已确认按下（未到长按） */
    BFS_LONG,         /**< 长按已触发，仍按住 */
    BFS_WAIT_DBL,     /**< 已释放，等待可能的第二次按下 */
} button_fsm_state_t;

/** 单个按键的状态机 */
typedef struct {
    /* ---- 配置（0 时在 init 中填默认值） ---- */
    uint16_t debounce_ms;    /**< 消抖时间 */
    uint16_t double_gap_ms;  /**< 双击窗口 */
    uint16_t long_press_ms;  /**< 长按阈值 */

    /* ---- 内部状态 ---- */
    button_fsm_state_t state;
    bool raw_level;          /**< 最近一次原始电平（true=按下） */
    bool stable_level;       /**< 消抖后的稳定电平 */
    bool raw_initialized;    /**< 是否已记录首个原始电平 */
    uint32_t raw_change_ms;  /**< 原始电平最近一次翻转时刻 */
    uint32_t press_start_ms; /**< 本次按下起始时刻 */
    uint32_t release_ms;     /**< 最近一次释放时刻（WAIT_DBL 用） */
    uint8_t click_count;     /**< 双击窗口内已完成的 click 次数 */
} button_fsm_t;

/**
 * @brief 初始化状态机（全部字段归零后填入默认时序参数）
 * @param fsm 状态机，不可为 NULL
 */
void button_fsm_init(button_fsm_t *fsm);

/**
 * @brief 喂入一个原始电平样本，返回本次样本产生的事件
 *
 * 应由驱动以固定周期（本工程 10ms）调用；传入的时间戳必须单调递增，
 * 允许 uint32_t 回绕。
 *
 * @param fsm      状态机，不可为 NULL
 * @param pressed  当前原始电平：true=按下（低有效按键的取反由调用方完成）
 * @param now_ms   当前毫秒时间戳
 * @return 本次产生的事件；无事件时为 BUTTON_FSM_NONE
 */
button_fsm_event_t button_fsm_update(button_fsm_t *fsm, bool pressed, uint32_t now_ms);
