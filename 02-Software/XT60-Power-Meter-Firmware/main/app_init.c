/**
 * @file app_init.c
 * @brief 启动流程实现：初始化表 + 分级容错
 *
 * 阅读顺序建议：
 *   ① s_boot_steps[]     —— 一眼看清"启动了哪些模块、各自的重要程度"
 *   ② app_init_startup() —— 遍历表项，按故障位决定是"致命"还是"降级"
 */
#include "app_init.h"

#include <stdbool.h>

#include "buzzer.h"
#include "data_store.h"
#include "device_ctrl.h"
#include "event_bus.h"
#include "settings.h"
#include "status_led.h"
#include "system_state.h"

#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "app_init";

/**
 * @brief 一个初始化步骤
 *
 * 用结构体描述而不是把顺序写死在代码里，好处是：
 *   · 新增模块只加一行表项，不用改流程控制代码；
 *   · 每个模块的"重要程度"由它的 fault_bit 唯一决定
 *     （与 SYSTEM_FAULT_CRITICAL_MASK 比对），信息只存在一处。
 */
typedef struct {
    const char *name;        /**< 模块名，仅用于日志 */
    esp_err_t (*init)(void); /**< 初始化函数 */
    uint32_t fault_bit;      /**< 失败时记入的故障位 */
} init_step_t;

/**
 * 初始化表（按依赖顺序排列：先基础设施，再外设）
 *
 * 顺序上有两条硬约束：
 *   1. event_bus 必须最先——其他模块要靠它通信；
 *   2. status_led 必须早于 system_state / device_ctrl——
 *      状态机初始化时会广播状态，device_ctrl 收到后要立刻设置指示灯，
 *      所以指示灯驱动必须已经就绪。
 *
 * 待这些模块实现后，按同样格式追加到表里即可（顺序 = 依赖顺序）：
 *
 *     { "button",        button_init,        SYSTEM_FAULT_BUTTON  },
 *     { "battery_adc",   battery_adc_init,   SYSTEM_FAULT_BATTERY },
 *     { "aht20",         aht20_init,         SYSTEM_FAULT_ENV     },
 *     { "imu_sc7a20h",   imu_sc7a20h_init,   SYSTEM_FAULT_IMU     },
 *     { "ads131m02",     ads131m02_init,     SYSTEM_FAULT_ADC     }, ← 关键
 *     { "lcd_nv3030b",   lcd_nv3030b_init,   SYSTEM_FAULT_LCD     },
 *     { "touch_cst816d", touch_cst816d_init, SYSTEM_FAULT_TOUCH   },
 *     { "lvgl_port",     lvgl_port_init,     SYSTEM_FAULT_LCD     },
 *     { "wifi_manager",  wifi_manager_start, SYSTEM_FAULT_WIFI    },
 */
static const init_step_t s_boot_steps[] = {
    /* ---- 软件基础设施 ---- */
    /* 事件总线：整个框架的通信基础，坏了系统就没法工作 → 关键 */
    { "event_bus", event_bus_init, SYSTEM_FAULT_INFRA },
    /* 指示灯：坏了只是看不到指示，测量功能不受影响 → 非关键 */
    { "status_led", status_led_init, SYSTEM_FAULT_LED },
    /* 蜂鸣器：同上，属于可选的提示外设 → 非关键。
     * 注意位置：必须排在 device_ctrl 之前，因为 device_ctrl 初始化时
     * 会立即按当前状态同步一次输出（包含设置蜂鸣器报警），
     * 那时蜂鸣器必须已经就绪。 */
    { "buzzer", buzzer_init, SYSTEM_FAULT_BUZZER },
    { "system_state", system_state_init, SYSTEM_FAULT_INFRA },

    /* 配置持久化（NVS）：失败只是"记不住用户设置"，设备仍可正常测量，
     * 因此归为非关键。必须排在 device_ctrl 之前——device_ctrl 初始化时
     * 会读取已保存的配置来恢复蜂鸣器响度等用户偏好。 */
    { "settings", settings_init, SYSTEM_FAULT_STORAGE },

    /* 设备控制：负责把状态映射成指示灯与蜂鸣器动作 → 非关键 */
    { "device_ctrl", device_ctrl_init, SYSTEM_FAULT_LED },

    /* ---- 数据通道 ---- */
    /* data_store 是测量结果通往 UI / 网页的必经通道，出问题等于没有数据可看，
     * 故用 INFRA（关键）而不是 STORAGE —— STORAGE 已归属真正的存储模块
     * settings，两者语义不同，不应混用同一个故障位。 */
    { "data_store", data_store_init, SYSTEM_FAULT_INFRA },
};

