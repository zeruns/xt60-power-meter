/**
 * @file power_measure.c
 * @brief 功率测量服务实现（4kHz 采样 / 标定 / 直流均值 / 三级波形缓冲）
 *
 * ────────────────────────────────────────────────────────────
 * 本设备测的是「直流」电压/电流（USB-PD / 电池供电场景），因此：
 *   · 电压、电流取**块内平均值**（直流分量），而不是真有效值 RMS。
 *     RMS ≥ |均值|，对带噪声的直流量用 RMS 会系统性偏大；示波类直流表
 *     显示的也是平均值。
 *   · 功率用 mean(V·I)（真实平均功率），在直流下与 mean(V)×mean(I) 一致，
 *     噪声相关性不为零时前者更准。
 * ────────────────────────────────────────────────────────────
 * 帧有效性校验（关键）：
 *   ADS131M02 每帧的 STATUS 字里 DRDY0/DRDY1（bit0/bit1）表示对应通道有新数据。
 *   只有「DRDY 下降沿之后读到的第一帧」这两个位才是 1。若读到 0，说明帧边界
 *   漂移/读到了陈旧帧，此时 CH0/CH1 的位置也不可信（表现为读数乱跳、出现
 *   与真实值无关的大幅摆动）。因此丢弃这类帧，并统计丢弃率用于诊断。
 *   若丢弃率异常高（>50%），说明该判据本身不可用，自动降级为"不过滤"并告警，
 *   避免界面因此完全不更新。
 *
 * 波形缓冲（PSRAM，三级降采样）：
 *   tier0  4000Hz × 4000 点  —— 1s 窗（原始逐点，已过 4 点滑动平均轻滤）
 *   tier1   400Hz × 24000 点 —— 60s 窗（每 10 点均值）
 *   tier2    10Hz × 6000 点  —— 10min 窗（每 40 个 tier1 点均值）
 */
#include "power_measure.h"

#include <math.h>
#include <string.h>

#include "ads131m02.h"
#include "aht20.h"
#include "battery_adc.h"
#include "data_store.h"
#include "power_calc.h"
#include "system_state.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>

#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "power_measure";

/* ============================================================
 * 采样与计算参数
 * ============================================================ */
#define PM_SAMPLE_RATE_HZ  4000u                 /**< ADC 输出数据率（与驱动配置一致） */
#define PM_BLOCK_SAMPLES   800u                  /**< 标称 4kHz 下 200ms 的样本数（仅作历史参考） */
#define PM_BLOCK_PERIOD_MS (1000u * PM_BLOCK_SAMPLES / PM_SAMPLE_RATE_HZ) /**< 块周期：墙钟 200ms */
#define PM_BLOCK_PER_SEC   (PM_SAMPLE_RATE_HZ / PM_BLOCK_SAMPLES) /**< 每秒块数 = 5 */

/** 显示平滑系数（对块均值做一阶低通）：约 0.8s 时间常数，兼顾响应与稳定 */
#define PM_DISPLAY_EMA_ALPHA 0.25f
/** 波形轻滤：4 点滑动平均（4kHz → 1kHz 等效带宽），抑制高频噪声 */
#define PM_WAVE_MAVG_N 4u

/** 前端默认标定（未实测标定时的理论值，见 power_calc.h 的量纲说明）
 *  电压：满量程 ±1.2V@PGA1× / 2^23 码，外部分压 101:1（R33 100k / R39 1k）
 *        → 1.2/8388608×101 = 1.445e-05 V/LSB */
#define PM_DEFAULT_V_GAIN  (1.2f / 8388608.0f * 101.0f)
/** 电流：满量程 ±1.2V / PGA16× / 2^23 码，分流器 1mΩ 四端开尔文
 *        → (1.2/16)/8388608/0.001 = 8.941e-06 A/LSB */
#define PM_DEFAULT_I_GAIN  ((1.2f / 16.0f) / 8388608.0f / 0.001f)

/* ---- 波形三级缓冲 ---- */
typedef struct {
    float v; /**< 电压 V */
    float i; /**< 电流 A */
    float p; /**< 功率 W */
    float e; /**< 电能 Wh（该点时刻的累计值） */
} wave_point_t;

#define PM_TIER0_CAP 4000u   /**< 4000Hz × 1s */
#define PM_TIER1_CAP 24000u  /**< 400Hz × 60s */
#define PM_TIER2_CAP 6000u   /**< 10Hz × 600s（10min） */
#define PM_TIER1_DECIM 10u   /**< tier0→tier1：10 点均值（4000/10=400Hz） */
#define PM_TIER2_DECIM 40u   /**< tier1→tier2：40 点均值（400/40=10Hz） */

