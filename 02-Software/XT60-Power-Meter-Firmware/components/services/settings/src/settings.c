/**
 * @file settings.c
 * @brief 配置持久化服务实现（NVS + 内存缓存 + 防抖落盘）
 *
 * 数据流：
 *   用户改设置 → settings_set_xxx() 更新内存缓存 → 标记"脏" → 启动防抖定时器
 *              → 定时器到点（期间无新改动）→ settings_save_now() 写 NVS
 *
 * 读取时只访问内存缓存，不碰 Flash，因此可以高频调用。
 */
#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

/** 日志标签 */
static const char *TAG = "settings";

/** NVS 命名空间（相当于一张表名，最长 15 字符） */
#define SETTINGS_NVS_NAMESPACE     "xt60cfg"

/** 各配置项的键名（NVS 键名同样限制 15 字符） */
#define SETTINGS_KEY_BUZZER_VOLUME "buzzer_vol"

/** 防抖时长（毫秒）：最后一次修改后静默这么久才真正写 Flash */
#define SETTINGS_SAVE_DELAY_MS     1500

/** 响度合法范围，与 buzzer 驱动保持一致 */
#define SETTINGS_VOLUME_MIN        1
#define SETTINGS_VOLUME_MAX        100

/** 内存缓存：保存当前生效的配置 */
static settings_t s_cache;

/** 是否已初始化 */
static bool s_ready = false;

/** 是否有未落盘的改动（"脏"标记） */
static bool s_dirty = false;

/** 防抖定时器句柄 */
static esp_timer_handle_t s_save_timer = NULL;

/**
 * 保护缓存与脏标记的自旋锁
 *
 * 缓存会被多个任务读写（UI 任务写、其他任务读），
 * 而脏标记会被任务与防抖定时器回调共同访问，因此需要保护。
 * 临界区里只做几个字节的赋值，开销可忽略。
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 把配置重置为出厂默认值
 *
 * 出厂默认值来自 Kconfig，也就是"编译时确定的初始设定"。
 */
static void load_defaults(settings_t *cfg)
{
    cfg->buzzer_volume = CONFIG_BUZZER_VOLUME_PERCENT;
}

/**
 * @brief 打开 NVS 并处理首次上电 / 版本升级两种特殊情况
 *
 * nvs_flash_init() 在两种情况下会返回错误，且都属于正常现象：
 *   · 分区里还没有有效数据（首次上电、刚换芯片）
 *   · 分区的 NVS 版本与当前 IDF 不一致（升级了 IDF）
 * 这两种情况的标准处理方式是"擦除分区并用新格式重建"。
 */
static esp_err_t nvs_init_with_recovery(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建（%s），执行擦除", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "擦除 NVS 失败: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }

    return err;
}

/**
 * @brief 从 NVS 载入各配置项到缓存（缺失项保持默认值）
 */
static void load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* 首次上电时这个命名空间还不存在，属于正常情况：
         * 缓存里已经是出厂默认值，直接返回即可 */
        ESP_LOGI(TAG, "尚无已保存的配置，使用出厂默认值");
        return;
    }

    /* ---- 蜂鸣器响度 ---- */
    uint8_t volume = 0;
    err = nvs_get_u8(handle, SETTINGS_KEY_BUZZER_VOLUME, &volume);
    if (err == ESP_OK && volume >= SETTINGS_VOLUME_MIN && volume <= SETTINGS_VOLUME_MAX) {
        s_cache.buzzer_volume = volume;
        ESP_LOGI(TAG, "已载入蜂鸣器响度: %u%%", volume);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        /* 值越界说明存储被异常改写，此时保留出厂默认值更安全 */
        ESP_LOGW(TAG, "蜂鸣器响度读取异常（%s，值 %u），使用默认值", esp_err_to_name(err), volume);
    }

    nvs_close(handle);
}

/**
 * @brief 把内存缓存写入 NVS（内部使用，调用前需确保已初始化）
 */
static esp_err_t persist_cache(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, SETTINGS_KEY_BUZZER_VOLUME, s_cache.buzzer_volume);
    if (err == ESP_OK) {
        /* nvs_set_xxx 只是写入 RAM 中的 NVS 页缓存，
         * 必须 commit 才会真正落到 Flash */
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 NVS 失败: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 防抖定时器回调：静默期结束，执行落盘
 */
static void save_timer_callback(void *arg)
{
    (void)arg;

    portENTER_CRITICAL(&s_lock);
    bool need_save = s_dirty;
    portEXIT_CRITICAL(&s_lock);

    if (!need_save) {
        return;
    }

    if (persist_cache() == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_dirty = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "配置已保存（防抖 %d ms 后落盘）", SETTINGS_SAVE_DELAY_MS);
    }
}

/**
 * @brief 标记"有改动待保存"，并重置防抖定时器
 */
static void mark_dirty_and_schedule(void)
{
    portENTER_CRITICAL(&s_lock);
    s_dirty = true;
    portEXIT_CRITICAL(&s_lock);

    /* 先停止再启动：esp_timer 要求启动前定时器处于停止状态。
     * 每来一次新改动就重置，从而实现"静默一段时间后才落盘"。 */
    esp_timer_stop(s_save_timer);
    esp_err_t err = esp_timer_start_once(s_save_timer, (uint64_t)SETTINGS_SAVE_DELAY_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动防抖定时器失败: %s", esp_err_to_name(err));
    }
}

esp_err_t settings_init(void)
{
    if (s_ready) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_OK;
    }

    /* ① 先把缓存填上出厂默认值：这样即使后面 NVS 出问题，
     *    设备也一定能拿到一份合法配置继续运行（优雅降级） */
    load_defaults(&s_cache);

    /* ② 创建防抖定时器（与 NVS 是否可用无关，先建好备用） */
    const esp_timer_create_args_t timer_cfg = {
        .callback = save_timer_callback,
        .arg = NULL,
        .name = "cfg_save",
    };
    esp_err_t err = esp_timer_create(&timer_cfg, &s_save_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建防抖定时器失败: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;

    /* ③ 尝试初始化 NVS 并载入已保存的配置。
     *    失败不回滚 s_ready —— 允许"只能读默认值、无法保存"的降级运行。 */
    err = nvs_init_with_recovery();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s，本次运行将使用出厂默认值（设置无法保存）",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    load_from_nvs();

    ESP_LOGI(TAG, "初始化完成（当前蜂鸣器响度 %u%%）", s_cache.buzzer_volume);
    return ESP_OK;
}

esp_err_t settings_get(settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 结构体很小（目前 1 字节），用临界区拷贝即可保证一致性，
     * 不必引入互斥锁 */
    portENTER_CRITICAL(&s_lock);
    *out = s_cache;
    portEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}

esp_err_t settings_set_buzzer_volume(uint8_t percent)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < SETTINGS_VOLUME_MIN || percent > SETTINGS_VOLUME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    s_cache.buzzer_volume = percent;
    portEXIT_CRITICAL(&s_lock);

    /* 只更新缓存 + 安排延迟落盘，不立即写 Flash（防磨损） */
    mark_dirty_and_schedule();

    return ESP_OK;
}

esp_err_t settings_save_now(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 取消待执行的防抖任务，改为当前立即落盘 */
    esp_timer_stop(s_save_timer);

    esp_err_t err = persist_cache();
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_dirty = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "配置已立即保存");
    }
    return err;
}
