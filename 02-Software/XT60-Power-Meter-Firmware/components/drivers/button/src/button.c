/**
 * @file button.c
 * @brief 实体按键驱动实现（GPIO 输入 + 消抖 + 短按/长按/释放事件）
 *
 * 器件：K1 = IO0（兼 BOOT），K2 = IO4，均为低电平有效（按下接地）。
 * 引脚见 board_pins.h 的 BOARD_PIN_KEY_BOOT / BOARD_PIN_KEY_USER。
 *
 * 阅读顺序建议：
 *   ① button_init()      —— 配 GPIO 输入 + 启动周期消抖定时器；
 *   ② button_tick()      —— 每 15ms 采样一次，做消抖与事件判定；
 *   ③ dispatch()         —— 把事件回调派发给上层（在 esp_timer 任务上下文）。
 *
 * ============================================================
 * 为什么用 esp_driver_gpio + esp_timer 自实现，而非托管组件 espressif/button
 * ============================================================
 * 1. 类型冲突：托管组件 iot_button.h 也定义了名为 button_event_t 的类型，
 *    与本工程固定接口 button.h 的 button_event_t 同名不同义，两者在同一编译
 *    单元内会重定义报错。本接口为既定契约，不可改名。
 * 2. 需求差异：本接口的事件模型（SHORT/LONG/RELEASE）与 IO0 strapping 脚
 *    「启动早期忽略」的安全要求，都需要定制映射。
 * 故按官方底层驱动 esp_driver_gpio + esp_timer 实现，与 status_led 风格一致，
 * 无额外托管依赖。esp_driver_gpio 本身即 ESP-IDF 官方驱动组件。
 *
 * ============================================================
 * 关于 strapping 脚 IO0（K1 / BOOT 键）的安全处理
 * ============================================================
 * IO0 复位期间须为高，按住复位会进入下载模式。因此：
 *   · 本驱动只把 IO0 配成「输入 + 上拉」，永不主动驱动它，不影响 strapping；
 *   · 「启动早期忽略」：若该键在初始化时正处于按下态（即复位期间被按住），
 *     则在它第一次被释放之前不派发任何事件（仅内部记录），避免把复位误触
 *     当成业务按键动作。释放后即恢复正常检测。
 *
 * ============================================================
 * 上板调试要点（按键无反应/误触发时排查）
 * ============================================================
 *   · 无反应：核对 IO0/IO4 电平（按下应为低）、上拉电阻、按键是否虚焊。
 *   · 一次按下触发多次：多为机械抖动，增大 BUTTON_DEBOUNCE_TICKS。
 *   · 长按误判为短按：调 BUTTON_LONG_PRESS_MS 阈值。
 *   · 回调上下文：回调在 esp_timer 任务里执行，禁止阻塞/长延时，
 *     需要耗时操作请自行投递到业务任务（如 event_bus）。
 * ============================================================
 */
#include "button.h"

#include <string.h>

#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

/** 日志标签 */
static const char *TAG = "button";

/* 消抖与长按参数 */
#define BUTTON_POLL_MS         15  /**< 采样周期（毫秒） */
#define BUTTON_DEBOUNCE_TICKS  2   /**< 连续稳定采样次数才认作有效电平（≈30ms） */
#define BUTTON_LONG_PRESS_MS   800 /**< 长按阈值（毫秒） */

/** 单个按键的运行状态 */
typedef struct {
    gpio_num_t pin;              /**< 绑定的 GPIO */
    const char *name;            /**< 名称，仅用于日志 */
    bool suppress_until_release; /**< strapping 安全：首次释放前不派发事件 */

    bool raw_pressed;      /**< 上次采样的原始按下态（未消抖） */
    bool stable_pressed;   /**< 消抖后的稳定按下态 */
    int debounce_cnt;      /**< 连续同态采样计数 */

    int64_t press_start_us; /**< 本次按下的起始时刻（微秒） */
    bool long_fired;        /**< 本次按下是否已派发过长按 */

    button_event_cb_t cb; /**< 事件回调 */
    void *cb_ctx;         /**< 回调用户上下文 */
} button_state_t;

/** 全部按键状态 */
static button_state_t s_buttons[BUTTON_ID_MAX];

/** 消抖定时器句柄 */
static esp_timer_handle_t s_timer = NULL;

/** 是否已初始化 */
static bool s_initialized = false;

/**
 * @brief 派发事件给注册的回调
 *
 * 在 esp_timer 任务上下文执行，回调不得阻塞。
 */
static void dispatch(button_state_t *btn, button_event_t event)
{
    if (btn->suppress_until_release) {
        /* 启动早期忽略期：仅记录，不触发业务动作 */
        ESP_LOGD(TAG, "%s 按键事件 %d 被忽略（strapping 启动保护中）", btn->name, event);
        return;
    }
    if (btn->cb != NULL) {
        btn->cb((button_id_t)(btn - s_buttons), event, btn->cb_ctx);
    }
}

/**
 * @brief 消抖定时器周期回调：采样 + 事件判定
 */
