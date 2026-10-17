/**
 * @file web_console.h
 * @brief 网页控制台（HTTP 静态页 + WebSocket 实时数据 / 参数下发）
 *
 * ─────────────────────────────────────────────────────────────
 * 分层定位与设计取舍
 * ─────────────────────────────────────────────────────────────
 * web_console 属于 middleware 层，只允许依赖同层的 data_store 与
 * ESP-IDF 组件（esp_http_server），**禁止** include services/ 的任何头文件。
 *
 * 参数下发（网页 → settings 落 NVS）因此采用「回调注入」而不是直接调用：
 *   上层（main）在 web_console_start() 之前调用 web_console_set_config_handler()
 *   把「写设置」的具体逻辑注册进来；本组件只负责收 JSON、拆键值对、逐对回调，
 *   再把回调结果翻译成 JSON 应答。
 *   好处：middleware 不认识业务——将来换设置存储（NVS/文件/云端），
 *   或者网页要下发新的参数键，都不需要改动本组件一个字。
 *
 * 数据来源只有 data_store_get() 的只读快照（电压/电流/功率/电能/
 * 温湿度/电池电压/运行时间），由测量核持续写入，本组件以约 5Hz
 * 通过 WebSocket 推给浏览器，并提供 GET /api/state 供无 WS 的客户端轮询。
 *
 * 接口一览：
 *   web_console_set_config_handler()  注册/注销参数写入回调
 *   web_console_start()               启动 HTTP 服务器（并拉起 WS 推送任务）
 *   web_console_stop()                停止并完整清理
 *
 * HTTP / WebSocket / JSON 协议细节见 src/web_console.c 文件头的协议表。
 *
 * 编译前提：需要 sdkconfig 打开 CONFIG_HTTPD_WS_SUPPORT（WebSocket 支持）。
 * 未打开时本组件仍可编译运行，但 /ws 只回 501，页面自动退化为轮询模式。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 参数写入回调类型（由上层注入，如 settings 落 NVS）
 *
 * 每收到一个键值对调用一次。回调在 httpd 任务上下文中执行，
 * 因此不能长时间阻塞（快速写 NVS 可以，擦写大块 Flash 请自行排队）。
 *
 * @param key   参数键（UTF-8，如 "voltage_alarm_v"），仅回调期间有效
 * @param value 参数值的字符串形式（仅回调期间有效）：
 *              - JSON 字符串 → 解转义后的原文（如 "XT60-1"）
 *              - JSON 数字   → 字面文本（如 "-12.50"），上层用 strtof/atoi 解析
 *              - JSON true/false/null → "true" / "false" / "null"
 * @return ESP_OK 写入成功；其他错误码会连同出错的键回给 HTTP 客户端
 */
typedef esp_err_t (*web_console_config_handler_t)(const char *key, const char *value);

/**
 * @brief 注册参数写入回调
 *
 * 可在 start 之前或之后任意时刻调用；传 NULL 表示注销
 * （此后 POST /api/config 会回 503 Service Unavailable）。
 *
 * @param handler 回调函数指针，NULL 表示注销
 * @return ESP_OK 成功
 */
esp_err_t web_console_set_config_handler(web_console_config_handler_t handler);

/**
 * @brief 启动网页控制台（HTTP 服务器与 WebSocket 推送任务）
 *
 * 服务端口 80；重复调用（已在运行）返回 ESP_ERR_INVALID_STATE，
 * 即「只允许一个实例」，不做静默忽略，方便上层发现启动顺序问题。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 已在运行
 *         ESP_ERR_NO_MEM / 其他  httpd_start 或任务创建失败
 */
esp_err_t web_console_start(void);

/**
 * @brief 停止网页控制台（先停推送任务，再停 HTTP 服务器，完整清理）
 *
 * 未在运行时返回 ESP_ERR_INVALID_STATE（与 start 对称）。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚未启动
 */
esp_err_t web_console_stop(void);