typedef struct {
    uint8_t tier;
    uint32_t points;
} window_map_t;
static const window_map_t s_window_map[PM_WAVE_WINDOW_MAX] = {
    [PM_WAVE_1S]    = { 0, PM_TIER0_CAP },
    [PM_WAVE_10S]   = { 1, 4000u },
    [PM_WAVE_30S]   = { 1, 12000u },
    [PM_WAVE_60S]   = { 1, PM_TIER1_CAP },
    [PM_WAVE_120S]  = { 2, 1200u },
    [PM_WAVE_10MIN] = { 2, PM_TIER2_CAP },
};

/* ---- NVS 持久化 ---- */
#define PM_NVS_NS "pm"
#define PM_NVS_KEY_DATA "pmdat"
#define PM_NVS_MAGIC 0x504D3031u /**< 'PM01' */

typedef struct {
    uint32_t magic;
    float energy_wh;
    uint32_t runtime_s;
} pm_persist_t;

/* ============================================================
 * 模块内部状态
 * ============================================================ */
static bool s_inited = false;
static bool s_started = false;

static TaskHandle_t s_task_sample = NULL;
static TaskHandle_t s_task_aux = NULL;

static power_measure_calib_t s_calib;
static SemaphoreHandle_t s_calib_mutex = NULL;

static pm_persist_t s_persist;

/** 电能清零请求（UI 核 → 测量核）：由采样任务在块边界消费，保持电能值单写者 */
static atomic_bool s_reset_energy_req = false;

static struct {
    SemaphoreHandle_t mutex;
    float temp_c;
    float humidity_pct;
    uint32_t battery_mv;
    bool temp_valid;
} s_aux_data;

typedef struct {
    wave_point_t *buf;
    uint32_t cap;
    uint32_t head;
    uint32_t count;
} ring_t;

static ring_t s_tier[3];
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_tier1_acc_n = 0;
static wave_point_t s_tier1_acc;
static uint32_t s_tier2_acc_n = 0;
static wave_point_t s_tier2_acc;

static void ring_init(ring_t *r, uint32_t cap)
{
    r->buf = NULL;
    r->cap = 0;
    r->head = 0;
    r->count = 0;
}

static void ring_push(ring_t *r, const wave_point_t *p)
{
    if (r->buf == NULL) {
        return;
    }
    r->buf[r->head] = *p;
    r->head = (r->head + 1) % r->cap;
    if (r->count < r->cap) {
        r->count++;
    }
}

/**
 * @brief 从环中读取最旧的 n 个点、按 stride 抽稀映射到 a/b 序列
 * @note 调用方持有 s_ring_lock
 */
static int ring_copy_locked(const ring_t *r, uint32_t n, int stride,
                            int field_a, int field_b, bool has_b,
                            float *a, float *b, int max_points)
{
    if (n == 0 || r->buf == NULL) {
        return 0;
    }
    if (n > r->count) {
        n = r->count;
    }
    int out_n = (int)((n + (uint32_t)stride - 1u) / (uint32_t)stride);
    if (out_n > max_points) {
        out_n = max_points;
    }
    uint32_t start = (r->head + r->cap - n) % r->cap;

    for (int k = 0; k < out_n; k++) {
        uint32_t idx = (start + (uint32_t)k * (uint32_t)stride) % r->cap;
        const wave_point_t *p = &r->buf[idx];
        switch (field_a) {
        case 0: a[k] = p->v; break;
        case 1: a[k] = p->i; break;
        case 2: a[k] = p->p; break;
        default: a[k] = p->e; break;
        }
        if (has_b && b != NULL) {
            switch (field_b) {
            case 0: b[k] = p->v; break;
            case 1: b[k] = p->i; break;
            case 2: b[k] = p->p; break;
            default: b[k] = p->e; break;
            }
        }
    }
    return out_n;
}

/** 4 点滑动平均的历史值（仅采样线程访问） */
static float s_mavg_v[PM_WAVE_MAVG_N];
static float s_mavg_i[PM_WAVE_MAVG_N];
static uint32_t s_mavg_idx = 0;

