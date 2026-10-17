/**
 * @file ota_update.h
 * @brief A/B 双分区 OTA 升级中间件：下载镜像 → 校验镜像头 → 自检 → 切换启动分区
 *
 * ─────────────────────────────────────────────────────────────
 * 一、A/B 分区升级的取舍（为什么做成"双分区 + 可回滚"）
 * ─────────────────────────────────────────────────────────────
 * 本工程分区表为 ota_0 / ota_1 两块 4MB 应用分区（见 partitions.csv）。
 * 升级时把新镜像整包下载到「当前未在运行」的备用分区，全部校验与自检
 * 通过后才把启动指针（otadata）切过去，重启后才生效。收益与代价：
 *
 *   收益：升级过程中的任何失败（断电、断网、镜像损坏、自检不过）
 *         都不会影响正在运行的旧系统——最坏情况只是"没升成"而不是"变砖"。
 *         设备装到用户手里之后还能修，靠的就是这一点。
 *   代价：Flash 占用翻倍（本工程 16MB Flash 里占 2×4MB）；
 *         下载/擦写 Flash 期间旧固件仍在运行，需保证与业务逻辑互不干扰
 *         （本工程已用 IRAM 化 GPIO/定时器中断兜住 cache 关闭窗口）。
 *
 * ─────────────────────────────────────────────────────────────
 * 二、回滚（rollback）语义与启动自检
 * ─────────────────────────────────────────────────────────────
 * 切换分区重启后，新固件首次启动时处于 PENDING_VERIFY（待验证）状态：
 *   · 自检通过 → esp_ota_mark_app_valid_cancel_rollback() 确认有效，正常运行；
 *   · 自检失败 → esp_ota_mark_app_invalid_rollback_and_reboot() 标记无效
 *     并重启，bootloader 自动回退到另一分区的旧固件（A/B 回滚）。
 * 启动早期调用 ota_update_rollback_if_pending() 即可完成上述流程。
 *
 * ⚠ 前提：PENDING_VERIFY 状态由 bootloader 写入，依赖
 *   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y。该配置关闭时（当前工程
 *   sdkconfig 默认如此）新固件首启状态为 NEW/VALID，回滚函数退化为空操作，
 *   "A/B 回滚"只剩「升级失败不动 boot 分区」这一半保障。
 *
 * ─────────────────────────────────────────────────────────────
 * 三、安全边界（务必读完再用）
 * ─────────────────────────────────────────────────────────────
 *   · 本版本**未启用镜像签名校验**：CONFIG_SECURE_BOOT（Secure Boot）
 *     与签名 OTA（CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT）均未开启，
 *     因此"镜像是否出自官方"无法在密码学上证明。当前能保证的是：
 *       ① 传输层：HTTPS 用证书包（CONFIG_MBEDTLS_CERTIFICATE_BUNDLE）
 *          验证服务端证书，防窃听与一般中间人；
 *       ② 一致性：esp_app_desc 的 magic 与 project_name 必须与当前固件一致，
 *          防止误刷其他项目/其他芯片的镜像；
 *       ③ 完整性：esp_ota_end() 校验镜像校验和，下载残缺或写坏的镜像
 *          不会被切换启动分区。
 *     要补齐"防篡改 / 防降级"，需在量产安全基线里开启 Secure Boot V2
 *     + 签名 OTA（配合 anti-rollback 的 secure_version），属于 sdkconfig
 *     与 eFuse 层面的改动，不在本组件范围内。
 *   · http:// 明文地址仅供内网/实验室使用：无加密、无服务端认证，
 *     任何中间人都可替换镜像，风险由调用方自担（见 ota_update_start()）。
 *   · 分层约束：本组件属 middleware，不依赖 services/ 与 event_bus；
 *     "升级完成后的自检"由上层通过 ota_update_set_post_check() 注入回调。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OTA 升级状态机状态
 *
 * 流转：IDLE → DOWNLOADING → VERIFYING → DONE
 *                     └──── 任一步失败 ────→ FAILED
 */
typedef enum {
    OTA_UPDATE_STATE_IDLE = 0,   /**< 空闲：未启动过升级（或上次任务创建失败） */
    OTA_UPDATE_STATE_DOWNLOADING, /**< 进行中：建连 + 下载 + 写入备用分区 */
    OTA_UPDATE_STATE_VERIFYING,   /**< 进行中：校验镜像 + 自检 + 切换启动分区 */
    OTA_UPDATE_STATE_DONE,        /**< 成功：boot 分区已切换，等待调用方 esp_restart() */
    OTA_UPDATE_STATE_FAILED,      /**< 失败：最近一次升级失败，boot 分区未改动 */
} ota_update_state_t;

/**
 * @brief 升级后自检回调
 *
 * 由上层注入（如检测外设/采样链路是否正常），本组件不关心具体检查内容。
 *
 * @return ESP_OK 自检通过；其他 esp_err_t 表示自检失败
 *         （错误码会记入 ota_update_get_last_error() 并打印日志）
 */
typedef esp_err_t (*ota_update_post_check_cb_t)(void);

