/**
 * @file status_led.c
 * @brief 系统运行状态指示灯实现
 *
 * 实现思路：内部创建一个小任务，每轮按「当前模式」的时序点亮 → 延时 → 熄灭 → 延时。
 * 模式变量声明为 volatile，任务每轮重新读取，因此切换模式最迟在
 * 当前闪烁周期结束后生效，无需加锁，也无需唤醒任务。
 */
#include "status_led.h"

#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** 日志标签 */
static const char *TAG = "status_led";

/** 单个闪烁模式的时序配置 */
typedef struct {
    uint32_t on_ms;  /**< 点亮持续时间（毫秒） */
    uint32_t off_ms; /**< 熄灭持续时间（毫秒），为 0 表示常亮 */
} led_pattern_cfg_t;

/** 各模式对应的时序表，下标即 status_led_pattern_t */
static const led_pattern_cfg_t s_patterns[] = {
    [STATUS_LED_PATTERN_SOLID] = { .on_ms = 1000, .off_ms = 0 }, /* 常亮：初始化中 */
    [STATUS_LED_PATTERN_SLOW] = { .on_ms = 500, .off_ms = 500 }, /* 慢闪 1 Hz：正常运行 */
    [STATUS_LED_PATTERN_FAST] = { .on_ms = 100, .off_ms = 100 }, /* 快闪 5 Hz：异常 */
};

/** 模式数量，用于参数合法性检查 */
#define PATTERN_COUNT (sizeof(s_patterns) / sizeof(s_patterns[0]))

/** 闪烁任务句柄，同时用作「驱动是否已初始化」的标志 */
static TaskHandle_t s_led_task;

/** 当前闪烁模式（任务每轮读取，故用 volatile） */
static volatile status_led_pattern_t s_pattern = STATUS_LED_PATTERN_SOLID;

/**
 * @brief 点亮指示灯（注意：硬件为低电平点亮）
 */
static inline void led_on(void)
{
    gpio_set_level(BOARD_PIN_SYS_LED, BOARD_SYS_LED_ACTIVE_LEVEL);
}

/**
 * @brief 熄灭指示灯（输出有效电平的反相值）
 */
static inline void led_off(void)
{
    gpio_set_level(BOARD_PIN_SYS_LED, !BOARD_SYS_LED_ACTIVE_LEVEL);
}

/**
 * @brief 指示灯闪烁任务
 *
 * 死循环按当前模式翻转 GPIO。每轮开始时重新读取 s_pattern，
 * 因此模式切换无需与任务同步。
 */
static void status_led_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* 取本轮生效的时序；模式在延时期间被更改时，下一轮自动采用新值 */
        const led_pattern_cfg_t cfg = s_patterns[s_pattern];

        led_on();
        vTaskDelay(pdMS_TO_TICKS(cfg.on_ms));

        if (cfg.off_ms == 0) {
            /* 常亮模式：保持点亮，直接进入下一轮重新点亮 */
            continue;
        }

        led_off();
        vTaskDelay(pdMS_TO_TICKS(cfg.off_ms));
    }
}

esp_err_t status_led_init(void)
{
    /* 配置为输出；LED 由 3V3 经限流电阻供电，无需内部上下拉 */
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_SYS_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 上电先熄灭，随后由状态机决定实际模式 */
    led_off();

    /* 优先级 4：高于普通业务任务但低于采样任务，保证指示灯时序稳定 */
    if (xTaskCreate(status_led_task, "sys_led", 2048, NULL, 4, &s_led_task) != pdPASS) {
        ESP_LOGE(TAG, "创建闪烁任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IO%d 初始化完成（低电平点亮）", BOARD_PIN_SYS_LED);
    return ESP_OK;
}

esp_err_t status_led_set_pattern(status_led_pattern_t pattern)
{
    /* 越界检查：防止数组下标越界访问 s_patterns */
    if ((size_t)pattern >= PATTERN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 任务句柄为空说明 init 未执行或被调用顺序错误 */
    if (s_led_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pattern = pattern;
    return ESP_OK;
}
