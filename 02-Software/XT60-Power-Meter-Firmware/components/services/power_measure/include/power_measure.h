/**
 * @file power_measure.h
 * @brief 功率测量服务：采样调度、标定、均值/功率/电能计算与波形缓冲
 *
 * 运行在 Core 1（APP_CPU，测量核），独占测量任务：
 *   task_sample（Core1）—— 由 ADS131M02 DRDY 节奏驱动：
 *     ① 每样本做原始码值→物理量换算（电压 V / 电流 A / 瞬时功率 W），
 *        写入三级 PSRAM 波形环形缓冲；
 *     ② 每 **200ms 墙钟块**（与实际帧率解耦，不按样本数计）结算一次，
 *        计算 V/I 块内平均值（直流量取平均而非 RMS）、平均功率、
 *        矩形积分电能，写入 data_store 快照。
 *   task_aux（Core0）—— 2s 周期读 AHT20（≥80ms 阻塞，放交互核避免拖慢采样）
 *     与电池电压，结果由采样任务合并进快照（维持 data_store 单写者约束）。
 *
 * 掉电保存：电能与运行时间每 30s（或有清零等改动时）由 task_aux 写入
 * NVS（命名空间 "pm"），启动时恢复。
 *
 * 波形缓冲（PSRAM，三级降采样）：
 *   tier0  4000Hz × 4000 点  —— 1s 窗（原始逐点）
 *   tier1    50Hz × 24000 点 —— 60s 窗（每 80 点均值，可回看 480s）
 *   tier2  1.25Hz × 6000 点  —— 10min 窗（每 40 个 tier1 点均值，可回看 80min）
 * 每个点含 V/I/P/E 四个 float；UI 用 pm_wave_copy() 按**固定时间槽
 * 桶聚合**取数（每槽固定时长、槽内多样本取均值，与采样率无关）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 标定参数：物理量 = 原始码值 × gain + offset（默认值由前端数学量推导，未实测标定） */
typedef struct {
    float voltage_gain;   /**< 电压通道增益（V/LSB） */
    float voltage_offset; /**< 电压零偏（V） */
    float current_gain;   /**< 电流通道增益（A/LSB） */
    float current_offset; /**< 电流零偏（A） */
} power_measure_calib_t;

/** 波形时间窗 */
typedef enum {
    PM_WAVE_1S = 0,
    PM_WAVE_10S,
    PM_WAVE_30S,
    PM_WAVE_60S,
    PM_WAVE_120S,
    PM_WAVE_10MIN,
    PM_WAVE_WINDOW_MAX,
} pm_wave_window_t;

/** 波形显示模式（决定主/副纵轴的两条线） */
typedef enum {
    PM_WAVE_MODE_P = 0, /**< 仅功率 */
    PM_WAVE_MODE_IP,    /**< 电流 + 功率 */
    PM_WAVE_MODE_VP,    /**< 电压 + 功率 */
    PM_WAVE_MODE_VI,    /**< 电压 + 电流 */
    PM_WAVE_MODE_E,     /**< 电能 */
    PM_WAVE_MODE_MAX,
} pm_wave_mode_t;

/**
 * @brief 初始化测量服务：载入 NVS 中的标定/电能/运行时间，准备波形缓冲
 *
 * 不创建任务、不访问 ADC（采样在 power_measure_start 后开始）。
 * @return ESP_OK 成功
 */
esp_err_t power_measure_init(void);

/**
 * @brief 启动采样任务（绑定 Core 1）与辅助采集任务（Core 0）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化/已启动；ESP_ERR_NO_MEM
 */
esp_err_t power_measure_start(void);

/**
 * @brief 更新标定参数（立即生效，并**同步**持久化到 NVS）
 *
 * ⚠ 本函数会在**调用者上下文**里执行 NVS 写入（可达数百 ms）。
 * 若调用方是持有 LVGL 锁的 UI 事件回调，请改用
 * power_measure_set_calibration_deferred()，否则整屏渲染会被冻结。
 *
 * @param calib 新标定参数，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t power_measure_set_calibration(const power_measure_calib_t *calib);

/**
 * @brief 更新标定参数（立即生效，落盘**异步**执行，v0.6.0 新增）
 *
 * 与 power_measure_set_calibration() 的唯一区别是"谁来写 Flash"：
 *   · 本函数只持锁改内存中的标定值，然后置"待落盘"原子标志并立即返回；
 *   · 真正的 NVS 写入由 task_aux（Core0 辅助任务）在下一个周期完成——
 *     它既有"每 30s 周期落盘"，也有"s_persist_dirty 置位即落盘"的通道
 *     （落盘失败会重新置位重试，掉电最多丢数秒标定）。
 *
 * 用途：校准页的"应用校准"按钮回调运行在 LVGL 事件回调里（UI 主循环整轮
 * 持有 LVGL 锁），NVS 写入若同步做会让界面冻结数百 ms，甚至与 task_aux 的
 * 周期落盘争 s_save_mutex 而叠加成秒级卡顿（2026-10 实机观察到的卡死现象）。
 *
 * @param calib 新标定参数，不可为 NULL
 * @return ESP_OK 已受理（内存已生效，落盘稍后完成）
 *         ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t power_measure_set_calibration_deferred(const power_measure_calib_t *calib);

/**
 * @brief 读取当前标定参数
 * @param out 输出，不可为 NULL
 */
