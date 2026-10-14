/**
 * @file app_init.c
 * @brief 启动流程实现：初始化表 + 硬件自检 + 分级容错
 *
 * 阅读顺序建议：
 *   ① s_boot_steps[]     —— 一眼看清"启动了哪些模块、各自的重要程度"；
 *   ② run_selftest()     —— 启动表跑完后"真读一次数据"的硬件自检；
 *   ③ app_init_startup() —— 依次执行两者，按故障位决定"致命 / 降级 / 进入运行态"。
 *
 * 启动分成两个阶段，缺一不可：
 *   阶段一（初始化表）：把各模块配起来；
 *   阶段二（硬件自检）：确认真能读到数据——"配置成功"不等于"器件可用"。
 * 只有两阶段都通过才进入运行态；自检全通过时才短鸣一声（见 app_init_startup）。
 */
#include "app_init.h"

#include <stdbool.h>

#include "ads131m02.h"
#include "aht20.h"
#include "battery_adc.h"
#include "button.h"
#include "buzzer.h"
#include "data_store.h"
#include "device_ctrl.h"
#include "event_bus.h"
#include "fs_storage.h"
#include "imu_sc7a20h.h"
#include "lcd_nv3030b.h"
#include "lvgl_port.h"
#include "orientation.h"
#include "power_measure.h"
#include "settings.h"
#include "status_led.h"
#include "system_state.h"
#include "touch_cst816d.h"

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
 * 另外两条来自"先有数据、再有展示"的自然依赖：
 *   3. 传感器 / 测量前端的初始化要早于显示与交互（界面只是把 data_store
 *      的数据显示出来，先把数据链路准备好）；
 *   4. ads131m02 是本设备的核心测量前端，用 SYSTEM_FAULT_ADC（关键位），
 *      其余外设（按键 / 电池 / 温湿度 / 加速度计）失败只降级，不进 FAULT。
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

    /* ---- 传感器与测量前端 ----
     * 这一组彼此之间没有依赖，排在数据通道之后、显示之前：
     * 先把"数据怎么来"准备好，再启动"数据怎么显示"。 */
    /* 按键（BOOT/USER）：坏了只是不能操作，测量不受影响 → 非关键 */
    { "button", button_init, SYSTEM_FAULT_BUTTON },
    /* 电池电压采样（ADC1 + 校准）：坏了只是看不到电量 → 非关键 */
    { "battery_adc", battery_adc_init, SYSTEM_FAULT_BATTERY },
    /* 温湿度 AHT20（I2C1，与 IMU 共享总线）：坏了只是少一项环境读数 → 非关键 */
    { "aht20", aht20_init, SYSTEM_FAULT_ENV },
    /* 加速度计 SC7A20H（I2C1，与 AHT20 共享总线）：坏了只是失去姿态判断 → 非关键 */
    { "imu_sc7a20h", imu_sc7a20h_init, SYSTEM_FAULT_IMU },
    /* 测量前端 ADC（SPI3 独占）：整个设备的核心，坏了就没有测量数据，
     * 因此用 SYSTEM_FAULT_ADC —— 该位在 SYSTEM_FAULT_CRITICAL_MASK 中 → 关键 */
    { "ads131m02", ads131m02_init, SYSTEM_FAULT_ADC },
    /* 测量服务：载入 NVS 电能/标定、分配 PSRAM 波形缓冲（不启动任务，
     * 任务在 app_tasks_start 中于 Core1 启动）。它不可用 = 无法测量，
     * 与 ADC 同属关键位。 */
    { "power_measure", power_measure_init, SYSTEM_FAULT_ADC },

    /* ---- 显示与交互 ---- */
    /* 资源文件系统：storage 分区挂为 /spiffs（UI 字体在上面）。
     * 挂载失败不阻止显示（lvgl_port 会回退内置 ASCII 字体），非关键。
     * 必须早于 lvgl_port——字体在 lvgl_port_init() 中从文件加载。 */
    { "fs_storage", fs_storage_init, SYSTEM_FAULT_STORAGE },
    /* LCD 面板（NV3030B，QSPI）：坏了只是"没有显示"，测量不受影响 → 非关键。
     * 须先于 lvgl_port——LVGL 适配层要注册显示设备。 */
    { "lcd_nv3030b", lcd_nv3030b_init, SYSTEM_FAULT_LCD },
    /* 电容触摸（CST816D）：坏了只是"不能触控"，界面仍可显示 → 非关键。
     * 触摸故障不阻止 UI 启动（见 app_tasks_start 的门控）。 */
    { "touch_cst816d", touch_cst816d_init, SYSTEM_FAULT_TOUCH },
    /* LVGL 适配层：注册显示/输入设备并启动心跳。失败则 UI 无法工作，
     * 故与 LCD 共用 SYSTEM_FAULT_LCD 故障位，UI 启动门控据此判断。 */
    { "lvgl_port", lvgl_port_app_init, SYSTEM_FAULT_LCD },

    /* 重力自动旋转：IMU/LVGL 就绪后启动方向监测，并按 NVS 恢复方向模式。
     * 非关键——失败只影响自动转屏，手动角度仍可用。 */
    { "orientation", orientation_boot_restore, 0 },
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

