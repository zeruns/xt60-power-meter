/**
 * @file mock_data_store.c
 * @brief 数据仓库 mock：从模拟快照读取（写入方为场景脚本）
 */
#include "data_store.h"

#include "sim.h"

esp_err_t data_store_init(void)
{
    return ESP_OK;
}

esp_err_t data_store_get(data_store_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = *sim_get_measurement();
    return ESP_OK;
}

/** 模拟器里数据由场景脚本经 sim_set_measurement() 直接写快照；
 *  本接口在真机由测量任务调用，这里忽略入参，只为让 UI 代码可链接。 */
esp_err_t data_store_update(const data_store_snapshot_t *snapshot)
{
    (void)snapshot;
    return ESP_OK;
}