void power_measure_get_calibration(power_measure_calib_t *out);

/**
 * @brief 清零电能与运行时间统计（"本次统计"从此重新开始）
 *
 * 由 UI 核调用；真正的清零动作交给测量任务在下一个 200ms 块边界执行，
 * 以保持"电能值只有一个写者（测量核）"的约定，避免跨核读写撕裂。
 * 清零只改内存并置"待落盘"标志，由 task_aux 异步写入 NVS
 * （掉电不回到旧值；采样任务内绝不做耗时的 flash 写入）。
 *
 * @return ESP_OK 已受理（下一个块边界生效）
 *         ESP_ERR_INVALID_STATE 测量服务尚未初始化
 */
esp_err_t power_measure_reset_energy(void);

/**
 * @brief 发起一次电流零点自动校准（v0.6.6 需求 5：采样 3 秒平均）
 *
 * 非阻塞：只置请求标志，测量核在块边界开始累计电流块均值，满 3 秒后把
 * current_offset 按"抵消均值"调整并 deferred 落盘（不阻塞 4kHz 采样）。
 *
 * 为什么由测量核做而不是 UI 读快照：200ms 块均值是电流的权威口径，3 秒=15 块
 * 样本比 UI 侧读 40Hz 显示帧的 1 秒均值更稳；且 UI 与网页控制台共用同一实现。
 * 调用前应保持**空载**（无负载电流），否则会把负载电流当成零点抹掉。
 *
 * @return ESP_OK 已受理
 *         ESP_ERR_INVALID_STATE 测量服务未初始化，或已有一次采样在进行中
 */
esp_err_t power_measure_zero_current_start(void);

/**
 * @brief 查询电流零点校准是否仍在采样（约 3 秒）
 * @return true=采样中；false=空闲或已完成
 */
bool power_measure_zero_current_is_running(void);

/**
 * @brief 读取最近一次电流零点采样的均值（A）；未做过校准时为 0
 */
float power_measure_zero_current_last_avg(void);

/**
 * @brief 查询采样率（Hz，当前固定 4000，与 ADS131M02 配置一致）
 */
uint32_t power_measure_sample_rate_hz(void);

/**
 * @brief 按时间窗与模式填充固定时间槽波形数据（供 lv_chart 刷新）
 *
 * 输出的两条序列 a/b 按时间从旧到新排列，恰好填满 max_points 个
 * **固定时长的时间槽**（桶聚合：每槽对窗内多样本取均值，空槽插值/留 NaN），
 * 右端是"现在"。UI 可「一比一」映射到屏幕列，无需再做抽稀/右对齐。
 * 副轴无数据时（单序列模式）b 槽全为 NaN。
 *
 * @param win        时间窗
 * @param mode       显示模式
 * @param a          主轴序列缓冲，不可为 NULL
 * @param b          副轴序列缓冲（单序列模式可为 NULL）
 * @param max_points 槽（列）数，建议 240~480，与屏宽对齐
 * @return 有效（非 NaN）的槽位数；参数非法返回 0
 */
int pm_wave_copy(pm_wave_window_t win, pm_wave_mode_t mode, float *a, float *b, int max_points);

/**
 * @brief 取某模式下双轴的建议量程（按当前窗口数据最小值/最大值加余量取整）
 *
 * @param win    时间窗
 * @param mode   显示模式
 * @param a_min/a_max 主轴建议上下限
 * @param b_min/b_max 副轴建议上下限（has_b=false 时不写）
 * @return 是否存在副轴
 */
bool pm_wave_get_range(pm_wave_window_t win, pm_wave_mode_t mode, float *a_min, float *a_max,
                       float *b_min, float *b_max);

/** @brief 时间窗的显示名（如 "1s"/"10min"） */
const char *pm_wave_window_name(pm_wave_window_t win);

/** @brief 显示模式的显示名（如 "电流+功率"） */
const char *pm_wave_mode_name(pm_wave_mode_t mode);
