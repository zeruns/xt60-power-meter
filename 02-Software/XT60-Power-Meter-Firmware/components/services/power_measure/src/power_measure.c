/**
 * @file power_measure.c
 * @brief 功率测量服务实现（采样链路 / 标定 / 直流均值 / 三级 PSRAM 波形缓冲 / NVS 掉电保存）
 *
 * ─────────────────────────────────────────────────────────────
 * 一、模块职责与在系统中的位置
 * ─────────────────────────────────────────────────────────────
 * 本模块是整机的「测量核心」：驱动 ADS131M02 同步采样 ADC，
 * 把原始码值换算成电压/电流/功率/电能，供 UI 显示并持久化。
 * 数据流（自下而上）：
 *
 *   ADS131M02（SPI，DRDY 中断提示"新样本就绪"）
 *      │  task_sample 在 Core1（测量核）逐帧读取
 *      ├─→ 码值→物理量换算（power_calc 纯函数）
 *      ├─→ 三级 PSRAM 波形环形缓冲（tier0/1/2，供示波界面）
 *      ├─→ 200ms 块统计 → data_store（单写者，UI 只读快照）
 *      └─→ 电能/运行时间 → NVS（命名空间 "pm"，掉电不丢）
 *
 * 任务布局（双核 ESP32-S3）：
 *   task_sample（Core1，高优先级）——测量主链路，节奏由 DRDY 驱动；
 *   task_aux   （Core0，低优先级）——AHT20 温湿度（I2C 读一次 ≥80ms，
 *     会阻塞，放交互核以免拖慢采样）与电池电压，结果由采样任务合并进快照。
 *
 * ─────────────────────────────────────────────────────────────
 * 二、关键设计取舍（为什么这么做）
 * ─────────────────────────────────────────────────────────────
 * 1. 直流量取「块内平均值」而不是 RMS：
 *    本设备测的是 USB-PD / 电池供电的直流。对带噪声的直流量，
 *    RMS ≥ |均值|，用 RMS 会系统性偏大；示波类直流表显示的也是平均值。
 *    功率用 mean(V·I)（真实平均功率），噪声相关性不为零时比
 *    mean(V)×mean(I) 更准。
 * 2. data_store 单写者：只有 task_sample 写快照，UI 核只读。
 *    若两个核同时写 float，读方可能读到"写了一半"的撕裂值；
 *    单写者 + 只读快照让 UI 无需加锁即可安全读取。
 * 3. 电能清零在「块边界」执行：清零请求（UI 核发起）只置原子标志，
 *    由采样任务在块边界消费。电能值因此永远只有一个写者，
 *    也不会在累加过程中被另一核改写造成撕裂。
 *    落盘（NVS 写入）与清零解耦：采样任务只改内存并置「待落盘」标志，
 *    真正的 flash 写入统一由 task_aux（Core0）执行。nvs_commit 可达
 *    数百 ms，若放在采样任务里同步做，期间 DRDY 帧全部被丢。
 * 4. NVS 只有一个写者（task_aux）：电能/运行时间的周期落盘、清零落盘、
 *    标定更新落盘全部收敛到 task_aux 串行执行（见 save_persist 的取舍说明）。
 * 5. 一切时间统计以「墙钟」（esp_timer）为准，不用「样本数/标称率」：
 *    2026-10 实测 4kHz 档下 DRDY 被抑制到 ~310Hz（SBAS853 §8.5.1.5 的
 *    读帧阻塞机制，非 MCLK/晶振异常），按样本数折算会把电能/运行时间
 *    算错数倍；帧过滤丢帧同样使样本数不可靠。墙钟对任何实际帧率都正确。
 *
 * ─────────────────────────────────────────────────────────────
 * 三、帧有效性校验（关键，毛刺根因）
 * ─────────────────────────────────────────────────────────────
 * ADS131M02 每帧的 STATUS 字里 DRDY0/DRDY1（bit0/bit1）表示对应通道有
 * 新数据。只有「DRDY 下降沿之后读到的第一帧」这两个位才是 1。若读到 0，
 * 说明帧边界漂移/读到了陈旧帧，此时 CH0/CH1 的数据位置也不可信
 * （表现为读数乱跳、与真实值无关的大幅摆动）。
 * 因此：以 CRC-16/CCITT 校验（crc_ok）为首要判据丢弃坏帧并统计丢弃率；
 * 若丢弃率 >50%（判据本身不可用），自动降级回 STATUS 位判据并告警，
 * 避免界面因全帧被拒而完全不更新。
 *
 * ─────────────────────────────────────────────────────────────
 * 四、波形缓冲（PSRAM，三级降采样）
 * ─────────────────────────────────────────────────────────────
 *   tier0  4000Hz × 4000 点  —— 1s 窗（原始逐点，已过 4 点滑动平均轻滤）
 *   tier1    50Hz × 24000 点 —— 60s 窗（每 80 点均值，可回看 480s）
 *   tier2  1.25Hz × 6000 点  —— 10min 窗（每 40 个 tier1 点均值，可回看 80min）
 * 每个点含 V/I/P/E 四个 float + 时间戳；UI 用 pm_wave_copy() 按
 * 固定时间槽聚合取数（契约：每槽固定时长，与采样率无关）。
 *
 * ─────────────────────────────────────────────────────────────
 * 五、建议阅读顺序
 * ─────────────────────────────────────────────────────────────
 *   ① 文末公共 API（power_measure_init/start）建立整体印象
 *   ② task_sample 主循环——采样→校验→换算→缓冲→块统计
 *   ③ wave_push_sample / ring_fill_slots——波形数据的写与读
 *   ④ load_persist_and_calib / save_persist——NVS 掉电保存
 */
#include "power_measure.h"

#include <math.h>
#include <string.h>

#include "ads131m02.h"
#include "aht20.h"
#include "battery_adc.h"
#include "data_store.h"
#include "event_bus.h"
#include "power_calc.h"
#include "settings.h"
#include "system_state.h"

#include "driver/temperature_sensor.h" /* 芯片内部温度传感器（SOC 结温）驱动 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>

#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "power_measure"; /**< ESP_LOG 日志标签，过滤本模块日志用 */

/* ============================================================
 * 采样与计算参数
 * ============================================================ */
#define PM_SAMPLE_RATE_HZ  4000u                 /**< ADC 输出数据率（与驱动 CLOCK OSR=1024 档配置一致） */
#define PM_BLOCK_SAMPLES   800u                  /**< 标称 4000Hz 下 200ms 的样本数（仅作历史参考） */
#define PM_BLOCK_PERIOD_MS (1000u * PM_BLOCK_SAMPLES / PM_SAMPLE_RATE_HZ) /**< 块周期：墙钟 200ms */
#define PM_BLOCK_PER_SEC   (PM_SAMPLE_RATE_HZ / PM_BLOCK_SAMPLES) /**< 每秒块数 = 5 */

/** 显示平滑系数（对块均值做一阶低通）：约 0.8s 时间常数，兼顾响应与稳定 */
#define PM_DISPLAY_EMA_ALPHA 0.25f
/** 波形轻滤：8 点滑动平均（4000Hz 采样 → 约 500Hz 等效带宽）。
 *  M4 显示平滑：OSR=1024 档单样本噪声约为 OSR=8192 档的 √8≈2.8 倍，
 *  8 点均值把显示噪声压回约原水平（统计/电能不受轻滤影响，仍用原始样本）。 */
#define PM_WAVE_MAVG_N 8u

/** 前端默认标定（未实测标定时的理论值，见 power_calc.h 的量纲说明）
 *  电压：满量程 ±1.2V@PGA1× / 2^23 码，外部分压 101:1（R33 100k / R39 1k）
 *        → 1.2/8388608×101 = 1.445e-05 V/LSB */
#define PM_DEFAULT_V_GAIN  (1.2f / 8388608.0f * 101.0f)
/** 电流：满量程 ±1.2V / PGA16× / 2^23 码，分流器 1mΩ 四端开尔文
 *        → (1.2/16)/8388608/0.001 = 8.941e-06 A/LSB */
#define PM_DEFAULT_I_GAIN  ((1.2f / 16.0f) / 8388608.0f / 0.001f)

/* ---- 波形三级缓冲 ---- */
typedef struct {
    float v;         /**< 电压 V */
    float i;         /**< 电流 A */
    float p;         /**< 功率 W */
    float e;         /**< 电能 Wh（该点时刻的累计值） */
    uint32_t t_ms;   /**< 采样时间戳（ms，自启动）。用于把样本按「真实时间」
                      *   对齐到屏幕固定时间槽——不再按标称点数抽稀，
                      *   因此实际采样率偏低/波动时时间尺度依然正确。 */
} wave_point_t;

#define PM_TIER0_CAP 4000u   /**< 4000Hz × 1s（恰好覆盖 1s 窗；4kHz 下回看余量=窗长） */
#define PM_TIER1_CAP 24000u  /**< 50Hz × 480s（覆盖最长 60s 窗） */
#define PM_TIER2_CAP 6000u   /**< 1.25Hz × 4800s（覆盖最长 10min 窗） */
#define PM_TIER1_DECIM 80u   /**< tier0→tier1：80 点均值（4000/80=50Hz，tier1 速率与旧版一致） */
#define PM_TIER2_DECIM 40u   /**< tier1→tier2：40 点均值（50/40=1.25Hz） */

typedef struct {
    uint8_t tier;    /**< 该窗口取哪一级缓冲 */
    uint32_t win_ms; /**< 该窗口的时长（毫秒）——水平时间尺度的唯一依据 */
} window_map_t;
/** 时间窗 → (缓冲级, 窗长) 映射表：UI 选哪个窗，就从对应 tier 取数。
 *  用 [枚举]= {...} 指定初始化，顺序打乱也不会错位。 */
static const window_map_t s_window_map[PM_WAVE_WINDOW_MAX] = {
    [PM_WAVE_1S]    = { 0, 1000u },
    [PM_WAVE_10S]   = { 1, 10000u },
    [PM_WAVE_30S]   = { 1, 30000u },
    [PM_WAVE_60S]   = { 1, 60000u },
    [PM_WAVE_120S]  = { 2, 120000u },
    [PM_WAVE_10MIN] = { 2, 600000u },
};

