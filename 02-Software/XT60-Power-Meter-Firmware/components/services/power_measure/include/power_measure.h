/**
 * @file power_measure.h
 * @brief 功率测量服务：采样调度、标定与功率 / 电能计算
 *
 * 运行在 Core 1（APP_CPU），独占测量核，避免被通信与 UI 任务抢占。
 *   task_power_sample —— 1 ms 周期读取 ADS131M02（由 ADC_DRDY 触发）
 *   task_power_calc   —— 10 ms 周期做标定、RMS、功率与电能累加
 * 结果写入 middleware/data_store 快照，供 UI 与网页控制台读取。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/** 标定参数（两点标定：增益与偏置） */
typedef struct {
    float voltage_gain;   /**< 电压增益系数 */
    float voltage_offset; /**< 电压零偏（V） */
    float current_gain;   /**< 电流增益系数 */
    float current_offset; /**< 电流零偏（A） */
} power_measure_calib_t;

/**
 * @brief 初始化测量服务（加载标定参数、准备数据缓冲）
 * @return ESP_OK 成功
 */
esp_err_t power_measure_init(void);

/**
 * @brief 启动采样与计算任务（绑定 Core 1）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t power_measure_start(void);

/**
 * @brief 更新标定参数（同时写入 services/settings 持久化）
 * @param calib 新标定参数，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t power_measure_set_calibration(const power_measure_calib_t *calib);