static void wave_push_sample(float v, float i)
{
    /* 轻滤：4 点滑动平均，抑制高频噪声使波形可读 */
    s_mavg_v[s_mavg_idx] = v;
    s_mavg_i[s_mavg_idx] = i;
    s_mavg_idx = (s_mavg_idx + 1u) % PM_WAVE_MAVG_N;

    float sv = 0.0f, si = 0.0f;
    for (uint32_t k = 0; k < PM_WAVE_MAVG_N; k++) {
        sv += s_mavg_v[k];
        si += s_mavg_i[k];
    }
    float fv = sv / (float)PM_WAVE_MAVG_N;
    float fi = si / (float)PM_WAVE_MAVG_N;

    portENTER_CRITICAL(&s_ring_lock);

    wave_point_t pt = { .v = fv, .i = fi, .p = fv * fi, .e = s_persist.energy_wh };
    ring_push(&s_tier[0], &pt);

    s_tier1_acc.v += fv;
    s_tier1_acc.i += fi;
    s_tier1_acc.p += pt.p;
    s_tier1_acc_n++;
    if (s_tier1_acc_n >= PM_TIER1_DECIM) {
        wave_point_t d = {
            .v = s_tier1_acc.v / (float)s_tier1_acc_n,
            .i = s_tier1_acc.i / (float)s_tier1_acc_n,
            .p = s_tier1_acc.p / (float)s_tier1_acc_n,
            .e = s_persist.energy_wh,
        };
        ring_push(&s_tier[1], &d);
        s_tier1_acc = (wave_point_t){ 0 };
        s_tier1_acc_n = 0;

        s_tier2_acc.v += d.v;
        s_tier2_acc.i += d.i;
        s_tier2_acc.p += d.p;
        s_tier2_acc_n++;
        if (s_tier2_acc_n >= PM_TIER2_DECIM) {
            wave_point_t d2 = {
                .v = s_tier2_acc.v / (float)s_tier2_acc_n,
                .i = s_tier2_acc.i / (float)s_tier2_acc_n,
                .p = s_tier2_acc.p / (float)s_tier2_acc_n,
                .e = s_persist.energy_wh,
            };
            ring_push(&s_tier[2], &d2);
            s_tier2_acc = (wave_point_t){ 0 };
            s_tier2_acc_n = 0;
        }
    }

    portEXIT_CRITICAL(&s_ring_lock);
}

/* ============================================================
 * NVS 持久化
 * ============================================================ */
static void load_persist_and_calib(void)
{
    s_persist.magic = PM_NVS_MAGIC;
    s_persist.energy_wh = 0.0f;
    s_persist.runtime_s = 0;

    s_calib.voltage_gain = PM_DEFAULT_V_GAIN;
    s_calib.voltage_offset = 0.0f;
    s_calib.current_gain = PM_DEFAULT_I_GAIN;
    s_calib.current_offset = 0.0f;

    nvs_handle_t h;
    if (nvs_open(PM_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 无历史数据，电能/运行时间从 0 开始，使用默认理论标定");
        return;
    }

    pm_persist_t blob;
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, PM_NVS_KEY_DATA, &blob, &len) == ESP_OK && len == sizeof(blob) &&
        blob.magic == PM_NVS_MAGIC) {
        s_persist.energy_wh = blob.energy_wh;
        s_persist.runtime_s = blob.runtime_s;
        ESP_LOGI(TAG, "已恢复电能 %.6f Wh / 运行时间 %u s", blob.energy_wh,
                 (unsigned)blob.runtime_s);
    }

    size_t clen = sizeof(s_calib);
    if (nvs_get_blob(h, "calib", &s_calib, &clen) == ESP_OK && clen == sizeof(s_calib)) {
        ESP_LOGI(TAG, "已加载标定（V gain=%.6e off=%.6f / I gain=%.6e off=%.6f）",
                 (double)s_calib.voltage_gain, (double)s_calib.voltage_offset,
                 (double)s_calib.current_gain, (double)s_calib.current_offset);
    }
    nvs_close(h);
}

