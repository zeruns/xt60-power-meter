/**
 * @file button.c
 * @brief 实体按键驱动实现（GPIO 输入 + 周期采样 + 状态机事件判定）
 *
 * 器件：K1 = IO0（兼 BOOT），K2 = IO4，均为低电平有效（按下接地）。
 * 引脚见 board_pins.h 的 BOARD_PIN_KEY_BOOT / BOARD_PIN_KEY_USER。
 *
 * 阅读顺序建议：
 *   ① button_init()    —— 配 GPIO 输入 + 启动 10ms 周期采样定时器；
 *   ② button_tick()    —— 每 10ms 采样一次，喂给纯逻辑状态机并派发事件；
 *   ③ button_fsm.h/.c  —— 消抖 / 单击 / 双击 / 长按的时序判定（可主机单测）。
 *
 * ============================================================
 * 为什么用 esp_driver_gpio + esp_timer 自实现，而非托管组件 espressif/button
 * ============================================================
 * 1. 类型冲突：托管组件 iot_button.h 也定义了名为 button_event_t 的类型，
 *    与本工程固定接口 button.h 的 button_event_t 同名不同义，两者在同一编译
 *    单元内会重定义报错。本接口为既定契约，不可改名。
 * 2. 需求差异：本接口的事件模型（单击/双击/长按）与 IO0 strapping 脚
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
 *     则在它第一次被释放之前不向状态机喂任何电平（仅内部记录），避免把复位
 *     误触当成业务按键动作。释放后初始化状态机并恢复正常检测。
 *
 * ============================================================
 * 回调上下文约束
 * ============================================================
 *   回调在 esp_timer 任务里执行，禁止阻塞/长延时，禁止直接调用 LVGL API。
 *   原因：LVGL 不是线程安全的，所有 lv_* 调用必须发生在持有 LVGL 锁的 UI
 *   任务上下文里；在 esp_timer 任务里直接碰 LVGL 会导致竞态（画面撕裂、
 *   内存损坏甚至 HardFault）。UI 层的做法是把事件投递到自己的消息队列
 *   （xQueueSend 之类的投递是非阻塞的，允许在定时器回调里做），
 *   在 UI 任务里消费后再操作界面。
 * ============================================================
 */
#include "button.h"

#include <string.h>

#include "board_pins.h"
#include "button_fsm.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

/** 日志标签 */
static const char *TAG = "button";

/** 采样周期（毫秒）。状态机的时序阈值全部基于该时基（可整除 20/300/800） */
#define BUTTON_POLL_MS 10