/* ---- NVS 持久化 ----
 * NVS（Non-Volatile Storage）是 ESP-IDF 的键值型掉电存储，
 * 烧录/OTA 不会擦掉它，适合存"标定参数、累计电能"这类小数据。 */
#define PM_NVS_NS "pm"                       /**< NVS 命名空间（相当于表名） */
#define PM_NVS_KEY_DATA "pmdat"              /**< 电能/运行时间 blob 的键名 */
#define PM_NVS_MAGIC 0x504D3031u /**< 'PM01'：blob 版本魔数，读到不认识的格式就丢弃 */
/* 【B11 数值修复】电能/容量的权威值以 double 独立键存储：
 * pmdat.energy_wh 是 float（保留兼容旧固件布局），>1024Wh 后一次落盘/恢复
 * 会引入 0.5 ulp 级舍入偏差；独立 double 键消除该偏差，旧键缺失时回退。 */
#define PM_NVS_KEY_ENERGY_HI "pmdw" /**< 累计电能（double，权威） */
#define PM_NVS_KEY_CAP_HI "pmah"    /**< 累计容量（double，Ah） */

/** 掉电保存的数据布局（整体作为一个 blob 存 NVS） */
typedef struct {
    uint32_t magic;      /**< 版本魔数，校验数据布局是否是本版本写入的 */
    float energy_wh;     /**< 累计电能（Wh，NVS 存储格式仍是 float，写入时从 double 降精度） */
    uint32_t runtime_s;  /**< 累计运行时间（秒） */
} pm_persist_t;

/* ============================================================
 * 模块内部状态
 * ============================================================ */
static bool s_inited = false;   /**< power_measure_init 已完成 */
static bool s_started = false;  /**< 测量任务已创建（防止重复 start） */

static TaskHandle_t s_task_sample = NULL; /**< FreeRTOS 任务句柄（保留以便日后挂起/删除） */
static TaskHandle_t s_task_aux = NULL;

static power_measure_calib_t s_calib;        /**< 当前生效的标定参数 */
static SemaphoreHandle_t s_calib_mutex = NULL; /**< 互斥锁：保护 s_calib 跨核读改写 */

/** 累计电能（Wh）：内部用 double 累计。
 *  float 只有 24 位尾数，累计值超过约 1024 Wh 后相邻可表示间隔大于
 *  典型的每块增量（50W×0.2s≈0.0028Wh），增量会被"舍入吞掉"，电能不再走动；
 *  double 有 53 位尾数，按 1W·h 分辨率算可累计约 900 万 Wh，永不丢增量。
 *  对外接口仍是 float（见 power_measure.h），取值时再转换。 */
static double s_energy_wh = 0.0;
/** 累计容量（Ah）：同上用 double 累计，与电能并列积分（v0.2.0 仪器功能批 F2） */
static double s_capacity_ah = 0.0;

/** 本次上电的峰/谷值（v0.2.0 仪器功能批 F1）：块均值口径，task_sample 单写者，
 *  40Hz 快照同任务读取无需加锁。首次块边界用首块均值初始化。 */
static float s_v_max = 0, s_v_min = 0, s_i_max = 0, s_i_min = 0, s_p_max = 0, s_p_min = 0;
static bool s_peak_init = false;

/** 当前越限状态位图（DATA_ALERT_*，v0.2.0 仪器功能批 F3）
 *  块边界判定、块边界清零；上升沿才发 APP_EVENT_MEASURE_ALERT 事件。 */
static uint32_t s_alert_mask = 0;
/** 累计运行时间（秒）。
 *  单写者约定：上面两个值只由 task_sample 写（清零/积分都在块边界），
 *  task_aux 落盘时只读。 */
static uint32_t s_runtime_s = 0;

/** 保护 s_energy_wh / s_runtime_s 的互斥锁
 *
 * 写者虽只有 task_sample，但 task_aux 落盘时要读它们组包：double 在
 * 32 位核上是两条 32 位写，落盘读到一半会被插进来的写者撕裂。
 * 读写两侧都持锁即可保证一致。注意 task_aux 绝不能持锁做 NVS 写入
 * （数百 ms），只在"拷贝进 blob"的几行代码里持锁。 */
static SemaphoreHandle_t s_persist_mutex = NULL;

/** 保护「NVS 落盘」动作的互斥锁（save_persist 内使用）
 *
 * s_persist_mutex/s_calib_mutex 只保护数据本身；这把锁保护"落盘过程"：
 * task_aux 周期落盘与 set_calibration 触发的落盘可能并发，
 * nvs_set_blob/commit 交错会互相覆盖（见 save_persist 的取舍说明）。
 * 只有落盘方竞争它，因此 NVS 写入的数百 ms 耗时不会波及采样任务。 */
static SemaphoreHandle_t s_save_mutex = NULL;

/** 有待落盘的改动（电能/运行时间/标定任一被改过，NVS 尚未写入）
 *
 * 用 atomic 而不是 bool：置位方跨核（task_sample / UI 核）、
 * 清除方是 task_aux，atomic_exchange 天然不会丢/重置竞态。 */
static atomic_bool s_persist_dirty = false;

/** 电能清零请求（UI 核 → 测量核）：由采样任务在块边界消费，保持电能值单写者 */
static atomic_bool s_reset_energy_req = false;

/** 辅助量（温湿度/电池）：task_aux 写、task_sample 合并进快照，用互斥锁保护 */
static struct {
    SemaphoreHandle_t mutex;   /**< 保护本结构体的互斥锁 */
    float temp_c;              /**< 温度（℃） */
    float humidity_pct;        /**< 相对湿度（%） */
    uint32_t battery_mv;       /**< 电池电压（mV） */
    bool temp_valid;           /**< 温湿度是否读到过有效值 */
    float soc_temp_c;          /**< ESP32-S3 芯片 SOC 结温（℃，v0.5.1 新增） */
    bool soc_temp_valid;       /**< SOC 温度是否读到过有效值 */
} s_aux_data;

/** 芯片内部温度传感器句柄（v0.5.1 新增：网页端展示主控发热情况）
 *
 * 与 AHT20 的 s_aux_data.temp_c（机内**环境**温度）区分：本传感器在 SOC
 * 硅片内部，读数通常比环境温度高若干度，反映主控自身发热。
 * 安装/使能失败时保持 NULL，UI 与网页端按"无数据"（NaN）显示，不影响测量。 */
static temperature_sensor_handle_t s_soc_tsens = NULL;

/** 环形缓冲描述（单写者 task_sample + 受 s_ring_lock 保护的读者 pm_wave_copy） */
typedef struct {
    wave_point_t *buf; /**< 数据区（PSRAM，初始化时分配） */
    uint32_t cap;      /**< 容量（点数），0 表示未分配 */
    uint32_t head;     /**< 下一个写入位置 */
    uint32_t count;    /**< 当前有效点数（≤ cap） */
} ring_t;

static ring_t s_tier[3]; /**< 三级波形缓冲 */
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED; /**< 自旋锁：环形缓冲读写互斥 */

static uint32_t s_tier1_acc_n = 0;  /**< tier1 降采样累计的样本数 */
static wave_point_t s_tier1_acc;    /**< tier1 降采样累计和（v/i/p 逐项累加） */
static uint32_t s_tier2_acc_n = 0;  /**< tier2 降采样累计的 tier1 点数 */
static wave_point_t s_tier2_acc;    /**< tier2 降采样累计和 */

/**
 * @brief 初始化环形缓冲描述（数据区由 power_measure_init 稍后分配）
 * @param r   目标环形缓冲
 * @param cap 容量（点数），仅记录，真正分配在 init
 */
static void ring_init(ring_t *r, uint32_t cap)
{
    r->buf = NULL;
    r->cap = 0;
    r->head = 0;
    r->count = 0;
}

/**
 * @brief 向环形缓冲写入一点（满则覆盖最旧点）
 *
 * @note 调用方需持有 s_ring_lock（与 pm_wave_copy 读者互斥）
 * @param r 目标环形缓冲
 * @param p 待写入的波形点
 */
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
 * @brief 时间槽位填充：把「以 now 为右端、跨 window_ms」的 slots 个槽填满
 *
 * 槽 j（0..slots-1）对应用户看到的第 j 列，覆盖时段
 *   age ∈ [(slots-1-j)*step, (slots-1-j+1)*step)，step = window_ms / slots
 *
 * 【2026-10 波形重构 Step1】桶聚合取代"最近样本+容差"：
 *   旧算法取「距 target 最近且 ≤半个槽」的单样本，当采样间隔（tier1 在
 *   500Hz 下为 40ms）与槽宽（10s/240≈41.7ms）不同步时约一半槽超出容差
 *   → 留 NaN → 曲线断成竖条碎片（视频实证）。
 *   新算法把时段内**所有样本**聚合取均值：每桶必有样本（只要采样间隔
 *   ≤ 槽宽），天然连续；顺带对桶内多样本求均值，抑制噪声（示波器
 *   均值采集模式同理）。
 *   真空缺（ADC 停帧超过槽宽）仍断线：空桶前向填充限 2 桶，超限留 NaN，
 *   既不画假波形，也不因亚槽级抖动产生碎片。
 *
 * ⚠ 这是本模块给 UI 的时间契约：**每槽固定时长**，与采样率无关。
 *
 * @param r         源环形缓冲
 * @param now_ms    当前时间戳（ms），槽位右端对齐到它
 * @param window_ms 窗口时长（ms）
 * @param slots     输出槽（列）数
 * @param field_a   主序列取点内哪个字段（0=v 1=i 2=p 3=e）
 * @param field_b   副序列字段（同上，has_b=false 时忽略）
 * @param has_b     是否输出副序列
 * @param a         主序列输出缓冲（长度 ≥ slots），空槽写 NaN
 * @param b         副序列输出缓冲（可为 NULL）
 * @note 本函数自行管理短临界区：区内只做 memcpy 快照，聚合/插值都在锁外
 */