static void save_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(PM_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败，本次电能/运行时间未落盘");
        return;
    }
    esp_err_t e1 = nvs_set_blob(h, PM_NVS_KEY_DATA, &s_persist, sizeof(s_persist));
    esp_err_t e2 = nvs_set_blob(h, "calib", &s_calib, sizeof(s_calib));
    if (e1 == ESP_OK && e2 == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ============================================================
 * 采样任务（Core1，4kHz，DRDY 自节奏）
 * ============================================================ */
#define PM_SAMPLE_STACK   4096
#define PM_SAMPLE_PRIO    22
#define PM_AUX_STACK      6144
#define PM_AUX_PRIO       5
#define PM_AUX_PERIOD_MS  2000
#define PM_SAVE_PERIOD_TICKS 15u /* 辅助任务每 15 拍（30s）落一次盘 */
#define PM_ADC_FAULT_AFTER_FAILS 2000u
#define PM_DIAG_PERIOD_BLOCKS 25u /* 每 25 块（5s）打印一次诊断 */

/* 快速显示子窗口周期（微秒）：**墙钟时间驱动**，与实际 ODR 解耦。
 * 2026-10 实测：本板 ADC 的 MCLK 异常，DRDY 实际只有 ~320Hz（标称 4kHz，
 * drdy 计数 317~326Hz 且 ±3% 抖动，晶振时基不应有此抖动——硬件时钟问题）。
 * 原方案"每 100 个有效样本开一窗"在 320Hz 下被拖到 ~2.6Hz（即用户所见
 * "首页刷新率低/数据不动"的根源）。改为每 25ms 强制输出一帧即时均值：
 * 4kHz 时约 100 样本/帧、320Hz 时约 8 样本/帧，UI 恒定 40Hz 有数据。 */
#define PM_DISPLAY_SUBWINDOW_US 25000ull

static void task_sample(void *arg)
{
    (void)arg;

    power_calc_channel_calib_t vcal = { 0 }, ical = { 0 };
    double sum_v = 0, sum_i = 0, sum_p = 0;
    uint32_t block_cnt = 0;
    uint32_t fail_streak = 0;
    bool fault_reported = false;
    uint32_t sec_acc_ms = 0; /**< 运行时间累积（毫秒，满 1000 进 1 秒） */

    /* 诊断统计 */
    uint32_t diag_blocks = 0;
    uint32_t frame_total = 0, frame_reject = 0;
    bool reject_check_ok = true; /* 判据是否可用（丢弃率过高则关闭） */
    int32_t raw_v_min = 0, raw_v_max = 0, raw_i_min = 0, raw_i_max = 0;
    double raw_v_sum = 0, raw_i_sum = 0;
    uint32_t raw_n = 0;

    /* 显示平滑（块值的低通） */
    float disp_v = 0.0f, disp_i = 0.0f, disp_p = 0.0f;
    bool disp_init = false;

    /* 快速显示子窗口累加（仅用于 40Hz 即时显示帧） */
    double qsum_v = 0.0, qsum_i = 0.0, qsum_p = 0.0;
    uint32_t qcnt = 0;
    int64_t sub_start_us = esp_timer_get_time();
    /* 块内电能逐步积分（让 40Hz 帧的电能读数也能连续走动；块末以权威值覆写） */
    float block_energy_wh = 0.0f;
    /* 块起点墙钟：dt 用真实时间而非「样本数/标称率」——
     * 2026-10 实测：ADC 实际帧率 ~310Hz（标称 4000Hz），按样本数折算 dt 会把
     * 电能/运行时间算错约 13 倍。真实墙钟对任何实际帧率都正确。 */
    int64_t block_start_us = esp_timer_get_time();
    /* 诊断：上一条诊断打印的墙钟与 DRDY 计数 */
    int64_t diag_last_us = block_start_us;
    uint32_t diag_last_drdy = ads131m02_drdy_count();

    ESP_LOGI(TAG, "采样任务启动（Core1，标称 %u SPS，块周期 %u ms（墙钟），V gain=%.6e I gain=%.6e）",
             PM_SAMPLE_RATE_HZ, PM_BLOCK_PERIOD_MS,
             (double)PM_DEFAULT_V_GAIN, (double)PM_DEFAULT_I_GAIN);

    for (;;) {
        ads131m02_sample_t raw;
        esp_err_t err = ads131m02_read(&raw);
        if (err != ESP_OK) {
            if (++fail_streak == PM_ADC_FAULT_AFTER_FAILS && !fault_reported) {
                ESP_LOGE(TAG, "ADC 连续 %u 次读取失败，上报测量前端故障", (unsigned)fail_streak);
                system_state_report_fault(SYSTEM_FAULT_ADC);
                fault_reported = true;
            }
            continue;
        }
        if (fail_streak >= PM_ADC_FAULT_AFTER_FAILS && fault_reported) {
            system_state_clear_fault(SYSTEM_FAULT_ADC);
            fault_reported = false;
            ESP_LOGI(TAG, "ADC 读取恢复，清除测量前端故障");
        }
        fail_streak = 0;

        /* ---- 帧有效性校验（见文件头）---- */
        frame_total++;
        bool frame_ok = ((raw.status & 0x0003u) == 0x0003u);
        if (!frame_ok) {
            frame_reject++;
        }
        if (reject_check_ok && frame_total >= 400u && frame_reject * 2u > frame_total) {
            reject_check_ok = false;
            ESP_LOGW(TAG, "STATUS 的 DRDY 位判据不可用（丢弃率 %u/%u），已关闭帧过滤，"
                          "请检查 STATUS 字读取是否与帧同步",
                     (unsigned)frame_reject, (unsigned)frame_total);
        }
        if (reject_check_ok && !frame_ok) {
            continue; /* 丢弃不可信帧，不参与统计 */
        }

        xSemaphoreTake(s_calib_mutex, portMAX_DELAY);
        vcal.scale = s_calib.voltage_gain;
        vcal.offset = s_calib.voltage_offset;
        ical.scale = s_calib.current_gain;
        ical.offset = s_calib.current_offset;
        xSemaphoreGive(s_calib_mutex);

        float v = power_calc_raw_to_value(raw.voltage_raw, &vcal);
        float i = power_calc_raw_to_value(raw.current_raw, &ical);
        float p = v * i;

        wave_push_sample(v, i);

        sum_v += v;
        sum_i += i;
        sum_p += (double)p;

        /* ---- 40Hz 快速显示快照（墙钟驱动，不触碰块级统计）---- */
        qsum_v += v;
        qsum_i += i;
        qsum_p += (double)p;
        qcnt++;
        const int64_t sub_now_us = esp_timer_get_time();
        if (qcnt > 0 && (uint64_t)(sub_now_us - sub_start_us) >= PM_DISPLAY_SUBWINDOW_US) {
            float qv = (float)(qsum_v / qcnt);
            float qi = (float)(qsum_i / qcnt);
            float qp = (float)(qsum_p / qcnt);
            /* 子窗口真实时长（墙钟）：帧率非标称值时按样本数折算会错 */
            const float sub_dt_s = (float)(sub_now_us - sub_start_us) / 1e6f;
            sub_start_us = sub_now_us;
            /* 本子窗口对块内电能的贡献（Wh）；块末权威积分以墙钟为准 */
            block_energy_wh = power_calc_energy_wh(block_energy_wh, qp, sub_dt_s);

            data_store_snapshot_t qsnap = {
                .voltage_v = qv,
                .current_a = qi,
                .power_w = qp,
                .energy_wh = s_persist.energy_wh + block_energy_wh,
            };
            xSemaphoreTake(s_aux_data.mutex, portMAX_DELAY);
            qsnap.temperature_c = s_aux_data.temp_c;
            qsnap.humidity_pct = s_aux_data.humidity_pct;
            qsnap.battery_mv = s_aux_data.battery_mv;
            xSemaphoreGive(s_aux_data.mutex);
            qsnap.runtime_s = s_persist.runtime_s;
            data_store_update(&qsnap);

            qsum_v = qsum_i = qsum_p = 0.0;
            qcnt = 0;
        }

        /* 原始码值统计（诊断用） */
        if (raw_n == 0) {
            raw_v_min = raw_v_max = raw.voltage_raw;
            raw_i_min = raw_i_max = raw.current_raw;
        } else {
            if (raw.voltage_raw < raw_v_min) raw_v_min = raw.voltage_raw;
            if (raw.voltage_raw > raw_v_max) raw_v_max = raw.voltage_raw;
            if (raw.current_raw < raw_i_min) raw_i_min = raw.current_raw;
            if (raw.current_raw > raw_i_max) raw_i_max = raw.current_raw;
        }
        raw_v_sum += raw.voltage_raw;
        raw_i_sum += raw.current_raw;
        raw_n++;

        block_cnt++;
        /* 块边界：**墙钟 200ms**（与实际 ODR 解耦——ODR 偏低时按样本数会把
         * 块长拖到秒级，电能/运行时间/诊断节拍全部失真）。 */
        const int64_t block_now_us = esp_timer_get_time();
        if ((uint64_t)(block_now_us - block_start_us) >= (uint64_t)PM_BLOCK_PERIOD_MS * 1000ull) {
            if (block_cnt == 0) {
                /* 本块无有效样本（ADC 全程停帧/全被拒）：只重置计时器，
                 * 避免恢复后第一块 dt 被拉长；统计与积分全部跳过 */
                block_start_us = block_now_us;
                continue;
            }
            /* 直流量：块内平均值（不是 RMS，理由见文件头） */
            float v_mean = (float)(sum_v / block_cnt);
            float i_mean = (float)(sum_i / block_cnt);
            float p_mean = (float)(sum_p / block_cnt);

            /* 显示平滑：一阶低通，避免数值高频跳动 */
            if (!disp_init) {
                disp_v = v_mean;
                disp_i = i_mean;
                disp_p = p_mean;
                disp_init = true;
            } else {
                disp_v += PM_DISPLAY_EMA_ALPHA * (v_mean - disp_v);
                disp_i += PM_DISPLAY_EMA_ALPHA * (i_mean - disp_i);
                disp_p += PM_DISPLAY_EMA_ALPHA * (p_mean - disp_p);
            }

            /* 清零请求（UI 核发起）：在块边界消费——本任务始终是电能值的唯一写者。
             * 同时重置秒累加器，避免清零后立刻又进 1 秒 */
            if (atomic_exchange(&s_reset_energy_req, false)) {
                s_persist.energy_wh = 0.0f;
                s_persist.runtime_s = 0;
                sec_acc_ms = 0;
                save_persist();
                ESP_LOGI(TAG, "电能与运行时间已清零（本次统计重新开始）");
            }

            /* 电能矩形积分：dt 用本块**真实墙钟时长**
             * （实际帧率 ≠ 标称 4kHz 时，按样本数折算会系统性算错；
             *   帧过滤丢帧也不影响——墙钟自动覆盖） */
            const float dt_s = (float)(block_now_us - block_start_us) / 1e6f;
            block_start_us = block_now_us;
            s_persist.energy_wh = power_calc_energy_wh(s_persist.energy_wh, p_mean, dt_s);

            /* 运行时间：按实际采样时间累加（每满 1s 进 1） */
            sec_acc_ms += (uint32_t)(dt_s * 1000.0f + 0.5f);
            if (sec_acc_ms >= 1000u) {
                s_persist.runtime_s += sec_acc_ms / 1000u;
                sec_acc_ms %= 1000u;
            }

            /* 注意：块末**不再**向 data_store 写 EMA 帧。显示快照唯一来源是
             * 上方 40Hz 子窗口的即时均值帧（电能在其中已用 block_energy_wh
             * 连续积分、块末归零处无缝衔接）。若再写一帧 EMA 值，30Hz 档
             * （窗口=1 帧）每 200ms 会有一次读数向慢响应值回顿，破坏高刷连贯。
             * EMA 值仍保留用于下方诊断日志。 */

            /* ---- 周期性诊断（每 25 块）：给"读数对不对"提供板上证据 ---- */
            if (++diag_blocks >= PM_DIAG_PERIOD_BLOCKS) {
                diag_blocks = 0;
                const int64_t diag_now_us = esp_timer_get_time();
                const uint32_t drdy_now = ads131m02_drdy_count();
                const uint32_t diag_wall_ms = (uint32_t)((diag_now_us - diag_last_us) / 1000);
                const uint32_t drdy_delta = drdy_now - diag_last_drdy;
                diag_last_us = diag_now_us;
                diag_last_drdy = drdy_now;
                ESP_LOGI(TAG, "诊断: wall=%ums drdy+%u(%.0fHz) 帧=%u 丢弃=%u(%.1f%%) | "
                              "原始码 V[min=%ld max=%ld 均值=%.0f] I[min=%ld max=%ld 均值=%.0f] | "
                              "换算 V=%.5fV I=%.5fA P=%.5fW",
                         (unsigned)diag_wall_ms, (unsigned)drdy_delta,
                         diag_wall_ms ? (1000.0 * (double)drdy_delta / (double)diag_wall_ms) : 0.0,
                         (unsigned)frame_total, (unsigned)frame_reject,
                         frame_total ? (100.0f * (float)frame_reject / (float)frame_total) : 0.0f,
                         (long)raw_v_min, (long)raw_v_max, raw_n ? (raw_v_sum / raw_n) : 0.0,
                         (long)raw_i_min, (long)raw_i_max, raw_n ? (raw_i_sum / raw_n) : 0.0,
                         (double)disp_v, (double)disp_i, (double)disp_p);
                frame_total = 0;
                frame_reject = 0;
                raw_n = 0;
                raw_v_sum = raw_i_sum = 0;
            }

            sum_v = sum_i = sum_p = 0;
            block_cnt = 0;
            block_energy_wh = 0.0f; /* 块末权威电能已写入，子窗口积分归零 */
        }
    }
}

/** 辅助采集任务（Core0）：温湿度/电池 2s 一次；每 30s 落一次盘 */
static void task_aux(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t ticks = 0;
    uint32_t aht_fail = 0;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(PM_AUX_PERIOD_MS));

        aht20_reading_t env;
        esp_err_t aerr = aht20_read(&env);
        if (aerr == ESP_OK) {
            if (aht_fail != 0) {
                ESP_LOGI(TAG, "AHT20 读取恢复正常（此前连续失败 %u 次）", (unsigned)aht_fail);
                aht_fail = 0;
            }
            xSemaphoreTake(s_aux_data.mutex, portMAX_DELAY);
            s_aux_data.temp_c = env.temperature_c;
            s_aux_data.humidity_pct = env.humidity_pct;
            s_aux_data.temp_valid = true;
            xSemaphoreGive(s_aux_data.mutex);
        } else {
            /* 前 3 次与之后每 15 次（30s）打印一次，避免刷屏又能定位问题 */
            if (aht_fail < 3 || (aht_fail % 15u) == 0) {
                ESP_LOGW(TAG, "AHT20 读取失败（第 %u 次）: %s", (unsigned)(aht_fail + 1),
                         esp_err_to_name(aerr));
            }
            aht_fail++;
        }

        uint32_t bat_mv = 0;
        if (battery_adc_read(&bat_mv) == ESP_OK) {
            xSemaphoreTake(s_aux_data.mutex, portMAX_DELAY);
            s_aux_data.battery_mv = bat_mv;
            xSemaphoreGive(s_aux_data.mutex);
        }

        if (++ticks >= PM_SAVE_PERIOD_TICKS) {
            ticks = 0;
            save_persist();
        }
    }
}

