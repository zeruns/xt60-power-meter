/**
 * @file status_led.c
 * @brief 系统运行状态指示灯实现（esp_timer 步进式闪烁）
 *
 * 阅读顺序建议：
 *   ① 先看下面的「步骤序列」数据结构与四个模式的序列表；
 *   ② 再看 led_advance()——它是核心：应用当前步骤的电平，并安排下一步；
 *   ③ 最后看 status_led_set_pattern()——切换模式时重置到序列开头。
 */
#include "status_led.h"

#include <stdbool.h>

#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/** 日志标签 */
static const char *TAG = "status_led";

/* ============================================================
 * 闪烁时序参数
 *
 * ⚠ 本文件不是配置点。所有时序都由 Kconfig 提供，在下面直接以
 *   CONFIG_STATUS_LED_* 宏使用，**不设本地默认值**。
 *
 * 原因：曾经用过 "#ifdef CONFIG_xxx / #else 本地默认值 / #endif" 的写法，
 *       但由于 Kconfig.projbuild 已定义该项，sdkconfig 中必然存在对应宏，
 *       于是 #else 分支成为**死代码**——改它没有任何效果且不报错，
 *       极易误判为"参数改了但硬件没反应"。
 *
 * 修改方法：
 *   ① idf.py menuconfig → 组件配置 → 系统状态指示灯（SYS_LED）
 *   ② 或改 Kconfig.projbuild 的 default 后**删除 sdkconfig** 重新生成
 * ============================================================ */

/**
 * @brief 一个闪烁步骤：让灯处于某个状态并保持一段时间
 *
 * 用「步骤序列」而不是「一个 on/off 周期」来描述模式，
 * 是为了能用同一种逻辑表达出双闪、三闪等任意节奏。
 */
typedef struct {
    uint16_t duration_ms; /**< 本步骤持续时间（毫秒） */
    uint8_t lit;          /**< 1 = 点亮，0 = 熄灭（逻辑值，不涉及电平极性） */
} led_step_t;

/** 一个闪烁模式 = 一条步骤序列，执行到末尾后从头循环 */
typedef struct {
    const led_step_t *steps; /**< 步骤数组 */
    uint8_t count;           /**< 步骤个数 */
} led_sequence_t;

/* ---------- 四个模式的步骤序列 ---------- */

/** 常亮：只有一步，反复重新点亮；视觉上始终亮着 */
static const led_step_t k_solid_steps[] = {
    { CONFIG_STATUS_LED_SOLID_HOLD_MS, 1 },
};

/** 慢闪 1Hz（默认）：亮 500ms、灭 500ms */
static const led_step_t k_slow_steps[] = {
    { CONFIG_STATUS_LED_SLOW_ON_MS, 1 },
    { CONFIG_STATUS_LED_SLOW_OFF_MS, 0 },
};

/** 快闪 5Hz（默认）：亮 100ms、灭 100ms */
static const led_step_t k_fast_steps[] = {
    { CONFIG_STATUS_LED_FAST_ON_MS, 1 },
    { CONFIG_STATUS_LED_FAST_OFF_MS, 0 },
};

/** 降级：两次短闪 + 一段长灭（长灭让它在视觉上与慢闪明显区分） */
static const led_step_t k_degraded_steps[] = {
    { CONFIG_STATUS_LED_DEGRADED_ON_MS, 1 },
    { CONFIG_STATUS_LED_DEGRADED_OFF_MS, 0 },
    { CONFIG_STATUS_LED_DEGRADED_ON_MS, 1 },
    { CONFIG_STATUS_LED_DEGRADED_GAP_MS, 0 },
};

/** 模式 → 序列 的映射表，下标即 status_led_pattern_t */
static const led_sequence_t s_sequences[] = {
    [STATUS_LED_PATTERN_SOLID] = { k_solid_steps, 1 },
    [STATUS_LED_PATTERN_SLOW] = { k_slow_steps, 2 },
    [STATUS_LED_PATTERN_FAST] = { k_fast_steps, 2 },
    [STATUS_LED_PATTERN_DEGRADED] = { k_degraded_steps, 4 },
};

/** 模式（序列）总数，用于参数合法性检查 */
#define PATTERN_COUNT (sizeof(s_sequences) / sizeof(s_sequences[0]))

/* ---------- 模块内部状态 ---------- */

/** 闪烁定时器句柄；非空表示驱动已初始化 */
static esp_timer_handle_t s_timer = NULL;

/** 当前模式（volatile：会被不同任务/回调读写） */
static volatile status_led_pattern_t s_pattern = STATUS_LED_PATTERN_SOLID;

/** 当前执行到序列中的第几个步骤 */
static volatile uint8_t s_step_index = 0;

/**
 * 保护 s_pattern 与 s_step_index 的自旋锁
 *
 * 这两个变量会被「事件总线任务」（切换模式）和「esp_timer 回调」（推进步骤）
 * 同时访问，因此需要一小段临界区来保证不会读到写了一半的组合。
 * 临界区里只做几个赋值，开销可以忽略。
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 按逻辑值设置灯的物理电平（这里完成极性换算）
 *
 * 硬件是低电平点亮，所以：
 *   lit = 1 → 输出有效电平（BOARD_SYS_LED_ACTIVE_LEVEL = 0）
 *   lit = 0 → 输出反相电平
 */
