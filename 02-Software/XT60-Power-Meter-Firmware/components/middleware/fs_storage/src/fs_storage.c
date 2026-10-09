/**
 * @file fs_storage.c
 * @brief SPIFFS 资源分区挂载实现
 */
#include "fs_storage.h"

#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "fs_storage";

/** 同时打开的文件数（字体文件每字号一个 fd，7 档×2 字重，留余量） */
#define FS_STORAGE_MAX_FILES 16

static bool s_mounted = false;

esp_err_t fs_storage_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "挂载 SPIFFS：label=%s -> %s", FS_STORAGE_PARTITION_LABEL,
             FS_STORAGE_MOUNT_POINT);

    esp_vfs_spiffs_conf_t conf = {
        .base_path = FS_STORAGE_MOUNT_POINT,
        .partition_label = FS_STORAGE_PARTITION_LABEL,
        .max_files = FS_STORAGE_MAX_FILES,
        /* 首次烧录分区可能是空白（无有效 SPIFFS 结构），让驱动自动格式化，
         * 避免首启失败；镜像损坏的情况同理。 */
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败（已尝试自动格式化）: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(FS_STORAGE_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "挂载成功：总计 %u KB，已用 %u KB（%.1f%%）",
                 (unsigned)(total / 1024), (unsigned)(used / 1024),
                 total ? (100.0f * (float)used / (float)total) : 0.0f);
    }

    s_mounted = true;
    return ESP_OK;
}
