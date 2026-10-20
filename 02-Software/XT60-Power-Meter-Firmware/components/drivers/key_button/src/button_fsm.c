/**
 * @file button_fsm.c
 * @brief 按键状态机实现（纯逻辑，无任何平台依赖）
 *
 * 设计说明见 button_fsm.h 文件头。
 *
 * 本文件为什么"纯逻辑"：不 include 任何 ESP-IDF / FreeRTOS 头文件，时间由
 * 调用方以毫秒数传入，电平由调用方采样后传入。好处是这段消抖/单击/双击/长按
 * 判定可以直接用 PC 上的 gcc 编译做单元测试（test/host/test_button_fsm.c），
 * 也可以在换 MCU / 换 RTOS 时原样复用，不用改一行。
 *
 * 核心思路（三层）：
 *   1. debounce_feed()：原始电平 → 稳定电平（时间消抖）；
 *   2. button_fsm_update()：稳定电平沿/电平 → 语义事件（状态机迁移）；
 *   3. 全部时间比较用 elapsed_ms() 的有符号差值，天然兼容 uint32_t 回绕。
 */
#include "button_fsm.h"

#include <stddef.h>

/**
 * @brief 计算两个毫秒时间戳的有符号差值（处理 uint32_t 回绕）
 * @param a 较晚的时间戳
 * @param b 较早的时间戳
 * @return a - b，结果范围视为 ±2^31ms
 * @note 直接写 a-b 会在计时器回绕（约 49.7 天）时得到巨大正数，转成
 *       int32_t 后回绕前后的差值依然正确，这是嵌入式里处理时间回绕的惯用手法。
 */
static int32_t elapsed_ms(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

/**
 * @brief 初始化状态机（全部字段归零后填入默认时序参数）
 *
 * 配置字段（消抖 20ms / 双击窗口 300ms / 长按 800ms）填头文件里的默认宏；
 * 状态回到 BFS_RELEASED，消抖器标记为"未见过样本"，等第一次 update 初始化。
 *
 * @param fsm 状态机，不可为 NULL（传 NULL 时静默返回，不产生副作用）
 */
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
 * 这是"时间消抖"：机械触点闭合瞬间会抖动几毫秒、电平快速通断，只有持续
 * 稳定 20ms 以上的电平才被认为是人的意图，毛刺全部被挡住。
 *
 * @param fsm     状态机
 * @param pressed 本次采样到的原始电平（true=按下）
 * @param now_ms  当前毫秒时间戳
 * @return true 表示本次样本让稳定电平发生了翻转（新电平 = fsm->stable_level）
 */
static bool debounce_feed(button_fsm_t *fsm, bool pressed, uint32_t now_ms)
{
    if (!fsm->raw_initialized) {
        /* 第一个样本：直接认作初始稳定电平。开机时按键可能本来就按着，
         * 不能把"初始状态"误判成一次按下沿 */
        fsm->raw_initialized = true;
        fsm->raw_level = pressed;
        fsm->raw_change_ms = now_ms;
        /* 首次样本直接作为初始稳定电平，不产生翻转事件 */
        fsm->stable_level = pressed;
        return false;
    }

    if (pressed != fsm->raw_level) {
        /* 原始电平刚翻转：记下翻转时刻，重新开始计 20ms 稳定时间 */
        fsm->raw_level = pressed;
        fsm->raw_change_ms = now_ms;
    }

    if (pressed != fsm->stable_level &&
        elapsed_ms(now_ms, fsm->raw_change_ms) >= (int32_t)fsm->debounce_ms) {
        /* 新电平已连续保持 ≥ debounce_ms：采纳为稳定电平，报告一次翻转 */
        fsm->stable_level = pressed;
        return true;
    }

    return false;
}

/**
 * @brief 喂入一个原始电平样本，返回本次样本产生的事件
 *
 * 应由驱动以固定周期（本工程 10ms）调用；传入的时间戳必须单调递增，
 * 允许 uint32_t 回绕。每次调用至多返回一个事件。
 *
 * 状态迁移要点：
 *   · RELEASED→PRESSED 在**确认按下沿**时发生（按下瞬间记 press_start_ms，
 *     长按计时从这里开始）；
 *   · PRESSED→WAIT_DBL 在**释放沿**发生（先记一次 click，但单击事件要等
 *     双击窗口超时才发——若窗口内又按下则升级为双击，这就是单击"故意延迟"
 *     300ms 才上报的原因）；
 *   · PRESSED→LONG 在按住时长达到 long_press_ms 时发生，此时就上报
 *     LONG_PRESS_START（不等释放，方便 UI 立刻给出长按反馈）；
 *   · LONG→RELEASED 在释放时发生，click 计数作废（长按与点击互斥）。
 *
 * @param fsm      状态机，不可为 NULL
 * @param pressed  当前原始电平：true=按下（低有效按键的取反由调用方完成）
 * @param now_ms   当前毫秒时间戳
 * @return 本次产生的事件；无事件时为 BUTTON_FSM_NONE
 */
button_fsm_event_t button_fsm_update(button_fsm_t *fsm, bool pressed, uint32_t now_ms)
{
    if (fsm == NULL) {
        return BUTTON_FSM_NONE;
    }

    bool edge = debounce_feed(fsm, pressed, now_ms); /* 消抖后的稳定电平是否刚翻转 */
    bool level = fsm->stable_level;                 /* 消抖后的当前稳定电平 */

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