static inline void led_write(bool lit)
{
    gpio_set_level(BOARD_PIN_SYS_LED,
                   lit ? BOARD_SYS_LED_ACTIVE_LEVEL : !BOARD_SYS_LED_ACTIVE_LEVEL);
}

/**
 * @brief 核心函数：应用当前步骤，并安排下一个步骤
 *
 * 执行顺序：
 *   ① 保证定时器处于停止状态（下一次启动要求定时器空闲）
 *   ② 读出「当前模式 + 当前步骤下标」
 *   ③ 输出这一步对应的电平
 *   ④ 把下标推进到下一步（到末尾则回到 0）
 *   ⑤ 启动一次性定时器，时长 = 本步骤时长；到点后本函数会被再次调用
 *
 * 关于并发：esp_timer 是一次性定时器，回调之间不会重入；
 * 与 set_pattern 并发时最坏情况只是多走/少走一个步骤（灯闪一下），
 * 对指示灯这种非关键外设完全可以接受，所以这里没有做更重的同步。
 */
static void led_advance(void)
{
    /* ① 停止定时器：esp_timer_start_once 要求定时器当前未被启动。
     *    若定时器本来就没在跑，会返回错误，这里刻意忽略。 */
    esp_timer_stop(s_timer);

    /* ② 读出当前模式与步骤下标 */
    portENTER_CRITICAL(&s_lock);
    status_led_pattern_t pattern = s_pattern;
    uint8_t index = s_step_index;
    portEXIT_CRITICAL(&s_lock);

    const led_sequence_t *seq = &s_sequences[pattern];
    if (index >= seq->count) {
        index = 0; /* 防御：下标越界时回到开头，避免读到非法内存 */
    }

    const led_step_t *step = &seq->steps[index];

    /* ③ 输出这一步的电平 */
    led_write(step->lit != 0);

    /* ④ 推进下标：走到末尾就绕回 0，实现无限循环 */
    portENTER_CRITICAL(&s_lock);
    s_step_index = (uint8_t)((index + 1) % seq->count);
    portEXIT_CRITICAL(&s_lock);

    /* ⑤ 安排下一次回调。esp_timer 的时间单位是「微秒」，故 ×1000 */
    esp_err_t err = esp_timer_start_once(s_timer, (uint64_t)step->duration_ms * 1000ULL);
    if (err != ESP_OK) {
        /* 极少发生；打印出来便于排查 */
        ESP_LOGW(TAG, "启动闪烁定时器失败: %s", esp_err_to_name(err));
    }
}

/**
 * @brief esp_timer 回调：每次定时到点，就推进到下一个闪烁步骤
 *
 * 注意：回调运行在 esp_timer 任务上下文（不是中断），所以可以调用日志、
 * 临界区等设施，但依然要保持简短——拖久了会影响系统里其他定时器。
 */
static void led_timer_callback(void *arg)
{
    (void)arg; /* 未使用回调参数 */

    led_advance();
}

esp_err_t status_led_init(void)
{
    /* 防重复初始化：定时器重复创建会泄漏资源 */
    if (s_timer != NULL) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    /* 配置 IO39 为输出。灯由 3V3 经限流电阻供电，因此不需要内部上下拉 */
    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_SYS_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IO%d 配置失败: %s", BOARD_PIN_SYS_LED, esp_err_to_name(err));
        return err;
    }

    /* 先确保灯是灭的，避免上电瞬间出现"莫名亮着"的状态 */
    led_write(false);

    /* 创建一次性定时器；dispatch_method 用默认（TASK），
     * 即回调在 esp_timer 任务里执行，不需要中断上下文。 */
    const esp_timer_create_args_t timer_cfg = {
        .callback = led_timer_callback,
        .arg = NULL,
        .name = "sys_led",
    };

    err = esp_timer_create(&timer_cfg, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建闪烁定时器失败: %s", esp_err_to_name(err));
        s_timer = NULL;
        return err;
    }

    /* 以默认模式启动序列（会立即点亮并安排下一步） */
    s_pattern = STATUS_LED_PATTERN_SOLID;
    s_step_index = 0;
    led_advance();

    ESP_LOGI(TAG, "IO%d 初始化完成（低电平点亮，esp_timer 步进式闪烁）", BOARD_PIN_SYS_LED);
    return ESP_OK;
}

esp_err_t status_led_set_pattern(status_led_pattern_t pattern)
{
    /* 参数检查：防止下标越界访问 s_sequences */
    if ((size_t)pattern >= PATTERN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 切换到新模式，并把序列重置到开头 */
    portENTER_CRITICAL(&s_lock);
    s_pattern = pattern;
    s_step_index = 0;
    portEXIT_CRITICAL(&s_lock);

    /* 立即执行新序列的第一步：这样切换模式能"马上看到"，
     * 不必等当前这一步走完（最坏可能要等 1 秒） */
    led_advance();

    return ESP_OK;
}