/** 单个按键的运行状态 */
typedef struct {
    gpio_num_t pin;              /**< 绑定的 GPIO */
    const char *name;            /**< 名称，仅用于日志 */
    bool suppress_until_release; /**< strapping 安全：首次释放前不喂状态机 */
    bool fsm_ready;              /**< 状态机是否已开始喂样本 */

    button_fsm_t fsm; /**< 纯逻辑按键状态机 */

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
 * @brief 把状态机事件映射为驱动事件并派发给注册的回调
 *
 * 两层事件类型是分开的：button_fsm_event_t 是纯逻辑层的输出（可主机单测），
 * button_event_t 是本驱动对外的契约（见 button.h）。这里只做一一对应转发。
 *
 * 在 esp_timer 任务上下文执行，回调不得阻塞（见文件头"回调上下文约束"）。
 *
 * @param btn 按键状态（内含已注册的回调与用户上下文）
 * @param ev  状态机本次输出的事件（BUTTON_FSM_NONE 直接忽略）
 */
static void dispatch_fsm_event(button_state_t *btn, button_fsm_event_t ev)
{
    button_event_t event;
    switch (ev) {
    case BUTTON_FSM_SINGLE_CLICK:
        event = BUTTON_EVENT_SINGLE_CLICK;
        break;
    case BUTTON_FSM_DOUBLE_CLICK:
        event = BUTTON_EVENT_DOUBLE_CLICK;
        break;
    case BUTTON_FSM_LONG_PRESS_START:
        event = BUTTON_EVENT_LONG_PRESS_START;
        break;
    case BUTTON_FSM_LONG_PRESS_RELEASE:
        event = BUTTON_EVENT_LONG_PRESS_RELEASE;
        break;
    case BUTTON_FSM_NONE:
    default:
        return;
    }

    if (btn->cb != NULL) {
        /* 指针相减 btn - s_buttons：算出当前按键在数组中的下标，即 button_id_t。
         * 比给结构体多存一个 id 字段更省事，也天然与数组下标一致 */
        btn->cb((button_id_t)(btn - s_buttons), event, btn->cb_ctx);
    }
}

/**
 * @brief 采样定时器周期回调：读电平 → strapping 过滤 → 喂状态机 → 派发事件
 *
 * 每 10ms 被 esp_timer 调度一次，逐个按键走完整个"采样→判定→派发"流水线。
 *
 * @param arg 未使用（创建定时器时传入的 arg，本驱动用全局数组代替）
 * @note 运行在 esp_timer 任务上下文：整个函数必须短小、不阻塞、不碰 LVGL。
 */
static void button_tick(void *arg)
{
    (void)arg;
    /* 毫秒时间戳（允许回绕，状态机内部按有符号差值处理）。
     * esp_timer_get_time() 返回自启动以来的微秒数(64位)，除以 1000 取毫秒 */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (int i = 0; i < BUTTON_ID_MAX; i++) {
        button_state_t *btn = &s_buttons[i];

        /* 低电平有效：读到 0 即按下。
         * gpio_get_level() 读取 GPIO 当前输入电平（返回 0 或 1） */
        bool pressed = (gpio_get_level(btn->pin) == 0);

        /* ---- strapping 启动保护：首次释放前完全不喂状态机 ----
         * 保护期内只观察电平：一旦读到释放（高电平），说明这是复位后的
         * 第一次真实松手，此刻才初始化状态机并放行后续检测 */
        if (btn->suppress_until_release) {
            if (!pressed) {
                btn->suppress_until_release = false;
                btn->fsm_ready = true;
                button_fsm_init(&btn->fsm); /* 重新初始化状态机：从干净状态起步 */
                ESP_LOGD(TAG, "%s 首次释放，解除启动保护，按键状态机开始工作", btn->name);
            } else {
                ESP_LOGD(TAG, "%s 启动保护期内保持按下，忽略", btn->name);
            }
            continue;
        }

        if (!btn->fsm_ready) {
            /* 普通按键首次进入：初始化一次状态机（之后不再重复） */
            btn->fsm_ready = true;
            button_fsm_init(&btn->fsm);
        }

        /* 把本次采样喂给纯逻辑状态机；若有事件（单击/双击/长按）则派发回调 */
        button_fsm_event_t ev = button_fsm_update(&btn->fsm, pressed, now_ms);
        dispatch_fsm_event(btn, ev);
    }
}

/**
 * @brief 初始化按键驱动：绑 GPIO → 配输入上拉 → strapping 保护精化 → 启动采样定时器
 *
 * 四步顺序有讲究（见函数体内各段注释）；任何一步失败都返回对应错误码，
 * 不会留半初始化状态。重复调用返回 ESP_ERR_INVALID_STATE。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化过；
 *         其他为 gpio_config / esp_timer 的错误码
 */
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
            .pin_bit_mask = 1ULL << s_buttons[i].pin, /* 用位掩码指定引脚（第 N 位为 1 即选中 IO_N） */
            .mode = GPIO_MODE_INPUT,                  /* 输入模式：只读电平，不驱动引脚 */
            .pull_up_en = GPIO_PULLUP_ENABLE,         /* 使能内部上拉：松手时被拉到高电平 */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,    /* 关闭下拉：与上拉互斥，同时开没有意义 */
            .intr_type = GPIO_INTR_DISABLE,           /* 不用 GPIO 中断：靠定时器轮询采样 */
        };
        esp_err_t err = gpio_config(&cfg);
        /* ^ 按上面的配置结构体一次性配置引脚，返回 ESP_OK 表示配置成功 */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s 引脚 IO%d 配置失败: %s", s_buttons[i].name, s_buttons[i].pin,
                     esp_err_to_name(err));
            return err;
        }
        button_fsm_init(&s_buttons[i].fsm);
    }

    /* ---- ③ strapping 保护的精确化 ----
     * suppress_until_release 的本意是「复位期间被按住的键，在其首次释放前不派发」。
     * 因此这里按**实际电平**判定，而不是一律保护：
     *   · 此刻该键未被按下（高电平）→ 复位期间没被按住，无需保护，立即解除；
     *   · 此刻该键正被按下（低电平）→ 保持保护，等第一次释放时解除。
     * 否则正常启动后，用户的第一次按键会被白白吞掉（曾经就是这个行为）。 */
    for (int i = 0; i < BUTTON_ID_MAX; i++) {
        /* gpio_get_level() 读 GPIO 电平（0/1）：== 0 即此刻正按着 */
        if (s_buttons[i].suppress_until_release && gpio_get_level(s_buttons[i].pin) != 0) {
            s_buttons[i].suppress_until_release = false;
            s_buttons[i].fsm_ready = true;
            ESP_LOGI(TAG, "%s 启动时未按下，无需 strapping 保护", s_buttons[i].name);
        }
    }

    /* ---- ④ 启动周期采样定时器 ----
     * 用周期 esp_timer 轮询而非 GPIO 中断：按键判定本就是周期采样，
     * 无 ISR，简单可靠，且与 status_led 的定时器用法一致。 */
    const esp_timer_create_args_t timer_cfg = {
        .callback = button_tick,     /* 周期到点时被调用的函数 */
        .arg = NULL,                 /* 传给回调的参数（这里不用） */
        .dispatch_method = ESP_TIMER_TASK, /* 回调在 esp_timer 任务里执行（不在 ISR 上下文） */
        .name = "button_tick",       /* 定时器名，仅调试用 */
    };
    esp_err_t err = esp_timer_create(&timer_cfg, &s_timer);
    /* ^ 创建一个 esp_timer 软件定时器（基于硬件定时器+任务调度），句柄存到 s_timer */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "采样定时器创建失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(s_timer, BUTTON_POLL_MS * 1000);
    /* ^ 启动周期定时器，每 10000 微秒(=10ms)触发一次 button_tick 回调 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "采样定时器启动失败: %s", esp_err_to_name(err));
        esp_timer_delete(s_timer); /* 创建了却没启动成功：删掉定时器，别泄漏句柄 */
        s_timer = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成（%d 个按键，采样 %dms，消抖 %dms，双击窗口 %dms，长按 %dms）",
             BUTTON_ID_MAX, BUTTON_POLL_MS, BUTTON_FSM_DEBOUNCE_MS, BUTTON_FSM_DOUBLE_GAP_MS,
             BUTTON_FSM_LONG_MS);
    return ESP_OK;
}

/**
 * @brief 注册按键事件回调（单击/双击/长按起/长按释）
 *
 * 每个按键同一时刻只能有一个回调，重复注册会覆盖旧的。回调在 esp_timer
 * 任务上下文执行，实现方须遵守"回调上下文约束"（见文件头）。
 *
 * @param id       按键编号（BUTTON_ID_BOOT / BUTTON_ID_USER）
 * @param cb       回调函数，不可为 NULL
 * @param user_ctx 透传给回调的用户指针（典型用法：传 UI 层的消息队列句柄）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法；ESP_ERR_INVALID_STATE 尚未 button_init()
 */
esp_err_t button_register_callback(button_id_t id, button_event_cb_t cb, void *user_ctx)
{
    if (id >= BUTTON_ID_MAX || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        /* 尚未初始化：无法写入状态，明确报错避免静默丢失 */
        return ESP_ERR_INVALID_STATE;
    }

    s_buttons[id].cb = cb;
    s_buttons[id].cb_ctx = user_ctx;
    return ESP_OK;
}