#define PM_WAVE_SLOT_MAX 480 /* 与 screen_wave 的 CHART_MAX_POINTS 对齐 */

/* 桶聚合暂存（仅 ring_fill_slots 内使用；pm_wave_copy 由 UI 任务串行调用，
 * 聚合全程在锁外，靠"单线程调用约定"保证不被并发覆盖） */
static float s_bsum_a[PM_WAVE_SLOT_MAX]; /**< 各槽主序列样本和 */
static float s_bsum_b[PM_WAVE_SLOT_MAX]; /**< 各槽副序列样本和 */
static uint16_t s_bcnt[PM_WAVE_SLOT_MAX]; /**< 各槽样本计数 */

/** 分块快照的块大小（点数）与暂存区
 * 【并发修复】临界区内绝不能做 O(万点) 的聚合/插值：portENTER_CRITICAL 是
 * 自旋锁，Core1 采样任务会被卡住数 ms，DRDY 帧全部作废。现在临界区内
 * **只 memcpy 出一小块原始点**（512 点 × 20B = 10KB，约 0.2~0.5ms），
 * 桶聚合/插值/填充全部移到锁外做。 */
#define PM_WAVE_CHUNK 512u
static wave_point_t s_snap[PM_WAVE_CHUNK]; /**< 分块快照暂存（同上，UI 任务串行使用） */

static void ring_fill_slots(const ring_t *r, uint32_t now_ms, uint32_t window_ms,
                            int slots, int field_a, int field_b, bool has_b,
                            float *a, float *b)
{
    /* 防御：槽位数超过暂存区容量时截断，避免数组越界写 */
    if (slots > PM_WAVE_SLOT_MAX) {
        slots = PM_WAVE_SLOT_MAX;
    }
    /* 全部预置 NaN（LVGL 折线遇到 NaN 断线）与零累计 */
    for (int j = 0; j < slots; j++) {
        a[j] = NAN;
        if (b != NULL) {
            b[j] = NAN;
        }
        s_bsum_a[j] = 0.0f;
        s_bsum_b[j] = 0.0f;
        s_bcnt[j] = 0;
    }
    if (r->buf == NULL || slots <= 0 || window_ms == 0) {
        return;
    }

    /* 先在短临界区里快照"有效区间"的边界（3 个整数，几条指令）：
     * start = 最旧点的物理下标，n = 有效点数，cap = 环容量 */
    uint32_t start, n, cap;
    portENTER_CRITICAL(&s_ring_lock);
    cap = r->cap;
    n = r->count;
    start = (r->head + cap - n) % r->cap;
    portEXIT_CRITICAL(&s_ring_lock);
    if (n == 0 || cap == 0) {
        return;
    }

    /* ① 桶聚合：按样本年龄归桶求均值。
     * 从**最新一端**开始分块拷贝：环满时写者只覆盖最旧端，
     * 就算拷贝期间环被推进，也只可能把还没读到的最旧几个点换成新点
     * （新点 t_ms 靠近"现在"，会多算进最新桶，观感无碍）；
     * 而每次拷贝都在临界区内，读到的永远是完整点，不会撕裂。 */
    for (uint32_t done = 0; done < n;) {
        /* 本块覆盖逻辑区间 [k0, k0+chunk)：从最新端往旧处切 */
        uint32_t chunk = n - done;
        if (chunk > PM_WAVE_CHUNK) {
            chunk = PM_WAVE_CHUNK;
        }
        const uint32_t k0 = n - done - chunk;

        /* 拷出本块（物理上绕环时拆成两段 memcpy） */
        uint32_t phys = (start + k0) % cap;
        uint32_t copied = 0;
        portENTER_CRITICAL(&s_ring_lock);
        while (copied < chunk) {
            uint32_t seg = cap - phys;
            if (seg > chunk - copied) {
                seg = chunk - copied;
            }
            memcpy(&s_snap[copied], &r->buf[phys], seg * sizeof(wave_point_t));
            copied += seg;
            phys = 0;
        }
        portEXIT_CRITICAL(&s_ring_lock);

        /* 锁外聚合本块（年龄归桶逻辑与旧实现一致） */
        bool any_in_window = false;
        for (uint32_t k = 0; k < chunk; k++) {
            const wave_point_t *p = &s_snap[k];
            const uint32_t age = now_ms - p->t_ms; /* 无符号回绕安全 */
            if (age > window_ms) {
                continue; /* 窗口之外 */
            }
            any_in_window = true;
            /* 年龄 0 → 最右槽（现在）；年龄越大越靠左 */
            int j = slots - 1 - (int)((double)age * (double)slots / (double)window_ms);
            if (j < 0) {
                j = 0;
            } else if (j >= slots) {
                j = slots - 1;
            }
            float va, vb = NAN;
            switch (field_a) {
            case 0: va = p->v; break;
            case 1: va = p->i; break;
            case 2: va = p->p; break;
            default: va = p->e; break;
            }
            if (has_b && b != NULL) {
                switch (field_b) {
                case 0: vb = p->v; break;
                case 1: vb = p->i; break;
                case 2: vb = p->p; break;
                default: vb = p->e; break;
                }
            }
            s_bsum_a[j] += va;
            s_bcnt[j]++;
            if (has_b && b != NULL) {
                s_bsum_b[j] += vb;
            }
        }
        /* 提前结束：本块整块都超出窗口，而越往旧处越超窗，
         * 后面的块不必再拷（短窗口时省掉绝大部分遍历） */
        if (!any_in_window) {
            break;
        }
        done += chunk;
    }

    /* ② 桶均值 → 输出 */
    for (int j = 0; j < slots; j++) {
        if (s_bcnt[j] > 0) {
            a[j] = s_bsum_a[j] / (float)s_bcnt[j];
            if (has_b && b != NULL) {
                b[j] = s_bsum_b[j] / (float)s_bcnt[j];
            }
        }
    }

    /* ③ 全桶线性插值（Step4·min/max 包络前提）：
     *   480 槽 > 屏宽 280 像素，LVGL 对 LINE 系列自动画 min/max 竖线包络
     *   （示波器 peak-detect 同款，防混叠丢峰）。但包络只覆盖**连续**段，
     *   NaN 断点会把包络也切成碎片——所以这里把空桶全插值补齐，
     *   让包络覆盖整个窗口。末端空桶（历史不足）回退单桶填补限 2 个，
     *   更靠左的仍留 NaN（画假曲线没意义）。 */
    for (int j = 1; j < slots; j++) {
        if (isfinite(a[j])) {
            continue;
        }
        int prev = j - 1;
        while (prev >= 0 && !isfinite(a[prev])) {
            prev--;
        }
        if (prev < 0) {
            continue;
        }
        /* 找右锚点：向右扫描下一个有效桶 */
        int next = j;
        while (next < slots && !isfinite(a[next])) {
            next++;
        }
        if (next < slots) {
            /* 两端均有锚点：线性插值中间所有空桶 */
            const int span = next - prev;
            for (int k = prev + 1; k < next; k++) {
                const float t = (float)(k - prev) / (float)span;
                a[k] = a[prev] + (a[next] - a[prev]) * t;
                if (has_b && b != NULL) {
                    b[k] = b[prev] + (b[next] - b[prev]) * t;
                }
            }
        } else if ((j - prev) <= 2) {
            /* 右端（最近）无数据：仅末端 2 桶前向保持，避免假曲线 */
            a[j] = a[prev];
            if (has_b && b != NULL) {
                b[j] = b[prev];
            }
        }
    }
}

/** 4 点滑动平均的历史值（仅采样线程访问） */
static float s_mavg_v[PM_WAVE_MAVG_N]; /**< 电压滑动平均历史窗口 */
static float s_mavg_i[PM_WAVE_MAVG_N]; /**< 电流滑动平均历史窗口 */
static uint32_t s_mavg_idx = 0;        /**< 滑动窗口写入位置（循环推进） */