/** 初始化步骤个数 */
#define BOOT_STEP_COUNT (sizeof(s_boot_steps) / sizeof(s_boot_steps[0]))

/**
 * @brief 判断某个步骤是否为关键模块
 *
 * 只需看它的故障位是否命中 SYSTEM_FAULT_CRITICAL_MASK。
 * 这样"关键与否"的定义集中在 system_state.h 一处，便于统一调整。
 */
static inline bool is_critical_step(const init_step_t *step)
{
    return (step->fault_bit & SYSTEM_FAULT_CRITICAL_MASK) != 0;
}

esp_err_t app_init_startup(void)
{
    bool critical_failed = false;

    ESP_LOGI(TAG, "开始系统初始化（共 %u 个步骤）", (unsigned)BOOT_STEP_COUNT);

    for (size_t i = 0; i < BOOT_STEP_COUNT; i++) {
        const init_step_t *step = &s_boot_steps[i];

        /* 已经出现过致命故障时，跳过后续"关键"步骤：
         * 它们都依赖基础设施，继续尝试没有意义，还可能产生二次错误
         * 掩盖真正的原因。但"非关键"步骤仍然继续，尽量保留可用功能
         * 与潜在的救回通道（后续的 WiFi / 屏幕）。 */
        if (critical_failed && is_critical_step(step)) {
            ESP_LOGW(TAG, "跳过关键模块 %s（此前已发生致命故障）", step->name);
            continue;
        }

        esp_err_t err = step->init();

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[%u/%u] %s 初始化成功", (unsigned)(i + 1), (unsigned)BOOT_STEP_COUNT,
                     step->name);
            continue;
        }

        if (is_critical_step(step)) {
            /* ---- 关键模块失败 ----
             * 尝试登记故障位。若失败发生在 system_state 之前，这一步会返回
             * ESP_ERR_INVALID_STATE，属预期内情况，故忽略其返回值——
             * 真正的原因由下面的错误日志与 main.c 的汇总日志体现。 */
            ESP_LOGE(TAG, "[%u/%u] 关键模块 %s 初始化失败: %s", (unsigned)(i + 1),
                     (unsigned)BOOT_STEP_COUNT, step->name, esp_err_to_name(err));
            critical_failed = true;
            (void)system_state_report_fault(step->fault_bit);
        } else {
            /* ---- 非关键模块失败：登记故障位，降级继续 ---- */
            ESP_LOGW(TAG, "[%u/%u] 模块 %s 初始化失败: %s -> 系统降级运行", (unsigned)(i + 1),
                     (unsigned)BOOT_STEP_COUNT, step->name, esp_err_to_name(err));
            (void)system_state_report_fault(step->fault_bit);
        }
    }

    if (critical_failed) {
        /* 刻意不返回 ESP_OK：让 main.c 知道启动未完成。
         * 同时也不 abort()——设备必须活着，这样才能通过指示灯 / 串口 / 网页
         * 反映故障，并保留远程升级救回的可能。 */
        return ESP_FAIL;
    }

    /* 全部关键步骤通过：把状态从 INIT 推进到 RUNNING（或 DEGRADED） */
    esp_err_t err = system_state_start_running();
    if (err != ESP_OK) {
        return err;
    }

    /* 启动通过后短鸣一声，给用户一个"设备已就绪"的听觉反馈。
     *
     * 为什么放在这里而不是 buzzer_init() 里：
     *   这个声音的含义是"整个启动流程完成"，属于系统级行为；
     *   塞进驱动初始化会产生难以预期的副作用。
     * 注意：若启动结果是 DEGRADED（有非关键外设故障），这里同样会响一声
     *   ——它表示"启动流程走完了"，具体哪个外设有问题由指示灯（双闪）
     *   与串口日志体现。
     * 失败不影响启动结果，因为蜂鸣器是可选外设。 */
    (void)device_ctrl_notify_boot_ok();

    return ESP_OK;
}
