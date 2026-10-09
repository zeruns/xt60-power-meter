/**
 * @file button_fsm.c
 * @brief 按键状态机实现（纯逻辑，无任何平台依赖）
 *
 * 设计说明见 button_fsm.h 文件头。
 */
#include "button_fsm.h"

#include <stddef.h>

/**
 * @brief 计算两个毫秒时间戳的有符号差值（处理 uint32_t 回绕）
 * @return a - b，结果范围视为 ±2^31ms
 */
static int32_t elapsed_ms(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

void button_fsm_init(button_fsm_t *fsm)
{
    if (fsm == NULL) {
        return;
    }

    fsm->debounce_ms = BUTTON_FSM_DEBOUNCE_MS;
    fsm->double_gap_ms = BUTTON_FSM_DOUBLE_GAP_MS;
    fsm->long_press_ms = BUTTON_FSM_LONG_MS;

    fsm->state = BFS_RELEASED;
    fsm->raw_level = false;
    fsm->stable_level = false;
    fsm->raw_initialized = false;
    fsm->raw_change_ms = 0;
    fsm->press_start_ms = 0;
    fsm->release_ms = 0;
    fsm->click_count = 0;
}

/**
 * @brief 对原始电平做时间消抖，返回消抖后的稳定电平是否发生变化
 *
 * 规则：原始电平翻转后必须保持 debounce_ms 不变，才采纳为新的稳定电平。
 *
 * @return true 表示本次样本让稳定电平发生了翻转（新电平 = fsm->stable_level）
 */
static bool debounce_feed(button_fsm_t *fsm, bool pressed, uint32_t now_ms)
{
    if (!fsm->raw_initialized) {
        fsm->raw_initialized = true;
        fsm->raw_level = pressed;
        fsm->raw_change_ms = now_ms;
        /* 首次样本直接作为初始稳定电平，不产生翻转事件 */
        fsm->stable_level = pressed;
        return false;
    }

    if (pressed != fsm->raw_level) {
        fsm->raw_level = pressed;
        fsm->raw_change_ms = now_ms;
    }

    if (pressed != fsm->stable_level &&
        elapsed_ms(now_ms, fsm->raw_change_ms) >= (int32_t)fsm->debounce_ms) {
        fsm->stable_level = pressed;
        return true;
    }

    return false;
}

button_fsm_event_t button_fsm_update(button_fsm_t *fsm, bool pressed, uint32_t now_ms)
{
    if (fsm == NULL) {
        return BUTTON_FSM_NONE;
    }

    bool edge = debounce_feed(fsm, pressed, now_ms);
    bool level = fsm->stable_level;

    switch (fsm->state) {
    case BFS_RELEASED:
        if (edge && level) {
            /* 确认按下沿 */
            fsm->press_start_ms = now_ms;
            fsm->state = BFS_PRESSED;
        }
        return BUTTON_FSM_NONE;

    case BFS_PRESSED: {
        if (!level) {
            /* 释放沿（未触发长按）→ 记一次 click，进入双击等待 */
            fsm->click_count++;
            fsm->release_ms = now_ms;
            fsm->state = BFS_WAIT_DBL;
            return BUTTON_FSM_NONE;
        }
        /* 按住中：到长按阈值 */
        if (elapsed_ms(now_ms, fsm->press_start_ms) >= (int32_t)fsm->long_press_ms) {
            fsm->state = BFS_LONG;
            return BUTTON_FSM_LONG_PRESS_START;
        }
        return BUTTON_FSM_NONE;
    }

    case BFS_LONG:
        if (!level) {
            /* 长按释放：click 计数作废，直接回空闲 */
            fsm->click_count = 0;
            fsm->state = BFS_RELEASED;
            return BUTTON_FSM_LONG_PRESS_RELEASE;
        }
        return BUTTON_FSM_NONE;

    case BFS_WAIT_DBL:
        if (edge && level) {
            /* 双击窗口内再次按下（可能成为双击的第二击） */
            fsm->press_start_ms = now_ms;
            fsm->state = BFS_PRESSED;
            return BUTTON_FSM_NONE;
        }
        if (!level && elapsed_ms(now_ms, fsm->release_ms) >= (int32_t)fsm->double_gap_ms) {
            /* 双击窗口超时：按已记录的 click 次数出事件 */
            button_fsm_event_t ev =
                (fsm->click_count >= 2) ? BUTTON_FSM_DOUBLE_CLICK : BUTTON_FSM_SINGLE_CLICK;
            fsm->click_count = 0;
            fsm->state = BFS_RELEASED;
            return ev;
        }
        return BUTTON_FSM_NONE;
    }

    return BUTTON_FSM_NONE;
}