/**
 * @brief 把一个新样本写入三级波形缓冲（含轻滤与两级降采样）
 *
 * 只被 task_sample 调用（单写者）。降采样不是"每 N 点取 1 点"，
 * 而是"每 N 点求均值再压 1 点"——既降数据量又抑制噪声。
 *
 * @param v 本样本电压（V）
 * @param i 本样本电流（A）
 */
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

    /* 本样本时间戳（ms）：三级缓冲共用同一时刻，供 UI 按真实时间对齐槽位。
     * esp_timer_get_time() 返回自启动的微秒计数（64 位单调时钟，不受任务
     * 调度影响），除以 1000 得毫秒；截成 32 位约 49 天回绕一次，
     * 槽位换算用无符号减法求"年龄"，回绕也正确。 */
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* 电能显示值快照：内部累计是 double，波形点里存 float 即可
     * （显示精度要求低，且 wave_point_t 结构紧凑省 PSRAM）。
     * 本任务是 s_energy_wh 的唯一写者，读自己写的值无需加锁 */
    const float e_now = (float)s_energy_wh;

    /* portENTER/EXIT_CRITICAL：FreeRTOS 临界区（多核版是自旋锁），
     * 进入后本核关中断、另一核进不来，保证下面的环形缓冲写与降采样
     * 累计不被读者 pm_wave_copy 打断。临界区要短，只包内存操作。 */
    portENTER_CRITICAL(&s_ring_lock);

    wave_point_t pt = { .v = fv, .i = fi, .p = fv * fi, .e = e_now, .t_ms = now_ms };
    ring_push(&s_tier[0], &pt);

    /* tier0→tier1：每 80 个 tier0 样本求均值压成 1 点（4000Hz→50Hz） */
    s_tier1_acc.v += fv;
    s_tier1_acc.i += fi;
    s_tier1_acc.p += pt.p;
    s_tier1_acc_n++;
    if (s_tier1_acc_n >= PM_TIER1_DECIM) {
        wave_point_t d = {
            .v = s_tier1_acc.v / (float)s_tier1_acc_n,
            .i = s_tier1_acc.i / (float)s_tier1_acc_n,
            .p = s_tier1_acc.p / (float)s_tier1_acc_n,
            .e = e_now,
            .t_ms = now_ms,
        };
        ring_push(&s_tier[1], &d);
        s_tier1_acc = (wave_point_t){ 0 };
        s_tier1_acc_n = 0;

        /* tier1→tier2：再每 40 个 tier1 点均值压 1 点（50Hz→1.25Hz） */
        s_tier2_acc.v += d.v;
        s_tier2_acc.i += d.i;
        s_tier2_acc.p += d.p;
        s_tier2_acc_n++;
        if (s_tier2_acc_n >= PM_TIER2_DECIM) {
            wave_point_t d2 = {
                .v = s_tier2_acc.v / (float)s_tier2_acc_n,
                .i = s_tier2_acc.i / (float)s_tier2_acc_n,
                .p = s_tier2_acc.p / (float)s_tier2_acc_n,
                .e = e_now,
                .t_ms = now_ms,
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

/**
 * @brief 启动时从 NVS 恢复电能/运行时间与标定，读不到就用默认值
 *
 * 读取流程都是"打不开/读失败就用默认值"的降级策略：
 * 首次开机没有数据是正常情况，不应报错阻塞启动。
 */
static void load_persist_and_calib(void)
{
    /* 先装默认值，NVS 失败时直接沿用（单出口，避免到处写 fallback） */
    s_energy_wh = 0.0;
    s_runtime_s = 0;

    s_calib.voltage_gain = PM_DEFAULT_V_GAIN;
    s_calib.voltage_offset = 0.0f;
    s_calib.current_gain = PM_DEFAULT_I_GAIN;
    s_calib.current_offset = 0.0f;

    nvs_handle_t h;
    /* nvs_open：打开命名空间句柄；NVS_READONLY 表示只读。
     * 首次开机该命名空间不存在会返回错误，属正常路径。 */
    if (nvs_open(PM_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 无历史数据，电能/运行时间从 0 开始，使用默认理论标定");
        return;
    }

    /* nvs_get_blob：按键读出一块任意二进制数据。
     * len 传入期望长度做校验：长度或魔数对不上说明是旧版本格式，丢弃。 */
    pm_persist_t blob;
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, PM_NVS_KEY_DATA, &blob, &len) == ESP_OK && len == sizeof(blob) &&
        blob.magic == PM_NVS_MAGIC) {
        /* NVS 里存的是 float（兼容旧布局），载入到 double 继续累计 */
        s_energy_wh = (double)blob.energy_wh;
        s_runtime_s = blob.runtime_s;
        ESP_LOGI(TAG, "已恢复电能 %.6f Wh / 运行时间 %u s", (double)blob.energy_wh,
                 (unsigned)blob.runtime_s);
    }

    /* 权威 double 键优先（B11）：pmdw/pmah 存在则覆盖 float 恢复值，
     * 消除 >1024Wh 量级落盘/恢复的 float 舍入偏差；缺失（首版固件/升级首启）
     * 时保持上面 float 恢复值或 0。 */
    double e_hi = 0.0, c_hi = 0.0;
    size_t dlen = sizeof(e_hi);
    if (nvs_get_blob(h, PM_NVS_KEY_ENERGY_HI, &e_hi, &dlen) == ESP_OK && dlen == sizeof(e_hi)) {
        s_energy_wh = e_hi;
    }
    dlen = sizeof(c_hi);
    if (nvs_get_blob(h, PM_NVS_KEY_CAP_HI, &c_hi, &dlen) == ESP_OK && dlen == sizeof(c_hi)) {
        s_capacity_ah = c_hi;
    }
    ESP_LOGI(TAG, "已恢复容量 %.6f Ah", s_capacity_ah);

    /* 标定参数独立存放（键 "calib"），允许只更新其一 */
    size_t clen = sizeof(s_calib);
    if (nvs_get_blob(h, "calib", &s_calib, &clen) == ESP_OK && clen == sizeof(s_calib)) {
        ESP_LOGI(TAG, "已加载标定（V gain=%.6e off=%.6f / I gain=%.6e off=%.6f）",
                 (double)s_calib.voltage_gain, (double)s_calib.voltage_offset,
                 (double)s_calib.current_gain, (double)s_calib.current_offset);
    }
    /* nvs_close：关闭句柄。NVS 内部有磨损均衡，读完即关是好习惯 */
    nvs_close(h);
}

/**
 * @brief 把电能/运行时间与标定写入 NVS
 *
 * 【并发修复 P1】本函数过去被三个上下文并发调用（task_sample 清零、
 * task_aux 周期落盘、UI 核 set_calibration），nvs_set_blob/commit 交错会
 * 互相覆盖，还直接裸读共享数据。两种修法的取舍：
 *   方案 A（选用）——模块级互斥锁把"快照 + 落盘"整段串行化；
 *   方案 B ——全部改成"待落盘"标志，只由 task_aux 一个任务落盘。
 * 选 A 的原因：电能清零已按 P1 要求改为"待落盘标志 + task_aux 落盘"
 * （采样任务绝不碰 NVS），剩余调用方只有 task_aux 与 set_calibration；
 * 保持 set_calibration"调用即持久化"的语义（标定值珍贵，掉电不该丢），
 * 再用 s_save_mutex 串行化两个调用方，改动最小且无语义回退。
 *
 * 锁纪律（重要）：共享数据只在**拷贝进栈上局部变量的瞬间**持
 * s_persist_mutex / s_calib_mutex；NVS 写入（可达数百 ms）期间只持
 * s_save_mutex（只有落盘方竞争它），绝不把数据锁带进 flash 操作——
 * 否则采样任务会在块边界被卡住几百 ms。
 *
 * 调用点：set_calibration（UI 核）、task_aux（清零待落盘 / 30s 周期）。
 * NVS 写入是对 flash 的真实擦写，不能每个样本都调（寿命与耗时都不允许）。
 */
static void save_persist(void)
{
    /* 落盘互斥：把并发的 NVS set/commit 整段串行化 */
    xSemaphoreTake(s_save_mutex, portMAX_DELAY);

    /* 先把共享数据快照到栈上局部变量，随即放锁——
     * 后面的 NVS 写入用的都是这份快照，与生产者互不干扰 */
    pm_persist_t blob = { .magic = PM_NVS_MAGIC };
    power_measure_calib_t calib_snap;
    double e_hi = 0.0, c_hi = 0.0;
    xSemaphoreTake(s_persist_mutex, portMAX_DELAY);
    blob.energy_wh = (float)s_energy_wh; /* double→float：NVS 布局保持兼容 */
    blob.runtime_s = s_runtime_s;
    e_hi = s_energy_wh; /* 权威 double（B11） */
    c_hi = s_capacity_ah;
    xSemaphoreGive(s_persist_mutex);
    xSemaphoreTake(s_calib_mutex, portMAX_DELAY);
    calib_snap = s_calib;
    xSemaphoreGive(s_calib_mutex);

    nvs_handle_t h;
    /* NVS_READWRITE：以读写方式打开命名空间，随后才允许 set */
    if (nvs_open(PM_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败，本次电能/运行时间未落盘");
        /* 失败保留脏标记：task_aux 下个周期会自动重试 */
        atomic_store(&s_persist_dirty, true);
        xSemaphoreGive(s_save_mutex);
        return;
    }
    /* nvs_set_blob：把整块数据写入键（先入缓存）；两个键任一失败都不 commit，
     * 避免"只写了一半"的不一致状态 */
    esp_err_t e1 = nvs_set_blob(h, PM_NVS_KEY_DATA, &blob, sizeof(blob));
    esp_err_t e2 = nvs_set_blob(h, "calib", &calib_snap, sizeof(calib_snap));
    esp_err_t e3 = nvs_set_blob(h, PM_NVS_KEY_ENERGY_HI, &e_hi, sizeof(e_hi));
    esp_err_t e4 = nvs_set_blob(h, PM_NVS_KEY_CAP_HI, &c_hi, sizeof(c_hi));
    if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && e4 == ESP_OK) {
        /* nvs_commit：提交后数据才真正落到 flash；不调它等于白写 */
        nvs_commit(h);
    } else {
        /* 任一键写失败同样留给下个周期重试 */
        atomic_store(&s_persist_dirty, true);
    }
    nvs_close(h);
    xSemaphoreGive(s_save_mutex);
}

/* ============================================================
 * 采样任务（Core1，4kHz，DRDY 自节奏）
 * ============================================================ */
#define PM_SAMPLE_STACK   4096  /**< 采样任务栈（IDF 中单位是字节，不是 FreeRTOS 原生的"字"） */
#define PM_SAMPLE_PRIO    24    /**< 采样任务优先级：全系统最高（合法上限 24，FreeRTOS
                                 *  configMAX_PRIORITIES=25 即 0~24；M1 基线兜底路线，
                                 *  保证 DRDY→读帧启动延迟 δ 抖动最小、不被 UI 抢占丢帧） */
#define PM_AUX_STACK      6144  /**< 辅助任务栈（I2C/浮点日志用得多，给大些） */
#define PM_AUX_PRIO       5     /**< 辅助任务优先级：低，慢 I2C 不该抢测量 */
#define PM_AUX_PERIOD_MS  2000  /**< 温湿度/电池采集周期（ms） */
#define PM_SAVE_PERIOD_TICKS 15u /* 辅助任务每 15 拍（30s）落一次盘 */
#define PM_ADC_FAULT_AFTER_FAILS 50u /**< 连续读**超时**多少次判定 ADC 故障（B8 分流后只计真故障；
                                      *  50 次 × 最坏 400ms ≈ 5~20s 内上报，替代原 2000 次的分钟级延迟） */
#define PM_DIAG_PERIOD_BLOCKS 25u /* 每 25 块（5s）打印一次诊断 */

/* 快速显示子窗口周期（微秒）：**墙钟时间驱动**，与实际 ODR 解耦。
 * 2026-10 排查记录：当时 CLOCK 还是 OSR=1024 档（标称 4kHz），DRDY 实际
 * 只有 ~310-320Hz 且抖动——根因是 DRDY 抑制机制（SBAS853 §8.5.1.5：读帧
 * 期间完成的转换其 DRDY 被吞），并非 MCLK/晶振异常。原方案"每 100 个
 * 有效样本开一窗"在 310Hz 下被拖到 ~2.6Hz（即用户所见"首页刷新率低/
 * 数据不动"的根源）。改为每 25ms 强制输出一帧即时均值：4000Hz 时约
 * 100 样本/帧，UI 恒定 40Hz 有数据；且样本数波动/丢帧不影响刷新节奏。 */
#define PM_DISPLAY_SUBWINDOW_US 25000ull

/**
 * @brief 采样主任务（Core1）：由 ADS131M02 DRDY 节奏驱动的测量主链路
 *
 * 每个有效样本：帧校验 → 标定换算 → 波形缓冲 → 快速显示帧（40Hz）→
 * 块统计累加；每 200ms 块边界：块均值、电能积分、运行时间、诊断日志。
 * 本任务是电能值与 data_store 的唯一写者（见文件头取舍 2/3）。
 *
 * @param arg 未使用（FreeRTOS 任务入口固定签名）
 */
static void task_sample(void *arg)
{
    (void)arg;

    /* ---- 本任务的栈上局部状态（单任务独占，无需加锁）---- */
    power_calc_channel_calib_t vcal = { 0 }, ical = { 0 }; /**< 换算用标定副本 */
    double sum_v = 0, sum_i = 0, sum_p = 0;   /**< 块内累加（用 double 防长块累加丢精度） */
    uint32_t block_cnt = 0;                   /**< 块内有效样本数 */
    uint32_t fail_streak = 0;                 /**< ADC 连续读**超时**计数（真故障候选） */
    uint32_t frame_drop = 0;                  /**< 槽满丢帧计数（B8 分流：瞬态，不计故障） */
    bool fault_reported = false;              /**< ADC 故障是否已上报 system_state */
    uint32_t sec_acc_ms = 0; /**< 运行时间累积（毫秒，满 1000 进 1 秒） */

    /* 诊断统计 */
    uint32_t diag_blocks = 0;                 /**< 距上次诊断打印的块数 */
    uint32_t frame_total = 0, frame_reject = 0; /**< 帧总数 / 被拒帧数（算丢弃率） */
    bool reject_check_ok = true; /* 判据是否可用（丢弃率过高则关闭） */
    bool crc_check_enabled = true; /* CRC 帧校验（自校准：失败率过高回退 STATUS 位判据） */
    int32_t raw_v_min = 0, raw_v_max = 0, raw_i_min = 0, raw_i_max = 0; /**< 原始码极值 */
    double raw_v_sum = 0, raw_i_sum = 0; /**< 原始码累加（诊断算均值用） */
    uint32_t raw_n = 0;                  /**< 原始码样本数 */

    /* 显示平滑（块值的低通） */
    float disp_v = 0.0f, disp_i = 0.0f, disp_p = 0.0f;
    bool disp_init = false; /**< 首块直接赋值，避免从 0 慢爬升 */

    /* 快速显示子窗口累加（仅用于 40Hz 即时显示帧） */
    double qsum_v = 0.0, qsum_i = 0.0, qsum_p = 0.0;
    uint32_t qcnt = 0;
    float qmin_v = 0, qmax_v = 0, qmin_i = 0, qmax_i = 0, qmin_p = 0, qmax_p = 0; /**< 掐头去尾：窗口内极值 */
    int64_t sub_start_us = esp_timer_get_time(); /**< 本子窗口起点墙钟（µs） */
    /* 块内电能逐步积分（让 40Hz 帧的电能读数也能连续走动；块末以权威值覆写） */
    double block_energy_wh = 0.0;
    /* 块起点墙钟：dt 用真实时间而非「样本数/标称率」——
     * 2026-10 实测：4kHz 档下 DRDY 被抑制到 ~310Hz（SBAS853 §8.5.1.5，
     * 见 ads131m02.c 的 ADS_CLOCK_VALUE 注释），按样本数折算 dt 会把
     * 电能/运行时间算错约 13 倍。真实墙钟对任何实际帧率都正确。 */
    int64_t block_start_us = esp_timer_get_time();
    /* 诊断：上一条诊断打印的墙钟与 DRDY 计数 */
    int64_t diag_last_us = block_start_us;
    uint32_t diag_last_drdy = ads131m02_drdy_count(); /**< DRDY 中断累计数（测实际帧率） */

    ESP_LOGI(TAG, "采样任务启动（Core1，标称 %u SPS，块周期 %u ms（墙钟），V gain=%.6e I gain=%.6e）",
             PM_SAMPLE_RATE_HZ, PM_BLOCK_PERIOD_MS,
             (double)PM_DEFAULT_V_GAIN, (double)PM_DEFAULT_I_GAIN);

    for (;;) {
        /* ads131m02_read：读一帧 ADC 数据（内部在 DRDY 事件上等待，
         * 即"自节奏"——由 ADC 数据就绪节拍驱动循环，而非定时器硬凑频率） */
        /* M3 帧错位自恢复：驱动检测到连续 CRC 失败会置重同步请求，
         * 在本循环（无锁上下文）消费执行「RESET# + 重配 + 恢复流式」。
         * 不能放进 ads131m02_read（其持总线锁，重同步内部寄存器访问自带
         * 同一把锁会死锁），故固定在每轮循环开头消费。 */
        (void)ads131m02_resync_if_needed();

        ads131m02_sample_t raw;
        esp_err_t err = ads131m02_read(&raw);
        if (err == ESP_ERR_INVALID_STATE) {
            /* 【B8 故障分流】槽满丢帧：瞬态事件（下一帧照常），只计数不上报
             * 故障、不进 fail_streak——丢帧与"ADC 真死"是两回事。 */
            frame_drop++;
            continue;
        }
        if (err != ESP_OK) {
            /* 等 DRDY 超时 = 真故障候选。连续失败达到阈值才上报一次故障：
             * 偶发 SPI 抖动不该弹告警。 */
            if (++fail_streak == PM_ADC_FAULT_AFTER_FAILS && !fault_reported) {
                ESP_LOGE(TAG, "ADC 连续 %u 次读取失败，上报测量前端故障", (unsigned)fail_streak);
                system_state_report_fault(SYSTEM_FAULT_ADC);
                fault_reported = true;
            }
            continue;
        }
        /* 恢复后撤掉故障标志，界面红点随之消失 */
        if (fail_streak >= PM_ADC_FAULT_AFTER_FAILS && fault_reported) {
            system_state_clear_fault(SYSTEM_FAULT_ADC);
            fault_reported = false;
            ESP_LOGI(TAG, "ADC 读取恢复，清除测量前端故障");
        }
        fail_streak = 0;

        /* ---- 帧有效性校验（见文件头）---- */
        frame_total++;
        /* 【2026-10 毛刺根因修复】CRC 校验为首要判据：帧错位/损坏时 CRC
         * 必不匹配（误判率 1/65536），而 STATUS 的 DRDY 位判据错位帧有 1/4
         * 概率误判为真（实测 STATUS 字出现在数据槽位造成尖峰）。
         * 自校准：CRC 失败率 >50%（如 CRC 覆盖范围理解有误）则回退旧判据。 */
        bool frame_ok;
        if (crc_check_enabled) {
            /* crc_ok 由驱动做 CRC-16/CCITT 帧校验后置位 */
            frame_ok = raw.crc_ok;
            if (!frame_ok) {
                frame_reject++;
            }
        } else {
            /* STATUS 低 2 位 = DRDY0/DRDY1，两通道都有新数据才认这帧 */
            frame_ok = ((raw.status & 0x0003u) == 0x0003u);
            if (!frame_ok) {
                frame_reject++;
            }
        }
        /* 样本足够多后检查丢弃率：判据本身失灵就降级，别把好帧全拒掉 */
        if (reject_check_ok && frame_total >= 400u && frame_reject * 2u > frame_total) {
            if (crc_check_enabled) {
                /* CRC 判据失效（超过半数帧校验失败）：回退 STATUS 位判据并告警 */
                crc_check_enabled = false;
                frame_total = 0;
                frame_reject = 0;
                ESP_LOGW(TAG, "CRC 判据失效（丢弃率 >50%），回退 STATUS 位判据；"
                              "请核对 CRC 覆盖范围/多项式是否与芯片一致");
            } else {
                reject_check_ok = false;
                ESP_LOGW(TAG, "STATUS 的 DRDY 位判据不可用（丢弃率 %u/%u），已关闭帧过滤，"
                              "请检查 STATUS 字读取是否与帧同步",
                         (unsigned)frame_reject, (unsigned)frame_total);
            }
        }
        if (reject_check_ok && !frame_ok) {
            continue; /* 丢弃不可信帧，不参与统计 */
        }

        /* 读标定副本：UI 核随时可能改标定，互斥锁保证不读到写一半的值。
         * portMAX_DELAY = 一直等到拿到锁为止（FreeRTOS 互斥锁语义） */
        xSemaphoreTake(s_calib_mutex, portMAX_DELAY);
        vcal.scale = s_calib.voltage_gain;
        vcal.offset = s_calib.voltage_offset;
        ical.scale = s_calib.current_gain;
        ical.offset = s_calib.current_offset;
        xSemaphoreGive(s_calib_mutex); /* 用完立即还锁，别把锁带进下面的计算 */

        /* 码值 → 物理量（纯函数，见 power_calc.h） */
        float v = power_calc_raw_to_value(raw.voltage_raw, &vcal);
        float i = power_calc_raw_to_value(raw.current_raw, &ical);
        float p = v * i;

        wave_push_sample(v, i);

        /* 块统计累加（200ms 块末统一结算） */
        sum_v += v;
        sum_i += i;
        sum_p += (double)p;

        /* ---- 40Hz 快速显示快照（墙钟驱动，不触碰块级统计）---- */
        qsum_v += v;
        qsum_i += i;
        qsum_p += (double)p;
        qcnt++;
        /* 掐头去尾：窗口内最大最小值各去一个再取均值，抵御 ADC 坏帧尖峰
         * （实测：稳定 10V 下电压码值偶发近零尖峰，单帧即可拉偏显示值 12%，
         * 导致"几秒跳一次"的观感）。只对显示快照生效，统计/电能不受影响。
         * V/I/P 三个通道做**同一套**处理（含 P）：若只对 V/I 掐头去尾，
         * 显示的 V×I 与 P 会对不上（P 里还含着被 V/I 剔掉的坏帧尖峰），
         * 观感自洽性被破坏。 */
        if (qcnt == 1) {
            qmin_v = qmax_v = v;
            qmin_i = qmax_i = i;
            qmin_p = qmax_p = p;
        } else {
            if (v < qmin_v) qmin_v = v;
            if (v > qmax_v) qmax_v = v;
            if (i < qmin_i) qmin_i = i;
            if (i > qmax_i) qmax_i = i;
            if (p < qmin_p) qmin_p = p;
            if (p > qmax_p) qmax_p = p;
        }
        const int64_t sub_now_us = esp_timer_get_time();
        if (qcnt > 0 && (uint64_t)(sub_now_us - sub_start_us) >= PM_DISPLAY_SUBWINDOW_US) {
            /* 掐头去尾均值：样本数 ≥3 时去最大最小各一个，抵御单个坏帧尖峰；
             * 样本不足 3 时退化为普通均值。 */
            float qv, qi, qp;
            if (qcnt >= 3) {
                qv = (float)((qsum_v - qmax_v - qmin_v) / (double)(qcnt - 2));
                qi = (float)((qsum_i - qmax_i - qmin_i) / (double)(qcnt - 2));
                qp = (float)((qsum_p - qmax_p - qmin_p) / (double)(qcnt - 2));
            } else {
                qv = (float)(qsum_v / qcnt);
                qi = (float)(qsum_i / qcnt);
                qp = (float)(qsum_p / qcnt);
            }
            /* 子窗口真实时长（墙钟）：帧率非标称值时按样本数折算会错 */
            const float sub_dt_s = (float)(sub_now_us - sub_start_us) / 1e6f;
            sub_start_us = sub_now_us;
            /* 本子窗口对块内电能的贡献（Wh）；块末权威积分以墙钟为准。
             * 直接用 double 矩形积分（W·s → Wh 除 3600，与 power_calc 同公式）：
             * power_calc_energy_wh 是 float 接口，用它会把电能精度又拉回 float */
            block_energy_wh += (double)qp * (double)sub_dt_s / 3600.0;

            /* 拼一帧快照：显示值 + 电能（块内连续积分）+ task_aux 的环境量。
             * 电能 = 块级权威累计值 + 本块内临时积分，
             * 这样 40Hz 刷新的电能读数也平滑走动，不等 200ms 跳一次。
             * 持 s_persist_mutex 快照权威值（写者是本任务块边界，
             * double 需两拍写，锁保证读到完整值，代价仅几条指令） */
            xSemaphoreTake(s_persist_mutex, portMAX_DELAY);
            const double energy_base = s_energy_wh;
            const double cap_base = s_capacity_ah;
            const uint32_t runtime_base = s_runtime_s;
            xSemaphoreGive(s_persist_mutex);
            data_store_snapshot_t qsnap = {
                .voltage_v = qv,
                .current_a = qi,
                .power_w = qp,
                .energy_wh = (float)(energy_base + block_energy_wh),
                /* v0.2.0 新字段（F1/F2/F3）：容量 + 峰谷 + 当前告警 */
                .capacity_ah = (float)cap_base,
                .v_max = s_v_max,
                .v_min = s_v_min,
                .i_max = s_i_max,
                .i_min = s_i_min,
                .p_max = s_p_max,
                .p_min = s_p_min,
                .alert_mask = s_alert_mask,
            };
            /* 借辅助数据的互斥锁读温湿度/电池（task_aux 是写者）。
             * 【B19 修复】温湿度未读到有效值时写 NaN（而非 0）——
             * 真实的 0℃/0%RH 与"无数据"必须可区分，UI 按 NaN 显示占位符。 */
            xSemaphoreTake(s_aux_data.mutex, portMAX_DELAY);
            qsnap.temperature_c = s_aux_data.temp_valid ? s_aux_data.temp_c : NAN;
            qsnap.humidity_pct = s_aux_data.temp_valid ? s_aux_data.humidity_pct : NAN;
            qsnap.battery_mv = s_aux_data.battery_mv;
            /* SOC 结温：与温湿度同样用 NaN 表示"暂无数据"（v0.5.1） */
            qsnap.soc_temp_c = s_aux_data.soc_temp_valid ? s_aux_data.soc_temp_c : NAN;
            xSemaphoreGive(s_aux_data.mutex);
            qsnap.runtime_s = runtime_base;
            /* data_store_update：写 UI 读取的显示快照（本任务是唯一写者） */
            data_store_update(&qsnap);

            /* 清空子窗口累加，开下一窗 */
            qsum_v = qsum_i = qsum_p = 0.0;
            qcnt = 0;
            qmin_v = qmax_v = qmin_i = qmax_i = qmin_p = qmax_p = 0.0f;
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

            /* ---- 峰/谷值更新（F1）：块均值口径，首次块用首块初始化 ---- */
            if (!s_peak_init) {
                s_v_max = s_v_min = v_mean;
                s_i_max = s_i_min = i_mean;
                s_p_max = s_p_min = p_mean;
                s_peak_init = true;
            } else {
                if (v_mean > s_v_max) s_v_max = v_mean;
                if (v_mean < s_v_min) s_v_min = v_mean;
                if (i_mean > s_i_max) s_i_max = i_mean;
                if (i_mean < s_i_min) s_i_min = i_mean;
                if (p_mean > s_p_max) s_p_max = p_mean;
                if (p_mean < s_p_min) s_p_min = p_mean;
            }

            /* ---- 告警判定（F3）：块边界消费 settings 阈值 ----
             * 上升沿（mask 由 0 变非 0）发 APP_EVENT_MEASURE_ALERT 事件
             * （device_ctrl 订阅转蜂鸣，UI 订阅弹窗）；持续越限维持 mask，
             * 恢复后清 0。event_bus_post 超时传 0：不阻塞采样任务。 */
            {
                settings_alarm_t alm;
                uint32_t new_mask = 0;
                if (settings_get_alarm(&alm) == ESP_OK) {
                    if (alm.v_en && v_mean > alm.v_limit) {
                        new_mask |= DATA_ALERT_OVP;
                    }
                    if (alm.i_en && i_mean > alm.i_limit) {
                        new_mask |= DATA_ALERT_OCP;
                    }
                    if (alm.p_en && p_mean > alm.p_limit) {
                        new_mask |= DATA_ALERT_OPP;
                    }
                }
                /* 【v0.6.4 需求 3】过温告警：机内温度（SOC 结温）> 60℃ 即置位。
                 * 恒定生效（不读使能开关）：前三项是用户可关的"测量告警"，
                 * 过温是硬件保护——关乎主控寿命与电池安全，不允许关闭。
                 * · 数据源：pm_aux 每 2s 更新的 s_aux_data.soc_temp_c（SOC
                 *   内部温度传感器）；未读到有效值（soc_temp_valid=false，
                 *   如温度驱动初始化失败）时不判，避免拿 0/NaN 误报。
                 * · 阈值 60℃：S3 结温上限约 105℃，60℃ 预警留出处理余量；
                 *   置位后设备端弹"机内温度"告警框、网页端横幅显示"过温"。
                 * · 读侧竞态说明：soc_temp_valid 在 soc_temp_c 之后写入
                 *   （见 task_aux），最坏读到旧一两拍的温度，告警延迟 ≤2s，
                 *   对 60℃ 级预警完全可接受。 */
                if (s_aux_data.soc_temp_valid && s_aux_data.soc_temp_c > 60.0f) {
                    new_mask |= DATA_ALERT_OTP;
                }
                if (new_mask != 0 && new_mask != s_alert_mask) {
                    /* 上升沿（含换项）：广播告警事件（不阻塞；丢弃可观测） */
                    app_event_t ev = {
                        .type = APP_EVENT_MEASURE_ALERT,
                        .data.alert = {
                            .voltage_v = v_mean,
                            .current_a = i_mean,
                            .alert_mask = new_mask,
                        },
                    };
                    (void)event_bus_post(&ev, 0);
                }
                s_alert_mask = new_mask;
            }

            /* 显示平滑：一阶低通（EMA），避免数值高频跳动。
             * alpha 越小越平滑、响应越慢，0.25 约 0.8s 时间常数。 */
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
             * atomic_exchange：原子地读出旧值并写入 false，多核下无撕裂、
             * 不会漏掉或重复消费请求（stdatomic.h，C11 原子操作）。
             * 【并发修复 P1】这里**只改内存并置「待落盘」标志，绝不做 NVS 落盘**：
             * nvs_commit 可达数百 ms，若在采样任务里同步做，这期间 DRDY 帧
             * 全部被丢（本任务是 Core1 高优先级，没人能替它收帧）。
             * 真正落盘由 task_aux（Core0）在其节拍消费 s_persist_dirty 完成
             * （≤2s 内，无须等满 30s 周期），最坏掉电多丢这几秒，是换实时性的折中。 */
            if (atomic_exchange(&s_reset_energy_req, false)) {
                xSemaphoreTake(s_persist_mutex, portMAX_DELAY);
                s_energy_wh = 0.0;
                s_capacity_ah = 0.0;
                s_runtime_s = 0;
                xSemaphoreGive(s_persist_mutex);
                sec_acc_ms = 0;
                block_energy_wh = 0.0;
                atomic_store(&s_persist_dirty, true);
                ESP_LOGI(TAG, "电能与运行时间已清零（落盘交由辅助任务）");
            }

            /* 电能矩形积分：dt 用本块**真实墙钟时长**
             * （实际帧率 ≠ 标称值时（如 DRDY 被抑制、丢帧），按样本数
             *   折算会系统性算错；墙钟自动覆盖任何实际帧率）。
             * 【数值修复 P2】内部用 double 累计：float 超约 1024 Wh 后
             * 增量被舍入吞掉、电能不再走动（见 s_energy_wh 的注释） */
            const float dt_s = (float)(block_now_us - block_start_us) / 1e6f;
            block_start_us = block_now_us;
            xSemaphoreTake(s_persist_mutex, portMAX_DELAY);
            s_energy_wh += (double)p_mean * (double)dt_s / 3600.0;
            /* 容量积分（F2）：A·s → Ah，与电能并列、同用墙钟 dt */
            s_capacity_ah += (double)i_mean * (double)dt_s / 3600.0;

            /* 运行时间：按实际采样时间累加（每满 1s 进 1） */
            sec_acc_ms += (uint32_t)(dt_s * 1000.0f + 0.5f);
            if (sec_acc_ms >= 1000u) {
                s_runtime_s += sec_acc_ms / 1000u;
                sec_acc_ms %= 1000u;
            }
            xSemaphoreGive(s_persist_mutex);

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
                /* 墙钟差 + DRDY 计数差 → 实测帧率，可直接看出 ADC 时钟异常 */
                const uint32_t diag_wall_ms = (uint32_t)((diag_now_us - diag_last_us) / 1000);
                const uint32_t drdy_delta = drdy_now - diag_last_drdy;
                diag_last_us = diag_now_us;
                diag_last_drdy = drdy_now;
                ESP_LOGI(TAG, "诊断: wall=%ums drdy+%u(%.0fHz) 帧=%u 丢弃=%u(%.1f%%) 槽满丢帧=%u | "
                              "原始码 V[min=%ld max=%ld 均值=%.0f] I[min=%ld max=%ld 均值=%.0f] | "
                              "换算 V=%.5fV I=%.5fA P=%.5fW",
                         (unsigned)diag_wall_ms, (unsigned)drdy_delta,
                         diag_wall_ms ? (1000.0 * (double)drdy_delta / (double)diag_wall_ms) : 0.0,
                         (unsigned)frame_total, (unsigned)frame_reject,
                         frame_total ? (100.0f * (float)frame_reject / (float)frame_total) : 0.0f,
                         (unsigned)frame_drop,
                         (long)raw_v_min, (long)raw_v_max, raw_n ? (raw_v_sum / raw_n) : 0.0,
                         (long)raw_i_min, (long)raw_i_max, raw_n ? (raw_i_sum / raw_n) : 0.0,
                         (double)disp_v, (double)disp_i, (double)disp_p);
                frame_total = 0;
                frame_reject = 0;
                frame_drop = 0;
                raw_n = 0;
                raw_v_sum = raw_i_sum = 0;
            }

            /* 块统计归零，开下一块 */
            sum_v = sum_i = sum_p = 0;
            block_cnt = 0;
            block_energy_wh = 0.0; /* 块末权威电能已写入，子窗口积分归零 */
        }
    }
}

/**
 * @brief 辅助采集任务（Core0）：温湿度/电池 2s 一次；每 30s 落一次盘
 *
 * 放在 Core0（跑 UI 的交互核）是因为 AHT20 走 I2C，一次读取要阻塞
 * ≥80ms 等传感器转换；若放进 Core1 采样任务会拖垮测量节拍。
 *
 * @param arg 未使用（FreeRTOS 任务入口固定签名）
 */
static void task_aux(void *arg)
{
    (void)arg;
    /* vTaskDelayUntil 需要的"上次唤醒时刻"基准 */
    TickType_t last = xTaskGetTickCount(); /* 读当前 tick 计数（tick=1ms） */
    uint32_t ticks = 0;   /**< 距上次落盘的周期数 */
    uint32_t aht_fail = 0; /**< AHT20 连续失败计数（做节流日志） */

    for (;;) {
        /* vTaskDelayUntil：周期延时到「上次唤醒 + 2s」。
         * 比 vTaskDelay 更准：任务执行耗时不会累积漂移。
         * pdMS_TO_TICKS 把毫秒换算成系统 tick。 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(PM_AUX_PERIOD_MS));

        aht20_reading_t env;
        esp_err_t aerr = aht20_read(&env);
        if (aerr == ESP_OK) {
            if (aht_fail != 0) {
                ESP_LOGI(TAG, "AHT20 读取恢复正常（此前连续失败 %u 次）", (unsigned)aht_fail);
                aht_fail = 0;
            }
            /* 写辅助数据要持锁（采样任务并发读同一结构体） */
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

        /* SOC 结温（v0.5.1）：读芯片内部温度传感器，经 aux 互斥锁写入。
         * temperature_sensor_get_celsius 内部是短暂的寄存器读+换算（微秒级），
         * 放在本任务（Core0，2s 周期）足够，且**不得在中断中调用**。
         * 句柄为 NULL（安装失败）时跳过，保持 soc_temp_valid=false →
         * 快照写 NaN，上层按"无数据"显示，不影响其他功能。 */
        if (s_soc_tsens != NULL) {
            float soc_t = 0.0f;
            if (temperature_sensor_get_celsius(s_soc_tsens, &soc_t) == ESP_OK) {
                xSemaphoreTake(s_aux_data.mutex, portMAX_DELAY);
                s_aux_data.soc_temp_c = soc_t;
                s_aux_data.soc_temp_valid = true;
                xSemaphoreGive(s_aux_data.mutex);
            } else {
                /* 超量程等情况按普通日志记录（2s 一次不会刷屏） */
                ESP_LOGW(TAG, "SOC 温度读取失败（超出传感器量程？）");
            }
        }

        /* 落盘节拍：每 30s（15 周期）**或**有「待落盘」改动时写一次 NVS。
         * 写 flash 有寿命与耗时成本（nvs_commit 可达数百 ms），所以：
         *   · 周期落盘——电能/运行时间持续在涨，需要定期持久化，
         *     掉电最多丢 30s 数据，是刻意的折中；
         *   · 待落盘标志——电能清零这类"用户动作"必须尽快持久化，
         *     否则掉电后清零失效、旧值复活。采样任务清零时只改内存并置
         *     s_persist_dirty（见 task_sample 块边界注释），电能/运行时间的
         *     落盘统一在本任务执行（标定落盘由 set_calibration 直接触发，
         *     但经 s_save_mutex 与本任务串行，NVS 不会交错写坏）。 */
        if (++ticks >= PM_SAVE_PERIOD_TICKS) {
            ticks = 0;
            /* 周期落盘写的就是最新快照，顺手消费掉待落盘标志
             * （save_persist 失败时会把标志重新置位，重试不丢） */
            (void)atomic_exchange(&s_persist_dirty, false);
            save_persist();
        } else if (atomic_exchange(&s_persist_dirty, false)) {
            save_persist();
        }
    }
}

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 初始化测量服务：分配互斥锁、恢复 NVS 数据、分配 PSRAM 波形缓冲
 *
 * 不创建任务、不访问 ADC（采样在 power_measure_start 后开始）。
 * 任一步失败都返回错误码，且不置 s_inited，可修复后重试。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 重复初始化；
 *         ESP_ERR_NO_MEM 互斥锁或 PSRAM 分配失败
 */
esp_err_t power_measure_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* xSemaphoreCreateMutex：创建 FreeRTOS 互斥锁（空闲即"可用"）。
     * 多核 ESP32 上两个任务分属两个核，共享数据必须互斥访问。 */
    s_calib_mutex = xSemaphoreCreateMutex();
    s_aux_data.mutex = xSemaphoreCreateMutex();
    s_persist_mutex = xSemaphoreCreateMutex();
    s_save_mutex = xSemaphoreCreateMutex();
    if (s_calib_mutex == NULL || s_aux_data.mutex == NULL || s_persist_mutex == NULL ||
        s_save_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    load_persist_and_calib();

    /* ---- 芯片内部温度传感器（SOC 结温，v0.5.1）----
     * 容错策略：安装/使能失败不致命——本功能只是"锦上添花"的信息展示，
     * 不影响测量主链路；失败后句柄保持 NULL，上层按 NaN 显示"无数据"。
     * TEMPERATURE_SENSOR_CONFIG_DEFAULT(min, max)：给出期望量程，
     * 驱动据此选内部换算曲线；-10~80℃ 覆盖本设备的实际工作范围。 */
    {
        temperature_sensor_config_t ts_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t terr = temperature_sensor_install(&ts_cfg, &s_soc_tsens);
        if (terr != ESP_OK) {
            ESP_LOGW(TAG, "SOC 温度传感器安装失败: %s（不影响测量，网页端将显示无数据）",
                     esp_err_to_name(terr));
            s_soc_tsens = NULL;
        } else if ((terr = temperature_sensor_enable(s_soc_tsens)) != ESP_OK) {
            ESP_LOGW(TAG, "SOC 温度传感器使能失败: %s（不影响测量，网页端将显示无数据）",
                     esp_err_to_name(terr));
            (void)temperature_sensor_uninstall(s_soc_tsens);
            s_soc_tsens = NULL;
        } else {
            ESP_LOGI(TAG, "SOC 温度传感器已启用（量程 -10~80℃）");
        }
    }

    /* 三级波形缓冲：容量大（合计约 1.5MB），必须放 PSRAM
     * （heap_caps_malloc + MALLOC_CAP_SPIRAM 只从外部 SPI RAM 分配，
     *  片内 RAM 留给栈和 DMA；失败返回 NULL，本模块选择整体失败）。 */
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

/**
 * @brief 启动采样任务（绑定 Core 1）与辅助采集任务（Core 0）
 *
 * 两个任务创建后立即开始运行，无需再触发。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化/已启动；ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t power_measure_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* xTaskCreatePinnedToCore：FreeRTOS 创建任务并绑定到指定 CPU 核。
     * 参数依次是(入口函数, 任务名, 栈字节数(IDF 语义), 传参, 优先级, 句柄出参, 核号)。
     * core=1 即 APP_CPU（PRO_CPU 是 0）：采样绑 Core1 不受 UI 卡顿干扰；
     * 辅助任务（慢 I2C）绑 Core0，与 UI 同核，绝不拖慢测量。 */
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

/**
 * @brief 更新标定参数（立即生效，并持久化到 NVS）
 *
 * 校验增益必须为正（负增益多半是标定流程算错，直接拒绝）。
 *
 * @param calib 新标定参数，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t power_measure_set_calibration_deferred(const power_measure_calib_t *calib)
{
    if (calib == NULL || calib->voltage_gain <= 0.0f || calib->current_gain <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 持锁改标定：采样任务每样本都在读这份数据 */
    xSemaphoreTake(s_calib_mutex, portMAX_DELAY);
    s_calib = *calib;
    xSemaphoreGive(s_calib_mutex);

    /* 【v0.6.0 卡死修复】只置"待落盘"标志，**不在调用者上下文写 NVS**。
     * task_aux 的落盘节拍会看到该标志并立即执行一次 save_persist()
     * （见 task_aux 的 `else if (atomic_exchange(&s_persist_dirty, false))` 分支），
     * 失败时 save_persist 会把标志重新置位，不会丢改动。
     * atomic_store：C11 原子写，跨核可见（本函数在 UI 核，落盘在 Core0/测量核协作）。 */
    atomic_store(&s_persist_dirty, true);
    ESP_LOGI(TAG, "标定已更新（内存即时生效，落盘由辅助任务异步完成）："
                  "V gain=%.6e off=%.6f / I gain=%.6e off=%.6f",
             (double)calib->voltage_gain, (double)calib->voltage_offset,
             (double)calib->current_gain, (double)calib->current_offset);
    return ESP_OK;
}

/**
 * @brief 更新标定参数（立即生效，并同步持久化到 NVS）
 *
 * ⚠ 同步版本：会在**调用者上下文**里写 NVS（可达数百 ms）。UI 事件回调
 * （持有 LVGL 锁）**不得**调用本函数，请改用 deferred 版本，否则界面会冻结。
 * 保留本函数供非 UI 上下文（如命令行/自动化）使用。
 *
 * @param calib 新标定参数，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t power_measure_set_calibration(const power_measure_calib_t *calib)
{
    esp_err_t err = power_measure_set_calibration_deferred(calib);
    if (err != ESP_OK) {
        return err;
    }
    /* 调用方明确要求"立即落盘"：这里同步执行（消费掉刚置的 dirty 标志） */
    (void)atomic_exchange(&s_persist_dirty, false);
    save_persist(); /* save_persist 内部快照共享数据 + s_save_mutex 串行落盘 */
    return ESP_OK;
}