/* ============================================================
 * 阶段二：硬件自检
 *
 * 为什么初始化成功还不够：
 *   「init 返回 OK」只代表寄存器配置写下去了，不代表器件真能出数据。
 *   典型反例：I2C 器件地址对但时序不对（能 ACK、读不出有效数据）；
 *   SPI ADC 的输入时钟缺失（配置全成功，但 DRDY 永远不来）。
 *   所以在启动表跑完后再做一轮「真读一次数据」的探测，把问题在启动阶段
 *   就暴露到日志里，而不是等到界面上显示不出数值才发现。
 *
 * 判定与门控（与既有分级容错保持一致）：
 *   · 关键项（故障位命中 SYSTEM_FAULT_CRITICAL_MASK，本工程只有 ads131m02）
 *     失败 → 登记故障位，**不进入运行态**（不短鸣，状态保持 FAULT）；
 *   · 非关键项失败 → 登记故障位，仍进入运行态（状态机推导为 DEGRADED），
 *     但**不短鸣**——短鸣的语义是「一切就绪」，降级时鸣一声会误导用户。
 *
 * 为什么按键不在自检表里：
 *   按键驱动只提供「事件回调」，没有可供查询的读取接口（回调只在用户
 *   真正按键时才触发），无法在启动时探测。它的可用性由 app_tasks 的
 *   巡检日志与用户实际操作体现。
 * ============================================================ */
typedef struct {
    const char *name;         /**< 自检项名，仅用于日志 */
    esp_err_t (*probe)(void); /**< 探测函数：真读到数据才算通过 */
    uint32_t fault_bit;       /**< 失败时记入的故障位 */
} selftest_item_t;

/** AHT20 合理量程：读到但明显越界说明数据不可信，同样判失败 */
#define SELFTEST_HUM_MIN_PCT  0.0f
#define SELFTEST_HUM_MAX_PCT  100.0f
#define SELFTEST_TEMP_MIN_C   (-40.0f)
#define SELFTEST_TEMP_MAX_C   85.0f

/**
 * @brief 自检：测量前端 ADC 能读回一帧数据
 *
 * 这是本设备唯一的测量来源，读不到就等于设备失去核心功能。
 */
static esp_err_t selftest_ads131m02(void)
{
    ads131m02_sample_t sample;
    return ads131m02_read(&sample);
}

/**
 * @brief 自检：温湿度能读回，且读数落在合理量程内
 */
