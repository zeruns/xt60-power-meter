/**
 * @file ota_update.c
 * @brief A/B 双分区 OTA 升级实现（esp_https_ota 流式下载 + 镜像头校验 + 自检门禁）
 *
 * 流程总览（安全边界与回滚取舍见 ota_update.h 文件头）：
 *   ota_update_start() 只做参数校验并拉起独立任务，随即返回；
 *   真正的升级在任务里分四步推进：
 *     ① esp_https_ota_begin()         建连（HTTPS 走证书包验证服务端证书）
 *     ② esp_https_ota_get_img_desc()  读镜像头 → 校验 magic / 项目名
 *        （此时 esp_ota_begin 尚未调用，**没有写过任何 Flash**，
 *          校验不过可直接中止，备用分区保持原样）
 *     ③ esp_https_ota_perform() 循环  下载并流式写入备用分区
 *     ④ 自检回调 + esp_https_ota_finish()
 *        finish 内部 = esp_ota_end()（校验镜像完整性）
 *                    + esp_ota_set_boot_partition()（切换启动分区），
 *        所以自检回调必须放在 finish 之前：回调不过就走 esp_https_ota_abort()，
 *        boot 分区绝不被触碰——这就是"失败路径干净"的落点。
 *
 * 状态机：
 *   IDLE ──start()──> DOWNLOADING ──下载完成──> VERIFYING
 *     VERIFYING ──镜像校验 + 自检 + finish 全过──> DONE（等调用方 esp_restart()）
 *     任一步失败 ─────────────────────────────> FAILED（boot 分区未动）
 *
 * 分层约束：只依赖 IDF 组件（app_update / esp_https_ota / esp_http_client /
 * esp_app_format / esp_partition / mbedtls），不 include services/，不依赖 event_bus。
 */
#include "ota_update.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

/** 日志标签 */
static const char *TAG = "ota_update";

/**
 * OTA 任务栈大小（字节）
 *
 * TLS 握手 + HTTP 收发都在本任务栈上完成，IDF 官方 OTA 示例同为 8192。
 * 注意：本工程 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0，且
 * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y，xTaskCreatePinnedToCore()
 * 的默认分配**不保证**把 8KB 栈放进内部 RAM（与显式 MALLOC_CAP_INTERNAL
 * 的 font_load 任务不同）；本任务只做网络 I/O，不主动读 SPIFFS。
 */
#define OTA_UPDATE_TASK_STACK   8192

/** OTA 任务优先级：5。低于事件总线(6)/UI(12)——下载再忙也不能抢交互与测量 */
#define OTA_UPDATE_TASK_PRIO    5

/** OTA 任务绑定核：Core 0（通信核），与"Core 1 = 测量核"的分工一致 */
#define OTA_UPDATE_TASK_CORE    0

/** HTTP 连接/读超时（毫秒）。默认 5s 对 TLS 握手偏紧，放宽到 15s */
#define OTA_UPDATE_HTTP_TIMEOUT_MS 15000

/** 升级状态机当前状态（写入走临界区，读取方拿无锁快照即可） */
static volatile ota_update_state_t s_state = OTA_UPDATE_STATE_IDLE;

/** 下载进度百分比 0~100；-1 表示服务器未给总长度，无法计算 */
static volatile int s_progress = -1;

/** 最近一次失败的错误码（ESP_OK = 未失败/已成功） */
static volatile esp_err_t s_last_error = ESP_OK;

/** 上层注入的升级后自检回调；NULL = 默认通过 */
static ota_update_post_check_cb_t s_post_check = NULL;

/** 下载地址副本
 *
 * ota_update_start() 时拷贝（调用方不保证字符串长期存活），
 * OTA 任务收尾时释放。
 */
static char *s_url = NULL;

/** 状态写入用自旋锁：双核 ESP32-S3 上保护 start() 的"检查并置忙"不被并发穿透 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 状态迁移（临界区保护，避免两个 start 同时通过忙闲检查）
 *
 * @param state 目标状态
 */
static void ota_update_set_state(ota_update_state_t state)
{
    portENTER_CRITICAL(&s_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_lock);
}

/**
 * @brief OTA 任务主体：建连 → 校验镜像头 → 下载写入 → 自检 → 切换启动分区
 *
 * 全程通过 err/goto exit 收尾：任何一步失败都走 esp_https_ota_abort()
 * 放弃写入（boot 分区不动），状态置 FAILED 并记录错误码。
 *
 * @param arg xTaskCreate 传入的任务参数（本模块未使用）
 */