/**
 * @brief 读取当前标定参数
 * @param out 输出，不可为 NULL
 */
void power_measure_get_calibration(power_measure_calib_t *out)
{
    if (out == NULL) {
        return;
    }
    /* 持锁整体拷贝，避免读到一半被写 */
    xSemaphoreTake(s_calib_mutex, portMAX_DELAY);
    *out = s_calib;
    xSemaphoreGive(s_calib_mutex);
}

/**
 * @brief 清零电能与运行时间统计（"本次统计"从此重新开始）
 *
 * 只置请求标志，真正的清零由采样任务在下一个 200ms 块边界执行
 * （保持电能值单写者，见文件头取舍 3）。
 *
 * @return ESP_OK 已受理（下一个块边界生效）；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t power_measure_reset_energy(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 只置请求标志，不在本核直接改电能值（跨核写会让累加中的 float 撕裂）。
     * atomic_store：C11 原子写，跨核可见且不会写出"半个值" */
    atomic_store(&s_reset_energy_req, true);
    return ESP_OK;
}

/**
 * @brief 查询采样率（Hz）
 * @return 驱动上报的实测采样率；驱动不可用时回退标称值
 */
uint32_t power_measure_sample_rate_hz(void)
{
    return ads131m02_get_sample_rate_hz() ? ads131m02_get_sample_rate_hz()
                                         : PM_SAMPLE_RATE_HZ;
}

