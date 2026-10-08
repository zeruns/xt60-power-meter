/**
 * @file device_ctrl.c
 * @brief 设备控制服务实现：订阅系统状态事件 → 驱动指示灯与蜂鸣器
 */
#include "device_ctrl.h"

#include "buzzer.h"
#include "event_bus.h"
#include "settings.h"
#include "status_led.h"
#include "system_state.h"

#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "device_ctrl";

/**
 * 状态 → 指示灯模式 映射表，下标即 system_state_t
 *
 * 把映射关系集中成一张表的好处：新增状态时若忘了配灯，
 * 会因为数组元素缺失而暴露出来，不容易出现"加了状态但漏了提示"的情况。
 */
static const status_led_pattern_t s_state_to_led[] = {
    [SYSTEM_STATE_INIT] = STATUS_LED_PATTERN_SOLID,        /* 初始化中：常亮 */
    [SYSTEM_STATE_RUNNING] = STATUS_LED_PATTERN_SLOW,      /* 正常：慢闪 1 Hz */
    [SYSTEM_STATE_DEGRADED] = STATUS_LED_PATTERN_DEGRADED, /* 降级：双闪 */
    [SYSTEM_STATE_FAULT] = STATUS_LED_PATTERN_FAST,        /* 异常：快闪 5 Hz */
};

/** 状态表项数量，用于参数合法性检查 */
#define STATE_COUNT (sizeof(s_state_to_led) / sizeof(s_state_to_led[0]))

/**
 * @brief 判断蜂鸣器是否已被登记为故障
 *
 * 蜂鸣器是可选外设，坏了不应影响其他输出，也不该再徒劳地尝试发声。
 */
static bool is_buzzer_broken(void)
{
    return (system_state_get_fault_mask() & SYSTEM_FAULT_BUZZER) != 0;
}

/**
 * @brief 按给定状态更新所有输出（指示灯 + 蜂鸣器）
 *
 * 这是本模块的核心：把抽象的"系统状态"翻译成具体的设备行为。
 *
 * @param state 目标状态
 */
static void apply_state_outputs(system_state_t state)
{
    /* 防御性检查：状态值理论上不会越界，但多一层判断可避免踩内存 */
    if ((size_t)state >= STATE_COUNT) {
        ESP_LOGE(TAG, "未知状态值 %d，忽略输出更新", (int)state);
        return;
    }

    /* ---- 指示灯 ---- */
    status_led_set_pattern(s_state_to_led[state]);

    /* ---- 蜂鸣器 ----
     * 只有"异常（FAULT）"才报警；降级运行不发声，
     * 避免用户为了一个非关键外设故障被持续打扰（用指示灯区分即可）。 */
    if (is_buzzer_broken()) {
        ESP_LOGW(TAG, "蜂鸣器已登记故障，跳过发声");
        return;
    }

    esp_err_t err = buzzer_set_alarm(state == SYSTEM_STATE_FAULT);
    if (err != ESP_OK) {
        /* 这里用告警而不是错误：蜂鸣器是可选外设，
         * 初始化失败时不应把整个状态更新视为失败 */
        ESP_LOGW(TAG, "蜂鸣器报警设置失败: %s", esp_err_to_name(err));
    }
}

/**
 * @brief 系统状态变化事件的处理函数
 *
 * 重要：本函数由 event_bus 的"分发任务"调用，不是中断上下文。
 *       因此可以调用日志、驱动接口；但必须快速返回，
 *       否则会堵住后面所有事件的分发（见 event_bus.h 的说明）。
 *
 * @param event    事件内容（生命周期仅限本函数内，不要保存指针）
 * @param user_ctx 订阅时传入的上下文（本模块未使用）
 */
static void on_system_state_changed(const app_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    system_state_t state = (system_state_t)event->data.sys_state.state;

    char fault_text[96];
    system_state_fault_to_str(event->data.sys_state.fault_mask, fault_text, sizeof(fault_text));

    ESP_LOGI(TAG, "响应状态事件: %s（故障: %s）", system_state_name(state), fault_text);

    apply_state_outputs(state);
}

esp_err_t device_ctrl_init(void)
{
    /* ① 订阅系统状态变化事件 */
    esp_err_t err =
        event_bus_subscribe(APP_EVENT_SYSTEM_STATE_CHANGED, on_system_state_changed, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "订阅系统状态事件失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ② 立即同步一次当前状态。
     *    原因：system_state_init() 在订阅之前就已广播过一次 INIT 状态，
     *    如果只等事件，输出设备会停留在驱动初始化时的默认状态。
     *    主动同步一次，保证"设备表现与系统状态"从一开始就一致。 */
    system_state_t current = system_state_get();
    apply_state_outputs(current);

    /* ③ 恢复用户上次保存的蜂鸣器响度。
     *    为什么放在这里而不是 buzzer_init() 里：
     *    驱动的职责是"配置硬件"，而"恢复用户偏好"属于业务决策；
     *    并且这样安排后，将来新增其他需要恢复的用户设置，
     *    只需在本函数里继续追加，驱动的接口保持不变。 */
    settings_t cfg;
    if (settings_get(&cfg) == ESP_OK) {
        esp_err_t vol_err = buzzer_set_volume(cfg.buzzer_volume);
        if (vol_err == ESP_OK) {
            ESP_LOGI(TAG, "已恢复蜂鸣器响度: %u%%", cfg.buzzer_volume);
        } else {
            ESP_LOGW(TAG, "恢复蜂鸣器响度失败: %s", esp_err_to_name(vol_err));
        }
    }

    ESP_LOGI(TAG, "初始化完成，已按当前状态 %s 同步输出", system_state_name(current));
    return ESP_OK;
}

esp_err_t device_ctrl_set_buzzer_volume(uint8_t percent)
{
    /* ① 让新响度生效（从下一声开始，见 buzzer.h 的说明） */
    esp_err_t err = buzzer_set_volume(percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置蜂鸣器响度失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ② 更新配置缓存并安排延迟落盘。
     *    落盘失败不影响本次音量已生效，因此只记录告警，
     *    不向 UI 返回错误（否则用户会看到"调音量失败"的误报）。 */
    esp_err_t save_err = settings_set_buzzer_volume(percent);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "响度已生效，但配置缓存更新失败: %s", esp_err_to_name(save_err));
    }

    return ESP_OK;
}

esp_err_t device_ctrl_get_buzzer_volume(uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 以驱动的当前值为准（它才是真正在起作用的响度） */
    return buzzer_get_volume(percent);
}

esp_err_t device_ctrl_save_settings(void)
{
    return settings_save_now();
}

esp_err_t device_ctrl_notify_boot_ok(void)
{
    /* 蜂鸣器坏了就安静地跳过：这不是致命问题，不要打扰用户 */
    if (is_buzzer_broken()) {
        ESP_LOGW(TAG, "蜂鸣器已登记故障，跳过启动提示音");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = buzzer_play(BUZZER_TONE_OK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动提示音播放失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "已播放启动完成提示音");
    return ESP_OK;
}
