/**
 * @file fs_storage.c
 * @brief SPIFFS 资源分区挂载实现
 *
 * 职责：把 partitions.csv 中名为 storage 的数据分区挂载成 SPIFFS 文件系统，
 * 并通过 ESP-IDF 的 VFS（Virtual File System，虚拟文件系统）层挂到 /spiffs
 * 路径下。挂好之后，整个固件都能用标准 C 库的 fopen/fread/fwrite 像读写
 * 普通文件一样访问这个分区（本项目主要放 UI 字体，避免字库挤占 app 固件空间）。
 *
 * 关键设计：
 *   · 幂等：重复调用 fs_storage_init() 直接返回成功，不重复向 VFS 注册；
 *   · **格式化是最后手段**：分区里存的是 UI 字体资源（中文显示依赖），
 *     静默格式化会把已烧录的字库抹掉、只剩空文件系统。故先按
 *     format_if_mount_failed=false 正常挂载，失败后先告警再允许格式化重挂
 *     ——只在"分区确实是空白/已损坏"时才走到格式化，且格式化前明确告知后果；
 *   · 挂载成功后查询并打印容量占用，方便提前发现"字体快放不下"。
 *
 * 时序约束：必须在 lvgl_port 创建字体之前调用（启动表已保证顺序），
 * 否则 LVGL 的 tiny_ttf 字体加载会因打不开文件而回退内置字体（不崩机但难看）。
 */
#include "fs_storage.h"

#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"

/** 日志标签（ESP_LOGx 宏的第一个参数，按模块过滤日志用） */
static const char *TAG = "fs_storage";

/** 同时打开的文件数上限（按 8 个字号 × 2 个字重 = 16 个文件句柄设定） */
#define FS_STORAGE_MAX_FILES 16

/** 是否已成功挂载（幂等标志，防止重复 register 与 VFS 已注册错误） */
static bool s_mounted = false;

/**
 * @brief 挂载 storage 分区到 /spiffs（格式化是最后手段，格式化前必告警）
 *
 * 流程：
 *   ① 先用 format_if_mount_failed=false 正常挂载一次（不动分区内容）——
 *      已烧录字体的量产设备绝大多数走这条路，零风险；
 *   ② 仅当 ① 失败（分区空白/文件系统损坏）才允许格式化重挂一次，
 *      且格式化前用 ESP_LOGW 明确告知"字体分区将被格式化，中文显示需重烧资源"；
 *   ③ 成功后 esp_spiffs_info() 查容量。
 *
 * 为什么不一上来就 format_if_mount_failed=true（历史写法的坑）：
 *   它会**静默**把分区抹掉重建——如果只是偶发挂载错误或版本不匹配，
 *   烧录好的中文字库就没了，且日志上看不出数据已丢，属于高代价的隐性破坏。
 *
 * @return ESP_OK 成功；其他为 SPIFFS/VFS 错误（此时上层应回退内置字体）
 * @note 本函数只负责把分区挂起来，不创建/写入任何文件。
 */
esp_err_t fs_storage_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "挂载 SPIFFS：label=%s -> %s", FS_STORAGE_PARTITION_LABEL,
             FS_STORAGE_MOUNT_POINT);

    /* VFS 挂载配置结构体：告诉 esp_spiffs 组件挂哪个分区、挂到哪个路径、
     * 以及最多允许同时打开几个文件（SPIFFS 打开的文件数有限，必须显式声明） */
    esp_vfs_spiffs_conf_t conf = {
        .base_path = FS_STORAGE_MOUNT_POINT,
        .partition_label = FS_STORAGE_PARTITION_LABEL,
        .max_files = FS_STORAGE_MAX_FILES,
        /* ① 第一次挂载禁止自动格式化：先把分区当"已有数据"对待 */
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    /* ^ 把 SPIFFS 分区注册为 VFS，挂到 base_path 指定的 /spiffs 路径，
     *   之后即可用标准 fopen("/spiffs/xxx", ...) 读写分区里的文件 */
    if (err != ESP_OK) {
        /* ② 正常挂载失败（分区空白/文件系统损坏/label 找不到等）：
         * 格式化重挂是最后手段，但会抹掉字体资源——必须显式告警后再做，
         * 绝不静默格式化。先反注册兜底，避免重复 register 报"已注册"。 */
        ESP_LOGW(TAG, "SPIFFS 首次挂载失败（%s）：字体分区将被格式化，"
                      "中文显示需重烧资源！（仅在分区空白/损坏时才会走到这里）",
                 esp_err_to_name(err));
        esp_vfs_spiffs_unregister(FS_STORAGE_PARTITION_LABEL);

        conf.format_if_mount_failed = true;
        err = esp_vfs_spiffs_register(&conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS 格式化重挂仍失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(FS_STORAGE_PARTITION_LABEL, &total, &used) == ESP_OK) {
        /* ^ 查询 SPIFFS 分区的总容量与已用字节数（分区必须已挂载才有效） */
        ESP_LOGI(TAG, "挂载成功：总计 %u KB，已用 %u KB（%.1f%%）",
                 (unsigned)(total / 1024), (unsigned)(used / 1024),
                 total ? (100.0f * (float)used / (float)total) : 0.0f);
    }

    s_mounted = true;
    return ESP_OK;
}