/**
 * @brief 把显示模式映射为波形点内字段号（0=v 1=i 2=p 3=e）
 *
 * 双序列模式的 fa/fb 分别是主/副序列字段；单序列模式 has_b=false。
 * 用整数字段号而非函数指针，让 ring_fill_slots 的热循环保持简单。
 *
 * @param mode  显示模式
 * @param fa    出参：主序列字段号
 * @param fb    出参：副序列字段号（has_b=false 时无意义）
 * @param has_b 出参：是否存在副序列
 */
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

/**
 * @brief 按时间窗与模式把波形填入固定时间槽（供 lv_chart 刷新）
 *
 * 输出点按时间从旧到新排列，索引 j 即屏幕第 j 列，右端是"现在"。
 * 这是给 UI 的「一比一」映射契约，UI 无需再做右对齐/抽稀计算。
 *
 * @param win        时间窗
 * @param mode       显示模式
 * @param a          主轴序列缓冲，不可为 NULL
 * @param b          副轴序列缓冲（单序列模式可为 NULL）
 * @param max_points 槽（列）数，建议 240~480，与屏宽对齐
 * @return 有效（非 NaN）的槽位数；参数非法返回 0
 */
int pm_wave_copy(pm_wave_window_t win, pm_wave_mode_t mode, float *a, float *b, int max_points)
{
    if (a == NULL || max_points <= 0 || win >= PM_WAVE_WINDOW_MAX || mode >= PM_WAVE_MODE_MAX) {
        return 0;
    }
    const window_map_t *m = &s_window_map[win];
    int fa, fb;
    bool has_b;
    mode_fields(mode, &fa, &fb, &has_b);

    /* 填满 max_points 个固定时间槽：索引 j 即屏幕第 j 列，
     * 右端（j=max_points-1）是"现在"，无数据的槽为 NaN。
     * UI 因此可以「一比一」映射，不再需要右对齐/填充点数计算。
     * 并发保护：ring_fill_slots 内部只在短临界区里 memcpy 快照，
     * 聚合/插值都在锁外做（见其注释，临界区长会卡住 Core1 采样）。 */
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ring_fill_slots(&s_tier[m->tier], now_ms, m->win_ms, max_points, fa, fb, has_b, a, b);

    /* 统计有效槽位数返回，UI 据此判断有没有画出来的东西 */
    int valid = 0;
    for (int i = 0; i < max_points; i++) {
        if (isfinite(a[i])) {
            valid++;
        }
    }
    return valid;
}

