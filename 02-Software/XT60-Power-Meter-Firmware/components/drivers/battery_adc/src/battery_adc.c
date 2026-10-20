/**
 * @file battery_adc.c
 * @brief 电池电压采样驱动实现（ADC1 单次采样 + 校准 + 均值滤波）
 *
 * 器件：ESP32-S3 内部 SAR ADC（ADC1），采样引脚 IO2（BOARD_PIN_BAT_ADC）。
 * 电路：VBAT -> R32 100kΩ -> IO2 -> R38 100kΩ -> GND，1:1 分压，
 *      因此实际电池电压 = 引脚电压 × BOARD_BAT_ADC_DIVIDER_RATIO(=2)。
 *
 * 阅读顺序建议：
 *   ① battery_adc_init()  —— 建 ADC1 单次采样通道 + 尝试建立校准；
 *   ② battery_adc_read()  —— 多次采样取均值 -> 转毫伏 -> 乘分压比。
 *
 * 用官方 esp_adc 组件（adc_oneshot + adc_cali）：
 *   · 单次采样模式（oneshot）足够，电池电压变化慢，无需连续采样/DMA；
 *   · 校准（curve/line fitting）用于消除 ADC 参考电压与增益误差，
 *     提升绝对精度。校准句柄创建失败不致命——退回「原始码值固定换算」，
 *     精度略降但功能可用（电池电量仅作指示，非精确测量）。
 *
 * ============================================================
 * 上板调试要点（读数偏高/偏低/乱跳时排查）
 * ============================================================
 *   · 读数恒为固定值（如 0 或满量程）：核对 IO2 是否被别的模块占用、
 *     分压电阻 R32/R38 是否虚焊。
 *   · 读数整体偏大/偏小固定比例：多为分压电阻实际误差，调 BATTERY_ADC_CAL
 *     标定系数（测一次真实电压反推）。
 *   · 读数有噪声抖动：增大 BATTERY_ADC_SAMPLE_COUNT 均值次数，
 *     或在读取前确保大电流负载稳定（电池内阻压降）。
 *   · 衰减档位：选 ADC_ATTEN_DB_12（量程约 0~3100mV）。引脚最高约
 *     2.1V（单节锂电 4.2V 分压后），留有余量；若电池规格不同需复核。
 * ============================================================
 */
#include "battery_adc.h"

#include "board_pins.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "battery_adc";

/* 采样通道：IO2 = ADC1 通道 1（ESP32-S3 引脚到 ADC 通道映射固定） */
#define BAT_ADC_UNIT      ADC_UNIT_1
#define BAT_ADC_CHANNEL   ADC_CHANNEL_1
#define BAT_ADC_ATTEN     ADC_ATTEN_DB_12 /* 量程约 0~3100mV，覆盖分压后最高约 2.1V */
#define BAT_ADC_BITWIDTH  ADC_BITWIDTH_DEFAULT

/** 均值采样次数：抵消 SAR ADC 量化噪声，取 2 的幂便于观察 */
#define BATTERY_ADC_SAMPLE_COUNT 16

/**
 * 分压/元件误差标定系数（无量纲）。
 * 理论分压比为 2；实测若整体偏大/偏小固定比例，按「真实电压 / 读数电压」
 * 微调本值（例如实测偏大 3% -> 乘 0.97）。默认 1.0 = 不校正。
 */
#define BATTERY_ADC_CAL 1.0f

/** 是否已初始化 */
static bool s_initialized = false;

/* ADC 句柄 */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
/* 校准句柄（可为 NULL，表示未校准） */
static adc_cali_handle_t s_cali_handle = NULL;
/** 是否建立了有效校准 */
static bool s_calibrated = false;

/**
 * @brief 尝试建立 ADC 校准方案
 *
 * 优先曲线拟合（curve fitting），ESP32-S3 支持；若编译目标不支持则不校准。
 * 建立失败仅记录警告，不影响驱动可用性。
 */
static void try_create_cali(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (err == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC 校准已启用（curve fitting）");
    } else {
        s_cali_handle = NULL;
        ESP_LOGW(TAG, "ADC 校准建立失败: %s -> 退回固定换算", esp_err_to_name(err));
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    esp_err_t err = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali_handle);
    if (err == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC 校准已启用（line fitting）");
    } else {
        s_cali_handle = NULL;
        ESP_LOGW(TAG, "ADC 校准建立失败: %s -> 退回固定换算", esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "当前目标不支持 ADC 校准方案 -> 使用固定换算");
#endif
}

esp_err_t battery_adc_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- ① 建 ADC1 单次采样单元 ---- */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 单元创建失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ② 配置采样通道（衰减档 + 位宽） ---- */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    /* ---- ③ 尝试建立校准（非致命） ---- */
    try_create_cali();

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成（ADC%d_CH%d on IO%d，衰减 %d，校准=%s）", BAT_ADC_UNIT + 1,
             BAT_ADC_CHANNEL, BOARD_PIN_BAT_ADC, BAT_ADC_ATTEN, s_calibrated ? "是" : "否");
    return ESP_OK;
}

esp_err_t battery_adc_read(uint32_t *voltage_mv)
{
    if (voltage_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 多次采样累加取均值，降低量化噪声 */
    int64_t raw_sum = 0;
    for (int i = 0; i < BATTERY_ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            return err;
        }
        raw_sum += raw;
    }
    int raw_avg = (int)(raw_sum / BATTERY_ADC_SAMPLE_COUNT);

    /* 原始码值 -> 引脚毫伏 */
    int pin_mv = 0;
    if (s_calibrated) {
        esp_err_t err = adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &pin_mv);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        /* 无校准时的近似固定换算：满量程 4095 对应约 3100mV（DB_12 档）。
         * 仅作粗略指示，绝对精度以校准为准。 */
        pin_mv = (int)((int64_t)raw_avg * 3100 / 4095);
    }

    /* 引脚电压 -> 电池实际电压（乘分压比 + 标定系数） */
    float batt_mv = (float)pin_mv * BOARD_BAT_ADC_DIVIDER_RATIO * BATTERY_ADC_CAL;
    *voltage_mv = (uint32_t)(batt_mv + 0.5f);

    return ESP_OK;
}
