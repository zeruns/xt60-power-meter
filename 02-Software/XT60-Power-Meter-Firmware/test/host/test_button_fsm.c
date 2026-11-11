/**
 * @file test_button_fsm.c
 * @brief button_fsm 按键状态机的主机单元测试（PC 编译运行，无需开发板）
 *
 * 编译运行：
 *   gcc -std=c11 -Wall -Wextra -I../../components/drivers/key_button/include \
 *       test_button_fsm.c ../../components/drivers/key_button/src/button_fsm.c \
 *       -o test_button_fsm.exe
 *   ./test_button_fsm.exe
 *
 * 覆盖：消抖毛刺过滤、单击（延迟确认）、双击、长按、长按不重复、
 *       两次独立单击不粘连、单击与长按互斥。
 *
 * 喂样本节奏与真机一致：真机每 10ms 采样一次并调用 button_fsm_update()，
 * 这里也用 10ms 步长推进时间轴，保证测的是真实时序行为。
 */
#include <stdio.h>

#include "button_fsm.h"

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (cond) {                                                                                \
            s_pass++;                                                                              \
        } else {                                                                                   \
            s_fail++;                                                                             \
            printf("  [失败] 第 %d 行: %s\n", __LINE__, #cond);                                    \
        }                                                                                          \
    } while (0)

/** 事件计数（索引为 button_fsm_event_t） */
typedef struct {
    int count[5];
} event_log_t;

static void log_event(event_log_t *log, button_fsm_event_t ev)
{
    log->count[(int)ev]++;
}

/**
 * @brief 在 [start_ms, end_ms) 区间内以固定步长喂入同一电平
 * @return 区间结束时的时间戳（ms）
 */
static uint32_t feed_level(button_fsm_t *fsm, event_log_t *log, bool pressed,
                           uint32_t start_ms, uint32_t end_ms, uint32_t step_ms)
{
    uint32_t t = start_ms;
    while (t < end_ms) {
        log_event(log, button_fsm_update(fsm, pressed, t));
        t += step_ms;
    }
    return t;
}

/** 释放后额外跑过双击窗口，确保延迟的单击事件被派发 */
static uint32_t flush_idle(button_fsm_t *fsm, event_log_t *log, uint32_t t)
{
    return feed_level(fsm, log, false, t, t + BUTTON_FSM_DOUBLE_GAP_MS + 30, 10);
}

/** 模拟一次「按下 hold_ms 后松开」（首尾各带释放态） */
static uint32_t click_once(button_fsm_t *fsm, event_log_t *log, uint32_t t, uint32_t hold_ms)
{
    t = feed_level(fsm, log, true, t, t + hold_ms, 10);
    return feed_level(fsm, log, false, t, t + 10, 10);
}