/** 有效样本（跳过 NaN 空槽）的最小/最大值；全空返回 false */
static bool span_of(const float *v, int n, float *mn, float *mx)
{
    bool any = false;
    float lo = 0.0f, hi = 0.0f;
    for (int k = 0; k < n; k++) {
        if (!isfinite(v[k])) {
            continue;
        }
        if (!any) {
            lo = hi = v[k];
            any = true;
        } else {
            if (v[k] < lo) lo = v[k];
            if (v[k] > hi) hi = v[k];
        }
    }
    if (!any) {
        return false;
    }
    *mn = lo;
    *mx = hi;
    return true;
}

/**
 * @brief 给数据范围上下各加 10% 余量作为建议量程
 *
 * 数据几乎恒定时（span≈0）不能用比例余量，改按绝对值或给 1.0 的
 * 固定宽度，否则曲线会缩成一条看不见的线。
 *
 * @param mn/mx   数据实际最小/最大值
 * @param out_min/out_max 出参：加余量后的量程上下限
 */
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

/**
 * @brief 取某模式下双轴的建议量程（按窗口内数据最小/最大值加余量）
 *
 * @param win    时间窗
 * @param mode   显示模式
 * @param a_min/a_max 主轴建议上下限
 * @param b_min/b_max 副轴建议上下限（has_b=false 时不写）
 * @return 是否存在副轴
 */