static esp_err_t selftest_aht20(void)
{
    aht20_reading_t reading;
    esp_err_t err = aht20_read(&reading);
    if (err != ESP_OK) {
        return err;
    }
    if (reading.humidity_pct < SELFTEST_HUM_MIN_PCT || reading.humidity_pct > SELFTEST_HUM_MAX_PCT ||
        reading.temperature_c < SELFTEST_TEMP_MIN_C || reading.temperature_c > SELFTEST_TEMP_MAX_C) {
        ESP_LOGW(TAG, "aht20 读数越界: %.1f%%RH / %.1f'C", reading.humidity_pct,
                 reading.temperature_c);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/** @brief 自检：加速度计能读回朝向 */
static esp_err_t selftest_imu_sc7a20h(void)
{
    imu_sc7a20h_orientation_t orientation;
    return imu_sc7a20h_get_orientation(&orientation);
}

/** @brief 自检：电池电压能读回 */
static esp_err_t selftest_battery_adc(void)
{
    uint32_t voltage_mv = 0;
    return battery_adc_read(&voltage_mv);
}

/**
 * @brief 自检：触摸控制器能应答
 *
 * 注意：用户此时不会碰屏幕，所以「未按下」是正常结果。
 * 只有 I2C 层面失败（器件不应答）才算自检失败。
 */
static esp_err_t selftest_touch_cst816d(void)
{
    touch_cst816d_point_t point;
    return touch_cst816d_read(&point);
}

/** 自检表（顺序 = 报告顺序，关键项排在首位便于优先看到） */
static const selftest_item_t s_selftest_items[] = {
    { "ads131m02", selftest_ads131m02, SYSTEM_FAULT_ADC }, /* 关键：测量前端 */
    { "aht20", selftest_aht20, SYSTEM_FAULT_ENV },
    { "imu_sc7a20h", selftest_imu_sc7a20h, SYSTEM_FAULT_IMU },
    { "battery_adc", selftest_battery_adc, SYSTEM_FAULT_BATTERY },
    { "touch_cst816d", selftest_touch_cst816d, SYSTEM_FAULT_TOUCH },
};

/** 自检项个数 */
#define SELFTEST_COUNT (sizeof(s_selftest_items) / sizeof(s_selftest_items[0]))

/**
 * @brief 逐项执行硬件自检
 *
 * @param critical_failed 输出：是否有「关键项」失败（不可为 NULL）
 * @return true = 全部通过；false = 有任一项失败（失败项已登记故障位）
 */
static bool run_selftest(bool *critical_failed)
{
    size_t failed = 0;
    bool any_critical_failed = false;

    ESP_LOGI(TAG, "开始硬件自检（共 %u 项）", (unsigned)SELFTEST_COUNT);

    for (size_t i = 0; i < SELFTEST_COUNT; i++) {
        const selftest_item_t *item = &s_selftest_items[i];
        const bool critical = (item->fault_bit & SYSTEM_FAULT_CRITICAL_MASK) != 0;

        /* 初始化阶段已登记过该模块故障 → 驱动未就绪，再探测只会重复报同一个错，
         * 徒增干扰，故跳过并把该项计为失败。 */
        if ((system_state_get_fault_mask() & item->fault_bit) != 0) {
            ESP_LOGW(TAG, "[自检 %u/%u] %s 跳过（初始化阶段已失败）", (unsigned)(i + 1),
                     (unsigned)SELFTEST_COUNT, item->name);
            failed++;
            any_critical_failed = any_critical_failed || critical;
            continue;
        }

        esp_err_t err = item->probe();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[自检 %u/%u] %s 通过%s", (unsigned)(i + 1), (unsigned)SELFTEST_COUNT,
                     item->name, critical ? "（关键）" : "");
            continue;
        }

        failed++;
        any_critical_failed = any_critical_failed || critical;
        (void)system_state_report_fault(item->fault_bit);
        if (critical) {
            ESP_LOGE(TAG, "[自检 %u/%u] %s 失败: %s（关键项 -> 不进入运行态）", (unsigned)(i + 1),
                     (unsigned)SELFTEST_COUNT, item->name, esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "[自检 %u/%u] %s 失败: %s -> 该功能不可用，系统降级运行",
                     (unsigned)(i + 1), (unsigned)SELFTEST_COUNT, item->name, esp_err_to_name(err));
        }
    }

    *critical_failed = any_critical_failed;

    if (failed == 0) {
        ESP_LOGI(TAG, "硬件自检全部通过");
        return true;
    }
    ESP_LOGW(TAG, "硬件自检有 %u/%u 项未通过", (unsigned)failed, (unsigned)SELFTEST_COUNT);
    return false;
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

    /* ---------- 阶段二：硬件自检 ----------
     * 初始化表跑完只说明"配置写下去了"，这里再"真读一次数据"确认器件可用。
     * 详见 run_selftest() 上方说明。 */
    bool selftest_critical_failed = false;
    bool selftest_all_passed = run_selftest(&selftest_critical_failed);

    if (critical_failed || selftest_critical_failed) {
        /* 刻意不返回 ESP_OK：让 main.c 知道启动未完成。
         * 同时也不 abort()——设备必须活着，这样才能通过指示灯 / 串口 / 网页
         * 反映故障，并保留远程升级救回的可能。 */
        return ESP_FAIL;
    }

    /* 关键项全部通过：把状态从 INIT 推进到 RUNNING（或 DEGRADED）。
     * DEGRADED 的判定由状态机按故障掩码自行推导，这里不需要区分。 */
    esp_err_t err = system_state_start_running();
    if (err != ESP_OK) {
        return err;
    }

    /* 只有「初始化全部成功 + 自检全部通过」才短鸣一声，给用户一个
     * "设备已就绪"的听觉反馈。
     *
     * 为什么放这里而不是 buzzer_init() 里：
     *   这个声音的语义是"整个启动流程走完且一切就绪"，属于系统级行为；
     *   塞进驱动初始化会产生难以预期的副作用。
     * 为什么降级时不鸣：
     *   有非关键外设故障时状态是 DEGRADED，此时鸣"就绪音"会误导用户；
     *   降级的具体项由上面的自检日志与指示灯（双闪）体现。
     * 短鸣失败不影响启动结果，因为蜂鸣器是可选外设。 */
    if (selftest_all_passed) {
        (void)device_ctrl_notify_boot_ok();
    } else {
        ESP_LOGW(TAG, "存在降级项，跳过启动短鸣（详见上面的自检结果）");
    }

    return ESP_OK;
}
