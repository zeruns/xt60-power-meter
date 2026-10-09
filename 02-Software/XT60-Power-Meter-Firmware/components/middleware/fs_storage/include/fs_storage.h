/**
 * @file fs_storage.h
 * @brief 资源文件系统服务：把 storage 分区挂载为 SPIFFS（/spiffs）
 *
 * storage 分区见 partitions.csv（data/spiffs，约 7.5MB），用途：
 *   · UI 字体（tiny_ttf 从文件加载，不占 app 程序空间）；
 *   · 后续波形记录导出、网页静态资源等。
 *
 * LVGL 通过 POSIX FS 驱动（盘符 'S'，路径前缀 /spiffs）访问本分区，
 * 因此本服务必须在 lvgl_port 创建字体之前完成挂载（启动表已保证顺序）。
 *
 * 挂载失败（首次烧录空分区 / 镜像损坏）时自动格式化一次再挂；
 * 仍失败则返回错误，上层（lvgl_port）回退内置 ASCII 字体，不崩机。
 */
#pragma once

#include "esp_err.h"

/** SPIFFS 挂载点（POSIX 路径前缀） */
#define FS_STORAGE_MOUNT_POINT "/spiffs"

/** 分区标签，与 partitions.csv 中的 Name 一致 */
#define FS_STORAGE_PARTITION_LABEL "storage"

/**
 * @brief 挂载 storage 分区到 /spiffs（失败自动格式化重试一次）
 * @return ESP_OK 成功；其他为 SPIFFS/VFS 错误
 */
esp_err_t fs_storage_init(void);