/* ============================================================
 * 公共 API
 * ============================================================ */
esp_err_t power_measure_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    s_calib_mutex = xSemaphoreCreateMutex();
    s_aux_data.mutex = xSemaphoreCreateMutex();
    if (s_calib_mutex == NULL || s_aux_data.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    load_persist_and_calib();

    ring_init(&s_tier[0], PM_TIER0_CAP);
    ring_init(&s_tier[1], PM_TIER1_CAP);
    ring_init(&s_tier[2], PM_TIER2_CAP);
    size_t point_bytes = sizeof(wave_point_t);
    const uint32_t caps[3] = { PM_TIER0_CAP, PM_TIER1_CAP, PM_TIER2_CAP };
    for (int t = 0; t < 3; t++) {
        s_tier[t].buf = heap_caps_malloc((size_t)caps[t] * point_bytes, MALLOC_CAP_SPIRAM);
        if (s_tier[t].buf == NULL) {
            ESP_LOGE(TAG, "tier%d 波形缓冲分配失败（PSRAM）", t);
            return ESP_ERR_NO_MEM;
        }
        s_tier[t].cap = caps[t];
    }
    ESP_LOGI(TAG, "波形缓冲已分配（PSRAM，%u 字节）",
             (unsigned)((PM_TIER0_CAP + PM_TIER1_CAP + PM_TIER2_CAP) * point_bytes));

    s_inited = true;
    ESP_LOGI(TAG, "初始化完成（采样率 %u Hz，直流量测平均，V gain=%.6e V/LSB，"
                  "I gain=%.6e A/LSB，均为未实测标定的理论值）",
             PM_SAMPLE_RATE_HZ, (double)PM_DEFAULT_V_GAIN, (double)PM_DEFAULT_I_GAIN);
    return ESP_OK;
}

