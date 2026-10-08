/**
 * @file data_store.h
 * @brief 测量数据快照仓库（跨核数据交换）
 *
 * 测量核（Core 1）写入最新快照与波形环形缓冲，
 * 交互核（Core 0）的 UI 与网页控制台只读快照，避免跨核共享可变状态。
 *
 * 接口骨架，尚未实现。
 */
#pragma once

#include "esp_err.h"

/** 实时测量快照 */
typedef struct {
    float voltage_v;     /**< 电压 (V) */
    float current_a;     /**< 电流 (A) */
    float power_w;       /**< 功率 (W) */
    float energy_wh;     /**< 累计电能 (Wh) */
    float temperature_c; /**< 内部温度 (℃) */
    float humidity_pct;  /**< 内部湿度 (%) */
} data_store_snapshot_t;

/**
 * @brief 初始化数据仓库（分配快照缓冲与同步原语）
 * @return ESP_OK 成功
 */
esp_err_t data_store_init(void);

/**
 * @brief 写入最新快照（仅测量核调用）
 * @param snapshot 快照内容，不可为 NULL
 * @return ESP_OK 成功
 */
esp_err_t data_store_update(const data_store_snapshot_t *snapshot);

/**
 * @brief 读取最新快照（任意核可调用，内部保证读写一致性）
 * @param out 输出：快照内容，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚无有效数据
 */
esp_err_t data_store_get(data_store_snapshot_t *out);
