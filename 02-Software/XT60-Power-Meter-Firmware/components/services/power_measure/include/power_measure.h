/**
 * @file power_measure.h
 * @brief 功率测量服务：4kHz 采样调度、标定、RMS/功率/电能计算与波形缓冲
 *
 * 运行在 Core 1（APP_CPU，测量核），独占测量任务：
 *   task_sample（Core1）—— 由 ADS131M02 DRDY 节奏驱动，4kSPS：
 *     ① 每样本做原始码值→物理量换算（电压 V / 电流 A / 瞬时功率 W），
 *        写入三级 PSRAM 波形环形缓冲；
 *     ② 每 800 样本（200ms）为一块，计算 V/I 真有效值、平均功率、
 *        矩形积分电能，合并温湿度/电池后写入 data_store 快照。
 *   task_aux（Core0）—— 2s 周期读 AHT20（≥80ms 阻塞，放交互核避免拖慢采样）
 *     与电池电压，结果由采样任务合并进快照（维持 data_store 单写者约束）。
 *
 * 掉电保存：电能与运行时间每 10s 写入 NVS（命名空间 "pm"），启动时恢复。
 *
 * 波形缓冲（PSRAM，三级降采样）：
 *   tier0  4000Hz × 4000 点  —— 1s 窗（原始逐点）
 *   tier1   400Hz × 24000 点 —— 60s 窗（每 10 点均值）
 *   tier2    10Hz × 6000 点  —— 10min 窗（每 40 个 tier1 点均值）
 * 每个点含 V/I/P/E 四个 float；UI 用 pm_wave_copy() 按窗抽稀取 ≤N 点。
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
 * @brief 更新标定参数（立即生效，并持久化到 NVS）
 * @param calib 新标定参数，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t power_measure_set_calibration(const power_measure_calib_t *calib);

/**
 * @brief 读取当前标定参数
 * @param out 输出，不可为 NULL
 */
void power_measure_get_calibration(power_measure_calib_t *out);

/**
 * @brief 查询采样率（Hz，当前固定 4000，与 ADS131M02 配置一致）
 */
uint32_t power_measure_sample_rate_hz(void);

/**
 * @brief 按时间窗与模式抽稀拷贝波形数据（供 lv_chart 刷新）
 *
 * 输出的两条序列 a/b 按时间从旧到新排列；抽稀采用等间隔取点，
 * 点数不超过 max_points。副轴无数据时（单序列模式）返回 has_b=false。
 *
 * @param win        时间窗
 * @param mode       显示模式
 * @param a          主轴序列缓冲，不可为 NULL
 * @param b          副轴序列缓冲，不可为 NULL
 * @param max_points 缓冲容量（建议 240~320，匹配屏宽）
 * @return 实际写入的点数；参数非法返回 0
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