esp_err_t power_measure_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok1 = xTaskCreatePinnedToCore(task_sample, "pm_sample", PM_SAMPLE_STACK, NULL,
                                             PM_SAMPLE_PRIO, &s_task_sample, 1);
    BaseType_t ok2 = xTaskCreatePinnedToCore(task_aux, "pm_aux", PM_AUX_STACK, NULL, PM_AUX_PRIO,
                                             &s_task_aux, 0);
    if (ok1 != pdPASS || ok2 != pdPASS) {
        ESP_LOGE(TAG, "测量任务创建失败（sample=%d aux=%d）", (int)ok1, (int)ok2);
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t power_measure_set_calibration(const power_measure_calib_t *calib)
{
    if (calib == NULL || calib->voltage_gain <= 0.0f || calib->current_gain <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_calib_mutex, portMAX_DELAY);
    s_calib = *calib;
    xSemaphoreGive(s_calib_mutex);
    save_persist();
    ESP_LOGI(TAG, "标定已更新并持久化（V gain=%.6e off=%.6f / I gain=%.6e off=%.6f）",
             (double)calib->voltage_gain, (double)calib->voltage_offset,
             (double)calib->current_gain, (double)calib->current_offset);
    return ESP_OK;
}

void power_measure_get_calibration(power_measure_calib_t *out)
{
    if (out == NULL) {
        return;
    }
    xSemaphoreTake(s_calib_mutex, portMAX_DELAY);
    *out = s_calib;
    xSemaphoreGive(s_calib_mutex);
}

esp_err_t power_measure_reset_energy(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只置请求标志，不在本核直接改电能值（跨核写会让累加中的 float 撕裂） */
    atomic_store(&s_reset_energy_req, true);
    return ESP_OK;
}

uint32_t power_measure_sample_rate_hz(void)
{
    return ads131m02_get_sample_rate_hz() ? ads131m02_get_sample_rate_hz()
                                         : PM_SAMPLE_RATE_HZ;
}

static void mode_fields(pm_wave_mode_t mode, int *fa, int *fb, bool *has_b)
{
    switch (mode) {
    case PM_WAVE_MODE_P:  *fa = 2; *fb = 0; *has_b = false; break;
    case PM_WAVE_MODE_IP: *fa = 1; *fb = 2; *has_b = true;  break;
    case PM_WAVE_MODE_VP: *fa = 0; *fb = 2; *has_b = true;  break;
    case PM_WAVE_MODE_VI: *fa = 0; *fb = 1; *has_b = true;  break;
    case PM_WAVE_MODE_E:  *fa = 3; *fb = 0; *has_b = false; break;
    default:             *fa = 2; *fb = 0; *has_b = false; break;
    }
}

int pm_wave_copy(pm_wave_window_t win, pm_wave_mode_t mode, float *a, float *b, int max_points)
{
    if (a == NULL || max_points <= 0 || win >= PM_WAVE_WINDOW_MAX || mode >= PM_WAVE_MODE_MAX) {
        return 0;
    }
    const window_map_t *m = &s_window_map[win];
    int fa, fb;
    bool has_b;
    mode_fields(mode, &fa, &fb, &has_b);

    uint32_t n = m->points;
    int stride = 1;
    if ((int)n > max_points) {
        stride = (int)((n + (uint32_t)max_points - 1u) / (uint32_t)max_points);
    }

    int got;
    portENTER_CRITICAL(&s_ring_lock);
    got = ring_copy_locked(&s_tier[m->tier], n, stride, fa, fb, has_b, a, b, max_points);
    portEXIT_CRITICAL(&s_ring_lock);
    return got;
}

static bool span_of(const float *v, int n, float *mn, float *mx)
{
    if (n <= 0) {
        return false;
    }
    float lo = v[0], hi = v[0];
    for (int k = 1; k < n; k++) {
        if (v[k] < lo) lo = v[k];
        if (v[k] > hi) hi = v[k];
    }
    *mn = lo;
    *mx = hi;
    return true;
}

static void pad_range(float mn, float mx, float *out_min, float *out_max)
{
    float span = mx - mn;
    if (span < 1e-9f) {
        float r = fabsf(mx) > 1e-9f ? fabsf(mx) * 0.1f : 1.0f;
        if (r < 1e-6f) r = 1.0f;
        *out_min = mn - r;
        *out_max = mx + r;
    } else {
        float pad = span * 0.1f;
        *out_min = mn - pad;
        *out_max = mx + pad;
    }
}

bool pm_wave_get_range(pm_wave_window_t win, pm_wave_mode_t mode, float *a_min, float *a_max,
                       float *b_min, float *b_max)
{
    static float ba[300], bb[300];
    int n = pm_wave_copy(win, mode, ba, bb, 300);
    int fa, fb;
    bool has_b;
    mode_fields(mode, &fa, &fb, &has_b);

    float mn, mx;
    if (span_of(ba, n, &mn, &mx)) {
        pad_range(mn, mx, a_min, a_max);
    } else {
        *a_min = 0.0f;
        *a_max = 1.0f;
    }
    if (has_b && span_of(bb, n, &mn, &mx)) {
        pad_range(mn, mx, b_min, b_max);
    } else if (has_b) {
        *b_min = 0.0f;
        *b_max = 1.0f;
    }
    return has_b;
}

const char *pm_wave_window_name(pm_wave_window_t win)
{
    static const char *names[PM_WAVE_WINDOW_MAX] = {
        "1s", "10s", "30s", "60s", "120s", "10min",
    };
    return (win < PM_WAVE_WINDOW_MAX) ? names[win] : "?";
}

const char *pm_wave_mode_name(pm_wave_mode_t mode)
{
    static const char *names[PM_WAVE_MODE_MAX] = {
        "功率", "电流+功率", "电压+功率", "电压+电流", "电能",
    };
    return (mode < PM_WAVE_MODE_MAX) ? names[mode] : "?";
}