bool pm_wave_get_range(pm_wave_window_t win, pm_wave_mode_t mode, float *a_min, float *a_max,
                       float *b_min, float *b_max)
{
    /* 300 个时间槽覆盖整个窗口；span_of 会跳过无数据的 NaN 槽，
     * 因此量程取自「有数据的时段」而非整个窗口。
     * static 暂存避免在栈上开 600 个 float；本函数只被 UI 任务串行调用，
     * 复用静态缓冲安全（如需多任务并发调用要改为局部数组）。 */
    enum { RANGE_SLOTS = 300 };
    static float ba[RANGE_SLOTS], bb[RANGE_SLOTS];
    (void)pm_wave_copy(win, mode, ba, bb, RANGE_SLOTS);
    int fa, fb;
    bool has_b;
    mode_fields(mode, &fa, &fb, &has_b);

    float mn, mx;
    if (span_of(ba, RANGE_SLOTS, &mn, &mx)) {
        pad_range(mn, mx, a_min, a_max);
    } else {
        /* 无任何数据：给 0~1 的占位量程，避免图表控件除零 */
        *a_min = 0.0f;
        *a_max = 1.0f;
    }
    if (has_b && span_of(bb, RANGE_SLOTS, &mn, &mx)) {
        pad_range(mn, mx, b_min, b_max);
    } else if (has_b) {
        *b_min = 0.0f;
        *b_max = 1.0f;
    }
    return has_b;
}

/**
 * @brief 时间窗的显示名
 * @param win 时间窗
 * @return 如 "1s"/"10min"；非法值返回 "?"
 */
const char *pm_wave_window_name(pm_wave_window_t win)
{
    static const char *names[PM_WAVE_WINDOW_MAX] = {
        "1s", "10s", "30s", "60s", "120s", "10min",
    };
    return (win < PM_WAVE_WINDOW_MAX) ? names[win] : "?";
}

/**
 * @brief 显示模式的显示名
 * @param mode 显示模式
 * @return 如 "电流+功率"；非法值返回 "?"
 */
const char *pm_wave_mode_name(pm_wave_mode_t mode)
{
    static const char *names[PM_WAVE_MODE_MAX] = {
        "功率", "电流+功率", "电压+功率", "电压+电流", "电能",
    };
    return (mode < PM_WAVE_MODE_MAX) ? names[mode] : "?";
}