static void ota_update_task(void *arg)
{
    (void)arg; /* 未使用参数，显式丢弃以避免编译告警 */

    esp_err_t err = ESP_OK;
    esp_https_ota_handle_t handle = NULL;
    esp_app_desc_t new_desc;
    const esp_app_desc_t *cur_desc;
    esp_http_client_config_t http_cfg;
    esp_https_ota_config_t ota_cfg;

    /* ── ① 建连 ────────────────────────────────────────────────────
     * esp_http_client 按 URL 前缀自动选择 HTTP/HTTPS 传输层。
     * timeout_ms 覆盖连接与单次读超时（默认 5s 对 TLS 握手偏紧）。 */
    memset(&http_cfg, 0, sizeof(http_cfg));
    http_cfg.url = s_url;
    http_cfg.timeout_ms = OTA_UPDATE_HTTP_TIMEOUT_MS;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    /* 用内置证书包验证服务端证书（需 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y，
     * 本工程 sdkconfig 已开启）。http:// 明文不走 TLS，此字段不生效。 */
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    memset(&ota_cfg, 0, sizeof(ota_cfg));
    ota_cfg.http_config = &http_cfg;

    /* esp_https_ota_begin：初始化上下文、建立连接、选定备用分区
     * （内部用 esp_ota_get_next_update_partition() 找到未运行的 ota_x）。
     * 注意：此时还没有 esp_ota_begin，未擦除/写入任何 Flash。 */
    err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "建立 OTA 连接失败: %s", esp_err_to_name(err));
        goto exit;
    }

    /* ── ② 镜像头校验：必须先于任何 Flash 写入 ────────────────────
     * esp_https_ota_get_img_desc() 先把镜像头读进 RAM 缓冲（未写 Flash），
     * 之后 perform() 会把这段缓冲原样写入，因此"先校验后写入"成立。 */
    err = esp_https_ota_get_img_desc(handle, &new_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取镜像描述失败（镜像头损坏或非应用镜像）: %s", esp_err_to_name(err));
        err = ESP_ERR_OTA_VALIDATE_FAILED;
        goto exit;
    }

    cur_desc = esp_app_get_description();
    if (new_desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(new_desc.project_name, cur_desc->project_name, sizeof(new_desc.project_name)) != 0) {
        ESP_LOGE(TAG, "镜像头校验失败: magic=0x%08x, 镜像项目名 \"%s\" != 当前项目名 \"%s\"",
                 (unsigned)new_desc.magic_word, new_desc.project_name, cur_desc->project_name);
        err = ESP_ERR_OTA_VALIDATE_FAILED;
        goto exit;
    }
    ESP_LOGI(TAG, "镜像头校验通过: %s v%s -> v%s",
             cur_desc->project_name, cur_desc->version, new_desc.version);

    /* ── ③ 下载并写入备用分区 ──────────────────────────────────────
     * 每次 perform() 只读写一小段即返回，循环调用直到完成；
     * ESP_ERR_HTTPS_OTA_IN_PROGRESS 表示"还没完，继续调"。 */
    for (;;) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        /* 更新进度（总长未知时置 -1，由 UI 决定如何展示） */
        int total = esp_https_ota_get_image_size(handle);
        int read_len = esp_https_ota_get_image_len_read(handle);
        if (total > 0 && read_len >= 0) {
            int percent = (int)(((int64_t)read_len * 100) / total);
            if (percent > 100) {
                percent = 100;
            }
            s_progress = percent;
        } else {
            s_progress = -1;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "下载/写入失败: %s（已读 %d 字节）",
                 esp_err_to_name(err), esp_https_ota_get_image_len_read(handle));
        goto exit;
    }

    /* 防"半张镜像"：连接正常结束不等于数据收全 */
    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "镜像未接收完整（连接被提前关闭）");
        err = ESP_ERR_OTA_VALIDATE_FAILED;
        goto exit;
    }

    /* ── ④ 自检门禁 + 收尾切换启动分区 ──────────────────────────── */
    ota_update_set_state(OTA_UPDATE_STATE_VERIFYING);

    /* 升级后自检：不通过就拒绝切换启动分区。
     * 默认（未注册回调）视为通过，与"默认不阻塞升级"的产品预期一致。 */
    if (s_post_check != NULL) {
        esp_err_t check = s_post_check();
        if (check != ESP_OK) {
            ESP_LOGE(TAG, "升级后自检未通过（%s），拒绝切换启动分区", esp_err_to_name(check));
            err = check;
            goto exit;
        }
    }

    /* esp_https_ota_finish 内部：
     *   esp_ota_end()             校验镜像完整性（校验和/结构）
     *   esp_ota_set_boot_partition() 切换启动分区（仅当上面全部成功）
     * esp_ota_end 失败时 finish 直接返回错误，不会切换 boot 分区。 */
    err = esp_https_ota_finish(handle);
    handle = NULL; /* finish 已释放上下文，收尾时不得再 abort */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "镜像最终校验/切换启动分区失败: %s", esp_err_to_name(err));
        goto exit;
    }

    ESP_LOGI(TAG, "OTA 成功：启动分区已切换，等待调用方 esp_restart()（新版本 v%s）",
             new_desc.version);

exit:
    /* 失败路径：abort 只回收本次 OTA 的句柄与连接，不写 otadata、不动 boot 分区 */
    if (handle != NULL) {
        esp_https_ota_abort(handle);
    }

    free(s_url);
    s_url = NULL;

    if (err == ESP_OK) {
        s_progress = 100;
        s_last_error = ESP_OK;
        ota_update_set_state(OTA_UPDATE_STATE_DONE);
    } else {
        s_last_error = err;
        ota_update_set_state(OTA_UPDATE_STATE_FAILED);
    }

    vTaskDelete(NULL); /* 任务自删除，不再返回 */
}