static void button_tick(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < BUTTON_ID_MAX; i++) {
        button_state_t *btn = &s_buttons[i];

        /* 低电平有效：读到 0 即按下 */
        bool pressed = (gpio_get_level(btn->pin) == 0);

        /* ---- 消抖：连续 BUTTON_DEBOUNCE_TICKS 次同态才更新稳定态 ---- */
        if (pressed == btn->raw_pressed) {
            btn->debounce_cnt++;
        } else {
            btn->raw_pressed = pressed;
            btn->debounce_cnt = 0;
        }

        if (btn->debounce_cnt < BUTTON_DEBOUNCE_TICKS || pressed == btn->stable_pressed) {
            /* 尚未稳定，或稳定态未变化：只做长按计时维持 */
            if (btn->stable_pressed && !btn->long_fired &&
                (now - btn->press_start_us) >= (int64_t)BUTTON_LONG_PRESS_MS * 1000) {
                btn->long_fired = true;
                dispatch(btn, BUTTON_EVENT_LONG_PRESS);
            }
            continue;
        }

        /* ---- 稳定态发生翻转 ---- */
        btn->stable_pressed = pressed;

        if (pressed) {
            /* 按下沿：记录起始时刻，进入 strapping 保护判定 */
            btn->press_start_us = now;
            btn->long_fired = false;
            if (btn->suppress_until_release) {
                /* 复位期间被按住的键：维持忽略，等释放 */
                ESP_LOGD(TAG, "%s 在启动保护期内按下，忽略", btn->name);
            }
        } else {
            /* 释放沿：解除启动保护（首次释放即恢复），并派发释放/短按 */
            bool was_suppressed = btn->suppress_until_release;
            btn->suppress_until_release = false;

            if (!was_suppressed) {
                dispatch(btn, BUTTON_EVENT_RELEASE);
                if (!btn->long_fired) {
                    dispatch(btn, BUTTON_EVENT_SHORT_PRESS);
                }
            } else {
                ESP_LOGD(TAG, "%s 首次释放，解除启动保护，后续按键恢复正常", btn->name);
            }
        }
    }
}

esp_err_t button_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_buttons, 0, sizeof(s_buttons));

    /* ---- ① 绑定按键与 GPIO ---- */
    s_buttons[BUTTON_ID_BOOT].pin = BOARD_PIN_KEY_BOOT;
    s_buttons[BUTTON_ID_BOOT].name = "K1(BOOT/IO0)";
    s_buttons[BUTTON_ID_BOOT].suppress_until_release = true; /* strapping 脚，启动早期忽略 */
    s_buttons[BUTTON_ID_USER].pin = BOARD_PIN_KEY_USER;
    s_buttons[BUTTON_ID_USER].name = "K2(USER/IO4)";
    s_buttons[BUTTON_ID_USER].suppress_until_release = false;

    /* ---- ② 配置 GPIO：输入 + 上拉（低有效按键），绝不驱动输出 ----
     * IO0 尤其重要：只做输入，避免把 strapping 脚拉向非默认电平。 */
    for (int i = 0; i < BUTTON_ID_MAX; i++) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_buttons[i].pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s 引脚 IO%d 配置失败: %s", s_buttons[i].name, s_buttons[i].pin,
                     esp_err_to_name(err));
            return err;
        }
    }

    /* ---- ③ strapping 保护的精确化 ----
     * suppress_until_release 的本意是「复位期间被按住的键，在其首次释放前不派发」。
     * 因此这里按**实际电平**判定，而不是一律保护：
     *   · 此刻该键未被按下（高电平）→ 复位期间没被按住，无需保护，立即解除；
     *   · 此刻该键正被按下（低电平）→ 保持保护，等第一次释放时解除。
     * 否则正常启动后，用户的第一次按键会被白白吞掉（曾经就是这个行为）。 */
    for (int i = 0; i < BUTTON_ID_MAX; i++) {
        if (s_buttons[i].suppress_until_release && gpio_get_level(s_buttons[i].pin) != 0) {
            s_buttons[i].suppress_until_release = false;
            ESP_LOGI(TAG, "%s 启动时未按下，无需 strapping 保护", s_buttons[i].name);
        }
    }

    /* ---- ④ 启动周期消抖定时器 ----
     * 用周期 esp_timer 轮询而非 GPIO 中断：两个按键消抖本就是周期采样，
     * 无 ISR，简单可靠，且与 status_led 的定时器用法一致。 */
    const esp_timer_create_args_t timer_cfg = {
        .callback = button_tick,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "button_tick",
    };
    esp_err_t err = esp_timer_create(&timer_cfg, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "消抖定时器创建失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_timer, BUTTON_POLL_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "消抖定时器启动失败: %s", esp_err_to_name(err));
        esp_timer_delete(s_timer);
        s_timer = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成（%d 个按键，消抖 %d×%dms，长按阈值 %dms）", BUTTON_ID_MAX,
             BUTTON_DEBOUNCE_TICKS, BUTTON_POLL_MS, BUTTON_LONG_PRESS_MS);
    return ESP_OK;
}

esp_err_t button_register_callback(button_id_t id, button_event_cb_t cb, void *user_ctx)
{
    if (id >= BUTTON_ID_MAX || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        /* 尚未初始化：无法写入状态机，明确报错避免静默丢失 */
        return ESP_ERR_INVALID_STATE;
    }

    s_buttons[id].cb = cb;
    s_buttons[id].cb_ctx = user_ctx;
    return ESP_OK;
}