int main(void)
{
    button_fsm_t fsm;
    event_log_t log;
    uint32_t t;

    /* 每个用例的约定：初始化后先喂一个 t=0 的释放态样本。
     * 真实驱动里 FSM 开始工作时按键必为释放态（K1/BOOT 在复位期间被按住的
     * 情况会被 strapping 保护挡住，见 button.c），时间轴不允许从"悬空按下"开始。 */
#define FSM_RESET()                                                                               \
    do {                                                                                           \
        button_fsm_init(&fsm);                                                                     \
        for (int i_ = 0; i_ < 5; i_++) log.count[i_] = 0;                                         \
        log_event(&log, button_fsm_update(&fsm, false, 0));                                        \
        t = 0;                                                                                     \
    } while (0)

    /* ---- 用例 1：短按 50ms，松开后过双击窗口 → 恰好 1 次单击 ---- */
    FSM_RESET();
    t = click_once(&fsm, &log, t, 50);
    flush_idle(&fsm, &log, t);
    CHECK(log.count[BUTTON_FSM_SINGLE_CLICK] == 1);
    CHECK(log.count[BUTTON_FSM_DOUBLE_CLICK] == 0);
    CHECK(log.count[BUTTON_FSM_LONG_PRESS_START] == 0);
    CHECK(log.count[BUTTON_FSM_LONG_PRESS_RELEASE] == 0);

    /* ---- 用例 2：两击间空闲 100ms（<300ms 双击窗口）→ 双击 1 次，且无单击 ---- */
    FSM_RESET();
    t = click_once(&fsm, &log, t, 50);                 /* 第一击，t≈60 */
    t = feed_level(&fsm, &log, false, t, t + 100, 10); /* 两击间空闲 100ms（按下间隔约 160ms，仍 <300ms） */
    t = click_once(&fsm, &log, t, 50);                 /* 第二击 */
    flush_idle(&fsm, &log, t);
    CHECK(log.count[BUTTON_FSM_DOUBLE_CLICK] == 1);
    CHECK(log.count[BUTTON_FSM_SINGLE_CLICK] == 0);

    /* ---- 用例 3：按下间隔约 490ms（>300ms）→ 两次独立单击 ---- */
    FSM_RESET();
    t = click_once(&fsm, &log, t, 50);
    t = flush_idle(&fsm, &log, t);                     /* 第一击确认 */
    t = feed_level(&fsm, &log, false, t, t + 100, 10);
    t = click_once(&fsm, &log, t, 50);
    flush_idle(&fsm, &log, t);                         /* 第二击确认 */
    CHECK(log.count[BUTTON_FSM_SINGLE_CLICK] == 2);
    CHECK(log.count[BUTTON_FSM_DOUBLE_CLICK] == 0);

    /* ---- 用例 4：按住 900ms（>800ms）→ 长按开始 1 次；松开 → 长按释放 ---- */
    FSM_RESET();
    t = feed_level(&fsm, &log, true, t, t + 900, 10);
    t = feed_level(&fsm, &log, false, t, t + 350, 10);
    CHECK(log.count[BUTTON_FSM_LONG_PRESS_START] == 1);
    CHECK(log.count[BUTTON_FSM_LONG_PRESS_RELEASE] == 1);
    CHECK(log.count[BUTTON_FSM_SINGLE_CLICK] == 0);
    CHECK(log.count[BUTTON_FSM_DOUBLE_CLICK] == 0);

    /* ---- 用例 5：长按持续 2000ms，LONG_PRESS_START 只触发一次 ---- */
    FSM_RESET();
    feed_level(&fsm, &log, true, t, t + 2000, 10);
    CHECK(log.count[BUTTON_FSM_LONG_PRESS_START] == 1);

    /* ---- 用例 6：消抖——按下毛刺仅持续 10ms（<20ms）应被忽略 ---- */
    FSM_RESET();
    t = feed_level(&fsm, &log, true, t, t + 10, 10);          /* 10ms 毛刺 */
    t = feed_level(&fsm, &log, false, t, t + 100, 10);        /* 恢复 */
    flush_idle(&fsm, &log, t);
    CHECK(log.count[BUTTON_FSM_SINGLE_CLICK] == 0);
    CHECK(log.count[BUTTON_FSM_LONG_PRESS_START] == 0);

    /* ---- 用例 7：释放毛刺（按住中抖一下 10ms）不应误判为单击 ---- */
    FSM_RESET();
    t = feed_level(&fsm, &log, true, t, t + 100, 10);
    t = feed_level(&fsm, &log, false, t, t + 10, 10);         /* 10ms 释放毛刺 */
    t = feed_level(&fsm, &log, true, t, t + 900, 10);         /* 继续按住到长按 */
    feed_level(&fsm, &log, false, t, t + 350, 10);
    /* 总按住（忽略毛刺）远超 800ms：应判长按，无 click 泄漏 */
    CHECK(log.count[BUTTON_FSM_LONG_PRESS_START] == 1);
    CHECK(log.count[BUTTON_FSM_SINGLE_CLICK] == 0);

    if (s_fail == 0) {
        printf("button_fsm: %d 项检查全部通过\n", s_pass);
        return 0;
    }
    printf("button_fsm: %d 通过 / %d 失败\n", s_pass, s_fail);
    return 1;
}