void ota_update_set_post_check(ota_update_post_check_cb_t cb)
{
    /* 简单赋值即可：约定在 start() 之前注册，升级过程中不更换 */
    s_post_check = cb;
}

esp_err_t ota_update_start(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "URL 必须以 https:// 或 http:// 开头: %s", url);
        return ESP_ERR_INVALID_ARG;
    }

    /* 先拷贝 URL：任务要用它到下载结束，调用方的字符串可能随栈销毁 */
    char *url_copy = strdup(url);
    if (url_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 临界区内"检查并置忙"：防止两个任务同时调用 start 各自拉起一个 OTA */
    portENTER_CRITICAL(&s_lock);
    if (s_state == OTA_UPDATE_STATE_DOWNLOADING ||
        s_state == OTA_UPDATE_STATE_VERIFYING ||
        s_state == OTA_UPDATE_STATE_DONE) {
        portEXIT_CRITICAL(&s_lock);
        free(url_copy);
        ESP_LOGW(TAG, "OTA 进行中或已完成待重启，忽略重复 start");
        return ESP_ERR_INVALID_STATE;
    }
    s_state = OTA_UPDATE_STATE_DOWNLOADING;
    portEXIT_CRITICAL(&s_lock);

    s_url = url_copy;
    s_progress = -1;
    s_last_error = ESP_OK;

    /* xTaskCreatePinnedToCore(任务函数, 任务名, 栈字节数, 参数, 优先级, 句柄, 核心号)：
     * 阻塞式下载必须离开调用者上下文，否则 UI/测量会被网络 I/O 拖死。
     * 绑到 Core 0（通信核），与测量核隔离；忙闲判定以状态机为准，无需任务句柄。 */
    BaseType_t ok = xTaskCreatePinnedToCore(ota_update_task, "ota_update",
                                            OTA_UPDATE_TASK_STACK, NULL,
                                            OTA_UPDATE_TASK_PRIO, NULL,
                                            OTA_UPDATE_TASK_CORE);
    if (ok != pdPASS) {
        free(s_url);
        s_url = NULL;
        ota_update_set_state(OTA_UPDATE_STATE_IDLE);
        ESP_LOGE(TAG, "创建 OTA 任务失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

ota_update_state_t ota_update_get_state(void)
{
    return s_state;
}

int ota_update_get_progress(void)
{
    return s_progress;
}

esp_err_t ota_update_get_last_error(void)
{
    return s_last_error;
}

esp_err_t ota_update_rollback_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "找不到运行分区（分区表异常）");
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    /* esp_ota_get_state_partition：从 otadata 分区读取指定应用分区的状态。
     * 常见值：NEW（刚烧录）/ PENDING_VERIFY（升级后首启，等待确认）/
     * VALID（已确认）/ INVALID（已标记坏）。ESP_ERR_NOT_FOUND 表示没有记录。 */
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_ERR_NOT_FOUND) {
        /* otadata 中没有当前分区的记录（如整机首次烧录）：无待验证升级，正常启动 */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取 otadata 状态失败: %s", esp_err_to_name(err));
        return err;
    }

    if (ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        /* 不是"升级后首次启动"（NEW/VALID/...），无需自检与回滚 */
        return ESP_OK;
    }

    /* PENDING_VERIFY：升级后的首启，运行上层注入的自检（默认通过） */
    esp_err_t check = ESP_OK;
    if (s_post_check != NULL) {
        check = s_post_check();
    }

    if (check == ESP_OK) {
        /* 自检通过：确认固件有效，取消回滚监视 */
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "升级后首启自检通过，固件已标记为有效");
        }
        return err;
    }

    /* 自检失败：标记当前固件无效并回滚重启。
     * IDF 的 esp_ota_mark_app_invalid_rollback_and_reboot() =
     * esp_ota_mark_app_invalid_rollback() + esp_restart()，
     * 且先写入"无效"标记成功后才重启，不会出现"已重启但标记没写上"的中间态；
     * 成功时本函数不返回，设备回到升级前的固件。 */
    ESP_LOGW(TAG, "升级后首启自检未通过（%s），回滚到升级前固件并重启", esp_err_to_name(check));
    err = esp_ota_mark_app_invalid_rollback_and_reboot();

    /* 走到这里说明回滚不可用：
     *   ESP_ERR_OTA_ROLLBACK_FAILED 另一分区没有可启动的有效镜像
     *   ESP_FAIL                    当前是工厂分区启动等无法回滚的情形
     * 设备继续运行当前固件，把错误抛给上层决定（提示重刷/降级运行）。 */
    ESP_LOGE(TAG, "回滚失败: %s，设备将继续运行当前固件", esp_err_to_name(err));
    return err;
}

const char *ota_update_get_current_version(void)
{
    return esp_app_get_description()->version;
}