/**
 * @brief 注册"升级后自检"回调
 *
 * 调用时机有两处，均以本回调为准入门槛：
 *   ① 升级流程内：镜像下载并写入完成之后、切换启动分区之前调用；
 *      回调不通过则拒绝切换启动分区（直接失败收尾，boot 分区不动）。
 *   ② 启动流程内：ota_update_rollback_if_pending() 在 PENDING_VERIFY
 *      首启时调用，不通过则触发 A/B 回滚。
 *
 * @param cb 回调函数指针；传 NULL 表示恢复默认（一律通过）
 *
 * @note 请在调用 ota_update_start() 之前注册，升级过程中不要更换回调。
 *       回调运行在 OTA 任务上下文（升级时）或调用者上下文（启动自检时），
 *       可做外设自检等操作，但不要长时间阻塞。
 */
void ota_update_set_post_check(ota_update_post_check_cb_t cb);

/**
 * @brief 启动一次 OTA 升级（异步，立即返回）
 *
 * 内部拉起独立任务执行阻塞式下载，调用方通过 ota_update_get_state()
 * / ota_update_get_progress() 查询进度。任务内流程：
 *   ① HTTPS 建连下载（http:// 明文亦允许，但见下方风险说明）；
 *   ② 校验镜像头（esp_app_desc 的 magic 与 project_name 必须与当前固件一致）；
 *      此步在任何 Flash 写入之前完成，校验不过直接失败；
 *   ③ 写入备用分区（当前未运行的那个 ota_x）；
 *   ④ 运行升级后自检回调；
 *   ⑤ esp_ota_end() 校验镜像完整性 + esp_ota_set_boot_partition() 切换启动分区。
 * 任一步失败均返回/记录 ESP_ERR_*，且**不改动 boot 分区**，设备继续跑旧固件。
 *
 * @param url 下载地址，必须以 "https://" 或 "http://" 开头，
 *            函数内部会拷贝该字符串，调用方无需保持其存活
 *
 * @return ESP_OK                任务已拉起（升级是否成功看状态机）
 *         ESP_ERR_INVALID_ARG   url 为 NULL/空/非 http(s) 前缀
 *         ESP_ERR_INVALID_STATE 已有升级在进行（重复 start），或上次升级已
 *                               DONE 尚未重启（防重复切换 boot 分区）
 *         ESP_ERR_NO_MEM        内存不足（URL 拷贝或任务创建失败）
 *
 * @note FAILED 状态后允许重新 start 重试；DONE 状态需先 esp_restart()。
 * @note ⚠ http:// 明文下载无加密无服务端认证，镜像可被中间人任意替换，
 *       仅限内网/实验室使用；且需 sdkconfig 开启 CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP，
 *       否则 esp_https_ota 会拒绝非验证连接。正式渠道请用 https://。
 * @note 升级成功后由**调用方**决定何时调用 esp_restart()（建议先保存现场、
 *       提示用户），本组件不主动重启。
 */
esp_err_t ota_update_start(const char *url);

/**
 * @brief 查询升级状态机当前状态
 *
 * @return 当前状态（任意任务可调用，返回无锁快照）
 */
ota_update_state_t ota_update_get_state(void);

/**
 * @brief 查询下载进度（可选功能）
 *
 * @return 0~100 进度百分比；-1 表示暂无法计算
 *         （服务器未返回 Content-Length、chunked 传输，或尚未开始下载）
 */
int ota_update_get_progress(void);

/**
 * @brief 查询最近一次升级失败的错误码
 *
 * @return ESP_OK 表示未失败（或已成功）；失败时为具体 ESP_ERR_*
 */
esp_err_t ota_update_get_last_error(void);

/**
 * @brief 启动时调用：处理"升级后首次启动"的自检与 A/B 回滚
 *
 * 建议在系统初始化早期（外设可用于自检之后、业务逻辑启动之前）调用：
 *   · 当前固件状态 == PENDING_VERIFY（升级后首启）：
 *       - 自检（升级后自检回调，默认通过）通过
 *         → esp_ota_mark_app_valid_cancel_rollback()，确认固件有效；
 *       - 自检失败 → esp_ota_mark_app_invalid_rollback_and_reboot()
 *         标记无效并重启，bootloader 回退到另一分区旧固件（成功时函数不返回）。
 *   · 其他状态（NEW/VALID/...，或 otadata 无记录）：无需处理，直接返回。
 *
 * @return ESP_OK                        无需回滚 / 已确认固件有效
 *         ESP_ERR_NOT_FOUND             找不到运行分区（分区表异常）
 *         其他 ESP_ERR_*                otadata 读取失败等
 *         ESP_ERR_OTA_ROLLBACK_FAILED   自检失败且回滚不可用（另一分区无有效镜像），
 *                                       设备继续运行当前固件，由上层决定后续动作
 *
 * @note 完整回滚语义依赖 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y（见文件头）；
 *       该配置关闭时本函数基本为空操作（状态不会是 PENDING_VERIFY）。
 */
esp_err_t ota_update_rollback_if_pending(void);

/**
 * @brief 获取当前运行固件的版本字符串
 *
 * @return 指向 esp_app_desc.version 的只读字符串（如 "1.0.0"），
 *         生命周期与固件一致，无需释放
 */
const char *ota_update_get_current_version(void);

#ifdef __cplusplus
}
#endif
