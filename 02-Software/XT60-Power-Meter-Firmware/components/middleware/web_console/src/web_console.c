/**
 * @file web_console.c
 * @brief 网页控制台实现：esp_http_server 提供静态页 / REST / WebSocket 推送
 *
 * ─────────────────────────────────────────────────────────────
 * 设计取舍（为什么长这样）
 * ─────────────────────────────────────────────────────────────
 * ① 静态页内嵌为 C 字符串常量（web_console_page.c），不依赖 SPIFFS/分区：
 *    网页是控制台的一部分，跟着固件一起升级，永远不存在「固件新、页面旧」；
 *    代价是页面改版要重新编译——对仪器类设备这是划算的交换。
 *
 * ② JSON 自己拼、自己解，不引第三方解析库：
 *    ESP-IDF v6.1 已不再内置 json/cJSON 组件，而本组件的 JSON 只有两种形状——
 *    「快照对象」（键固定、值全是数字）和「配置对象」（扁平键值对），
 *    手写百来行解析器比引入外部组件更可控，也符合 middleware 最小依赖原则。
 *
 * ③ WS 推送走「独立任务 + httpd_ws_send_data_async()」：
 *    esp_http_server 的 socket 写入必须在 httpd 任务里执行，跨任务只能用
 *    httpd_queue_work 系的异步接口（send_data_async 内部就是排了这样一个
 *    工作项）。它只拷贝帧结构、不拷贝 payload，所以每个客户端各拷贝一份
 *    堆缓冲，发送完成后在 transfer_complete_cb 里释放。
 *    发送失败只记 DEBUG 日志（静默降级——断线的客户端不值得刷屏），
 *    并触发服务器回收该会话，避免反复向死连接发送。
 *
 * ④ 参数下发不认识业务（见 web_console.h 的分层说明）：只做「拆键值对 + 回调」。
 *    注意回调是逐对调用的：若第 3 个键写入失败，前 2 个已经生效（部分写入），
 *    回滚与否由上层语义决定，本组件不猜业务。
 *
 * ─────────────────────────────────────────────────────────────
 * 协议表
 * ─────────────────────────────────────────────────────────────
 * GET  /            → 200 text/html          内嵌单页（深色仪表风、移动端可用）
 * GET  /api/state   → 200 application/json   当前测量快照（见下方字段表）
 *                    503 {"ok":false,"code":"no_data",...}   尚无测量数据
 * POST /api/config  → 请求体为扁平 JSON 对象（键值对），逐对交给注册的回调
 *                    200 {"ok":true}
 *                    400 {"ok":false,"code":"bad_request",...}  JSON 语法/形状错误
 *                    413 {"ok":false,"code":"too_large",...}    请求体超限
 *                    503 {"ok":false,"code":"no_handler",...}   回调未注册
 *                    500 {"ok":false,"code":"apply_failed","key":出错键,
 *                         "esp_err":错误码,...}
 * WS   /ws          → 服务端每 200ms 推一帧 TEXT，内容与 /api/state 的 200 响应相同；
 *                    上行仅支持 {"cmd":"ping"}（回 {"ok":true,"pong":true}）
 *                    与 {"cmd":"state"}（立即回一帧快照）；其他帧静默忽略——
 *                    但一定消费掉，防止残留字节破坏 WS 帧流。
 *
 * 快照 JSON 字段（与 data_store_snapshot_t 一一对应）：
 *   {"ok":true,
 *    "voltage_v":12.34,      电压（V，2 位小数）
 *    "current_a":1.234,      电流（A，3 位小数）
 *    "power_w":15.23,        功率（W，2 位小数）
 *    "energy_wh":4.56,       累计电能（Wh，2 位小数）
 *    "temperature_c":25.3,   机内温度（℃，1 位小数）
 *    "humidity_pct":48.2,    机内湿度（%，1 位小数）
 *    "battery_mv":3980,      电池电压（mV，整数，0 = 尚未采集）
 *    "runtime_s":3600}       运行时间（秒，整数）
 */
#include "web_console.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "data_store.h"

/** 日志标签 */
static const char *TAG = "web_console";

/* ============================ 可调参数 ============================ */

/** HTTP 服务端口 */
#define WEB_CONSOLE_PORT 80
/** WS 快照推送周期（ms），200ms ≈ 5Hz */
#define WEB_CONSOLE_PUSH_PERIOD_MS 200
/** WS 推送任务栈（字节）：只做 snprintf + 异步排发送，4KB 足够 */
#define WEB_CONSOLE_PUSH_TASK_STACK 4096
/** WS 推送任务优先级：略高于 idle，不与测量/显示任务抢 CPU */
#define WEB_CONSOLE_PUSH_TASK_PRIO 4
/** 同时保持的连接数上限（HTTP + WS 共用）；httpd_get_client_list() 要求
 *  其数组长度不小于 max_open_sockets，故两者共用这一个宏 */
#define WEB_CONSOLE_MAX_CLIENTS 8
/** POST /api/config 请求体上限（字节）：参数都是短键值对，1KB 绰绰有余 */
#define WEB_CONSOLE_CFG_BODY_MAX 1024
/** JSON 快照缓冲（字节）：实测约 170 字节，留一倍余量 */
#define WEB_CONSOLE_JSON_BUF_LEN 256
/** 参数键长度上限（字节，含结尾 '\0'） */
#define WEB_CONSOLE_KEY_MAX 48
/** 参数值长度上限（字节，含结尾 '\0'） */
#define WEB_CONSOLE_VALUE_MAX 256

/* ============================ 运行状态 ============================ */

/** HTTP 服务器句柄（NULL = 未运行） */
static httpd_handle_t s_server;
/** 上层注入的参数写入回调（NULL = 未注册） */
static web_console_config_handler_t s_config_handler;
#if CONFIG_HTTPD_WS_SUPPORT
/** WS 快照推送任务句柄（仅 start/stop 访问） */
static volatile TaskHandle_t s_push_task;
/** 推送任务退出请求标志 */
static volatile bool s_push_stop_req;
/** 推送任务已挂起、等待 stop() 删除（避免「任务自删 vs stop 强删」竞态） */
static volatile bool s_push_parked;
#endif

/** 内嵌单页 HTML，定义在 web_console_page.c（大段字符串独立成文件，避免淹没主逻辑） */
extern const char web_console_page_html[];

/* ============================ JSON 构建 ============================ */

/**
 * @brief 把 data_store 快照拼成 JSON
 *
 * 键名固定、值全是数字，直接 snprintf 即可，无需通用序列化器。
 * 浮点统一转 double 传入（变参提升规则），输出用固定小数位，方便前端解析。
 *
 * @param buf     输出缓冲
 * @param buf_len 缓冲长度
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚无测量数据
 *         ESP_ERR_INVALID_SIZE  缓冲不足（理论上不会发生，防御性检查）
 */
static esp_err_t build_state_json(char *buf, size_t buf_len)
{
    data_store_snapshot_t snap;
    esp_err_t err = data_store_get(&snap);
    if (err != ESP_OK) {
        return err; /* 通常是 ESP_ERR_INVALID_STATE：测量核还没写过数据 */
    }

    int n = snprintf(buf, buf_len,
                     "{\"ok\":true,"
                     "\"voltage_v\":%.2f,\"current_a\":%.3f,\"power_w\":%.2f,"
                     "\"energy_wh\":%.2f,\"temperature_c\":%.1f,\"humidity_pct\":%.1f,"
                     "\"battery_mv\":%u,\"runtime_s\":%u}",
                     (double)snap.voltage_v, (double)snap.current_a,
                     (double)snap.power_w, (double)snap.energy_wh,
                     (double)snap.temperature_c, (double)snap.humidity_pct,
                     (unsigned)snap.battery_mv, (unsigned)snap.runtime_s);
    if (n <= 0 || (size_t)n >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/**
 * @brief 发送 JSON 应答（状态行 + Content-Type + 正文一步到位）
 *
 * @param req    当前请求
 * @param status HTTP 状态行，如 "200 OK" / "503 Service Unavailable"
 * @param body   JSON 正文（'\0' 结尾）
 * @return 透传 httpd_resp_send 的结果
 */
static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    /* 故意不加 Access-Control-Allow-Origin：页面与接口同源，不需要 CORS；
     * 不放开跨域还能挡住「别的网页代发参数」这类 CSRF 尝试 */
    return httpd_resp_sendstr(req, body);
}

/* ============================ 手写 JSON 解析 ============================ */

/**
 * 为什么手写：见文件头②。本解析器只接受「扁平 JSON 对象」——
 *   {"键1":值1,"键2":值2,...}
 * 值支持 字符串 / 数字 / true / false / null（嵌套对象与数组会报错），
 * 统一转成字符串后逐对回调，正好匹配「参数下发 = 键值对」的需求。
 */
typedef esp_err_t (*json_kv_cb_t)(const char *key, const char *value, void *ctx);

/**
 * @brief 解码 JSON 字符串字面量（输入指向开引号之后）
 *
 * 处理标准转义：\" \\ \/ \b \f \n \r \t \uXXXX（含 UTF-16 代理对 → UTF-8）。
 * 非转义的原始字节（含中文 UTF-8）直接透传。
 *
 * @param p       输入游标（开引号之后）
 * @param out     输出缓冲
 * @param out_len 输出缓冲容量（含结尾 '\0'）
 * @return 闭引号之后的游标；语法错误或缓冲不足返回 NULL
 */
static const char *json_decode_string(const char *p, char *out, size_t out_len)
{
    size_t o = 0;

#define JSON_PUSH_BYTE(b)                       \
    do {                                        \
        if (o + 1 >= out_len) {                 \
            return NULL; /* 值超长：直接判非法 */ \
        }                                       \
        out[o++] = (char)(b);                   \
    } while (0)

    while (*p != '\0' && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            char esc = *p++;
            switch (esc) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                /* \uXXXX：先读 4 位十六进制；若是高代理对，再读 \uXXXX 合并 */
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = *p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9')      cp |= (uint32_t)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
                    else return NULL;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = *p++;
                        lo <<= 4;
                        if (h >= '0' && h <= '9')      lo |= (uint32_t)(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= (uint32_t)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= (uint32_t)(h - 'A' + 10);
                        else return NULL;
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        return NULL;
                    }
                }
                /* 码点 → UTF-8 */
                if (cp < 0x80) {
                    JSON_PUSH_BYTE(cp);
                } else if (cp < 0x800) {
                    JSON_PUSH_BYTE(0xC0 | (cp >> 6));
                    JSON_PUSH_BYTE(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    JSON_PUSH_BYTE(0xE0 | (cp >> 12));
                    JSON_PUSH_BYTE(0x80 | ((cp >> 6) & 0x3F));
                    JSON_PUSH_BYTE(0x80 | (cp & 0x3F));
                } else {
                    JSON_PUSH_BYTE(0xF0 | (cp >> 18));
                    JSON_PUSH_BYTE(0x80 | ((cp >> 12) & 0x3F));
                    JSON_PUSH_BYTE(0x80 | ((cp >> 6) & 0x3F));
                    JSON_PUSH_BYTE(0x80 | (cp & 0x3F));
                }
                continue;
            }
            default:
                return NULL; /* 未知转义：宁可报错也不猜 */
            }
            JSON_PUSH_BYTE(c);
        } else if (c < 0x20) {
            return NULL; /* JSON 规定控制字符必须转义 */
        } else {
            JSON_PUSH_BYTE(c);
        }
    }
#undef JSON_PUSH_BYTE

    if (*p != '"') {
        return NULL; /* 字符串未闭合 */
    }
    out[o] = '\0';
    return p + 1;
}

/**
 * @brief 解码 JSON 值（字符串 / 数字 / true / false / null），统一转成文本
 *
 * @param p       输入游标（值的第一个字符）
 * @param out     输出缓冲
 * @param out_len 输出缓冲容量（含结尾 '\0'）
 * @return 值之后的游标；语法错误或缓冲不足返回 NULL
 */
static const char *json_decode_value(const char *p, char *out, size_t out_len)
{
    if (*p == '"') {
        return json_decode_string(p + 1, out, out_len);
    }

    /* 字面量 true / false / null：原样拷贝 */
    static const char *const kLiterals[] = { "true", "false", "null" };
    for (size_t i = 0; i < sizeof(kLiterals) / sizeof(kLiterals[0]); i++) {
        size_t n = strlen(kLiterals[i]);
        if (strncmp(p, kLiterals[i], n) == 0) {
            if (n + 1 > out_len) {
                return NULL;
            }
            memcpy(out, kLiterals[i], n + 1);
            return p + n;
        }
    }

    /* 数字：拷贝字面文本（如 "-12.50"），转数值留给上层用 strtof/atoi */
    size_t o = 0;
    while (isdigit((unsigned char)*p) || *p == '-' || *p == '+' ||
           *p == '.' || *p == 'e' || *p == 'E') {
        if (o + 1 >= out_len) {
            return NULL;
        }
        out[o++] = *p++;
    }
    if (o == 0) {
        return NULL; /* 既不是字符串/字面量，也不是数字 */
    }
    out[o] = '\0';
    return p;
}

/**
 * @brief 解析「扁平 JSON 对象」，逐对回调
 *
 * @param text 待解析文本（'\0' 结尾）
 * @param cb   键值对回调（不可为 NULL）；回调返回非 ESP_OK 立即中止并透传该错误码
 * @param ctx  透传给回调的上下文
 * @return ESP_OK 解析并回调成功
 *         ESP_ERR_INVALID_ARG  JSON 语法/形状错误（含空对象、嵌套对象）
 *         其他                 回调返回的错误码
 */
static esp_err_t json_parse_flat_object(const char *text, json_kv_cb_t cb, void *ctx)
{
    const char *p = text;
    char key[WEB_CONSOLE_KEY_MAX];
    char value[WEB_CONSOLE_VALUE_MAX];
    int count = 0;

    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '{') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;

    while (1) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '}') {
            p++;
            break;
        }
        if (*p != '"') {
            return ESP_ERR_INVALID_ARG; /* 键必须是字符串 */
        }
        p = json_decode_string(p + 1, key, sizeof(key));
        if (p == NULL) {
            return ESP_ERR_INVALID_ARG;
        }

        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != ':') {
            return ESP_ERR_INVALID_ARG;
        }
        p++;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        p = json_decode_value(p, value, sizeof(value));
        if (p == NULL) {
            return ESP_ERR_INVALID_ARG;
        }

        count++;
        esp_err_t err = cb(key, value, ctx);
        if (err != ESP_OK) {
            return err; /* 回调失败：错误码原样上抛，由调用方与语法错误区分 */
        }

        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            p++;
            break;
        }
        return ESP_ERR_INVALID_ARG;
    }

    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0') {
        return ESP_ERR_INVALID_ARG; /* '}' 之后不允许有残留 */
    }
    return (count > 0) ? ESP_OK : ESP_ERR_INVALID_ARG; /* 空对象视为参数错误 */
}

/* ============================ 参数下发（回调注入） ============================ */

/** POST /api/config 的逐对分发上下文 */
typedef struct {
    web_console_config_handler_t handler;
    int applied;                          /**< 已成功写入的键数 */
    char bad_key[WEB_CONSOLE_KEY_MAX];    /**< 出错的键（供错误应答定位） */
    esp_err_t err;                        /**< 回调返回的错误码 */
} config_apply_ctx_t;

/**
 * @brief 把一个键值对交给上层回调
 *
 * 只记录键、不记录值：参数里可能有密码类内容，日志不落敏感数据。
 */
static esp_err_t config_apply_kv_cb(const char *key, const char *value, void *ctx)
{
    config_apply_ctx_t *c = (config_apply_ctx_t *)ctx;
    (void)value;

    esp_err_t err = c->handler(key, value);
    if (err != ESP_OK) {
        snprintf(c->bad_key, sizeof(c->bad_key), "%s", key);
        c->err = err;
        ESP_LOGW(TAG, "参数 [%s] 写入失败：%s", key, esp_err_to_name(err));
        return err;
    }
    c->applied++;
    ESP_LOGI(TAG, "参数已写入：[%s]", key);
    return ESP_OK;
}

/**
 * @brief 读取请求体到堆缓冲（调用方负责 free）
 *
 * @param req     当前请求
 * @param max_len 请求体长度上限
 * @return '\0' 结尾的正文；空正文、超限或接收失败返回 NULL
 */
static char *read_request_body(httpd_req_t *req, size_t max_len)
{
    if (req->content_len == 0 || req->content_len > max_len) {
        return NULL;
    }

    char *buf = (char *)malloc(req->content_len + 1);
    if (buf == NULL) {
        return NULL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) { /* 对端中断 / 超时：放弃本请求 */
            free(buf);
            return NULL;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return buf;
}

/* ============================ HTTP URI 处理器 ============================ */

/** GET /：内嵌单页 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* 页面是常量字符串，直接整发；不缓存，避免浏览器拿旧页面配新固件 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, web_console_page_html);
}

static void hist_tick(void); /* 前向声明（定义见下方历史记录模块） */

/** GET /api/state：当前测量快照 */
static esp_err_t api_state_get_handler(httpd_req_t *req)
{
    char json[WEB_CONSOLE_JSON_BUF_LEN];
    esp_err_t err = build_state_json(json, sizeof(json));
    if (err == ESP_OK) {
        hist_tick(); /* F9：轮询场景也捎带记录（WS 未启用时的唯一节拍来源） */
        return send_json(req, HTTPD_200, json);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json(req, "503 Service Unavailable",
                         "{\"ok\":false,\"code\":\"no_data\",\"msg\":\"no measurement yet\"}");
    }
    return send_json(req, HTTPD_500,
                     "{\"ok\":false,\"code\":\"internal\",\"msg\":\"build json failed\"}");
}

/* ============================ 历史记录与 CSV 导出（F9） ============================
 * 分钟级内存环：每 60s 记一帧快照，保留 60 点（1 小时），GET /api/export.csv
 * 逐行分块输出。记录挂在快照 JSON 生成路径上（ws 推送与 /api/state 都会走到），
 * 用墙钟节拍去重，WS 未启用/页面轮询场景同样有数据。
 * 历史不落 Flash：写存储限频与寿命管理在 v1.0.0「连续记录」再做，本版先满足
 * 「带走最近一小时数据」的最小导出需求。 */
#define HIST_POINTS 60
typedef struct {
    int64_t ts_us;   /* 记录时刻（esp_timer µs，导出时转分钟相对时） */
    data_store_snapshot_t snap;
} hist_point_t;
static hist_point_t s_hist[HIST_POINTS];
static int s_hist_head = 0;   /* 下一个写入位 */
static int s_hist_count = 0;
static int64_t s_hist_last_us = 0;

/** 快照历史记录（墙钟 60s 节拍；由快照生成路径捎带触发） */
static void hist_tick(void)
{
    data_store_snapshot_t snap;
    if (data_store_get(&snap) != ESP_OK) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_hist_last_us != 0 && (now - s_hist_last_us) < 60 * 1000000LL) {
        return;
    }
    s_hist_last_us = now;
    s_hist[s_hist_head].ts_us = now;
    s_hist[s_hist_head].snap = snap;
    s_hist_head = (s_hist_head + 1) % HIST_POINTS;
    if (s_hist_count < HIST_POINTS) {
        s_hist_count++;
    }
}

/** GET /api/export.csv：导出分钟级历史（text/csv，附件下载） */
static esp_err_t api_export_csv_handler(httpd_req_t *req)
{
    hist_tick(); /* 导出前顺手补一帧最新，保证含"此刻" */
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"xt60-history.csv\"");

    /* 表头 + 逐行分块发送（60 行约 6KB，chunk 避免大缓冲） */
    char line[160];
    snprintf(line, sizeof(line),
             "t_min,voltage_v,current_a,power_w,energy_wh,capacity_ah,temp_c,humidity_pct,"
             "battery_mv,runtime_s\r\n");
    if (httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    const int base_idx = (s_hist_head - s_hist_count + HIST_POINTS) % HIST_POINTS;
    const int64_t base = (s_hist_count > 0) ? s_hist[base_idx].ts_us : 0;
    for (int k = 0; k < s_hist_count; k++) {
        const hist_point_t *h = &s_hist[(base_idx + k) % HIST_POINTS];
        const double t_min = (double)(h->ts_us - base) / 60000000.0;
        snprintf(line, sizeof(line), "%.2f,%.3f,%.4f,%.3f,%.4f,%.4f,%.1f,%.0f,%u,%u\r\n",
                 t_min, (double)h->snap.voltage_v, (double)h->snap.current_a,
                 (double)h->snap.power_w, (double)h->snap.energy_wh, (double)h->snap.capacity_ah,
                 (double)h->snap.temperature_c, (double)h->snap.humidity_pct,
                 (unsigned)h->snap.battery_mv, (unsigned)h->snap.runtime_s);
        if (httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0); /* 结束块 */
}

/** POST /api/config：JSON 键值对 → 注册的回调 */
static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    /* ① 超限与空正文先挡掉，避免无谓分配 */
    if (req->content_len > WEB_CONSOLE_CFG_BODY_MAX) {
        return send_json(req, "413 Payload Too Large",
                         "{\"ok\":false,\"code\":\"too_large\",\"msg\":\"body too large\"}");
    }
    char *body = read_request_body(req, WEB_CONSOLE_CFG_BODY_MAX);
    if (body == NULL) {
        return send_json(req, HTTPD_400,
                         "{\"ok\":false,\"code\":\"bad_request\",\"msg\":\"empty or unreadable body\"}");
    }

    /* ② 回调未注册：503（服务可用但该能力未配置） */
    web_console_config_handler_t handler = s_config_handler;
    if (handler == NULL) {
        free(body);
        return send_json(req, "503 Service Unavailable",
                         "{\"ok\":false,\"code\":\"no_handler\",\"msg\":\"config handler not registered\"}");
    }

    /* ③ 解析并逐对分发 */
    config_apply_ctx_t ctx = {
        .handler = handler,
        .applied = 0,
        .bad_key = "",
        .err = ESP_OK,
    };
    esp_err_t err = json_parse_flat_object(body, config_apply_kv_cb, &ctx);
    free(body);

    char resp[192];
    if (err == ESP_OK) {
        return send_json(req, HTTPD_200, "{\"ok\":true}");
    }
    if (ctx.err != ESP_OK) {
        /* 回调失败：带上出错的键与错误码，便于上层/前端定位 */
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"code\":\"apply_failed\",\"key\":\"%s\",\"esp_err\":%d,"
                 "\"msg\":\"apply failed\"}",
                 ctx.bad_key, (int)ctx.err);
        return send_json(req, HTTPD_500, resp);
    }
    snprintf(resp, sizeof(resp),
             "{\"ok\":false,\"code\":\"bad_request\",\"msg\":\"bad json or empty object\","
             "\"applied\":%d}",
             ctx.applied);
    return send_json(req, HTTPD_400, resp);
}

/* ============================ WebSocket ============================ */

#if CONFIG_HTTPD_WS_SUPPORT

/** WS 上行命令帧长度上限（页面只发 {"cmd":"..."}，256 字节绰绰有余） */
#define WS_CMD_MAX 256

/**
 * @brief 异步发送完成回调（在 httpd 任务里执行）
 *
 * 失败静默降级：只留 DEBUG 痕迹，不刷屏；顺手让服务器回收该会话，
 * 避免之后每 200ms 都向死连接排一次发送。
 *
 * @param err    发送结果
 * @param socket 目标 socket fd
 * @param arg    发送缓冲（由本函数释放）
 */
static void ws_send_done_cb(esp_err_t err, int socket, void *arg)
{
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "WS 发送失败（fd=%d，%s），静默降级", socket, esp_err_to_name(err));
        if (s_server != NULL) {
            httpd_sess_trigger_close(s_server, socket);
        }
    }
    free(arg);
}

/**
 * @brief 向所有 WS 客户端广播一段 JSON 文本
 *
 * 必须走 httpd_ws_send_data_async()（内部排入 httpd 工作队列），
 * 因为 socket 写入只能发生在 httpd 任务上下文。
 * payload 每客户端各拷一份堆缓冲，发送完成回调里释放。
 *
 * @param json JSON 文本（'\0' 结尾）
 */
static void ws_broadcast(const char *json)
{
    httpd_handle_t server = s_server;
    if (server == NULL) {
        return;
    }

    int fds[WEB_CONSOLE_MAX_CLIENTS];
    size_t n_fds = WEB_CONSOLE_MAX_CLIENTS;
    if (httpd_get_client_list(server, &n_fds, fds) != ESP_OK) {
        return;
    }

    size_t len = strlen(json);
    for (size_t i = 0; i < n_fds; i++) {
        if (httpd_ws_get_fd_info(server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue; /* 只推 WS 会话，普通 HTTP 会话跳过 */
        }

        char *copy = (char *)malloc(len + 1);
        if (copy == NULL) {
            continue; /* 内存紧张就少推一帧，下一个周期自会补上 */
        }
        memcpy(copy, json, len + 1);

        httpd_ws_frame_t frame = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)copy,
            .len = len,
            .left_len = 0,
        };
        if (httpd_ws_send_data_async(server, fds[i], &frame, ws_send_done_cb, copy) != ESP_OK) {
            free(copy); /* 没排进队列，回调不会来，自己收尾 */
        }
    }
}

/** WS 上行命令的解析上下文 */
typedef struct {
    char cmd[16]; /**< 命令名（"ping" / "state"） */
} ws_cmd_ctx_t;

/** 提取 {"cmd":"..."} 中的 cmd 值，其余键忽略 */
static esp_err_t ws_cmd_kv_cb(const char *key, const char *value, void *ctx)
{
    ws_cmd_ctx_t *c = (ws_cmd_ctx_t *)ctx;
    if (strcmp(key, "cmd") == 0) {
        snprintf(c->cmd, sizeof(c->cmd), "%s", value);
    }
    return ESP_OK;
}

/**
 * @brief /ws 处理器：消费上行帧并响应命令
 *
 * 握手由 esp_http_server 内部完成（v6.1 起握手不再进入本处理器），
 * 本处理器只在真正收到数据帧时被调用；因此**必须**把帧读干净，
 * 否则残留字节会破坏后续帧流。
 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;

    /* 先取帧长（max_len=0 不读 payload） */
    esp_err_t err = httpd_ws_recv_frame(req, &pkt, 0);
    if (err != ESP_OK) {
        return err; /* 帧流已不可信，让服务器关掉连接，客户端自动重连 */
    }
    if (pkt.len == 0) {
        return ESP_OK; /* 空帧（如 PONG） */
    }

    uint8_t buf[WS_CMD_MAX];
    if (pkt.len > sizeof(buf)) {
        /* 超大帧：按块收下丢弃（保持帧流对齐），不作为命令处理 */
        uint8_t sink[64];
        pkt.payload = sink;
        while (pkt.left_len > 0) {
            size_t chunk = (pkt.left_len < sizeof(sink)) ? pkt.left_len : sizeof(sink);
            err = httpd_ws_recv_frame_part(req, &pkt, chunk);
            if (err != ESP_OK) {
                return err;
            }
        }
        ESP_LOGD(TAG, "忽略超大 WS 帧（%u 字节）", (unsigned)pkt.len);
        return ESP_OK;
    }

    pkt.payload = buf;
    err = httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    /* 只处理文本帧；二进制帧静默忽略 */
    if (pkt.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }
    buf[pkt.len < sizeof(buf) ? pkt.len : sizeof(buf) - 1] = '\0';

    ws_cmd_ctx_t cmd = { .cmd = "" };
    if (json_parse_flat_object((const char *)buf, ws_cmd_kv_cb, &cmd) != ESP_OK) {
        return ESP_OK; /* 非 JSON / 非对象：忽略，不惩罚客户端 */
    }

    if (strcmp(cmd.cmd, "ping") == 0) {
        httpd_ws_frame_t pong = {
            .final = true,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)"{\"ok\":true,\"pong\":true}",
            .len = strlen("{\"ok\":true,\"pong\":true}"),
        };
        return httpd_ws_send_frame(req, &pong);
    }

    if (strcmp(cmd.cmd, "state") == 0) {
        char json[WEB_CONSOLE_JSON_BUF_LEN];
        if (build_state_json(json, sizeof(json)) != ESP_OK) {
            return ESP_OK; /* 没数据就不回，避免客户端误判出错 */
        }
        httpd_ws_frame_t frame = {
            .final = true,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json),
        };
        return httpd_ws_send_frame(req, &frame);
    }

    return ESP_OK; /* 未知命令：静默忽略 */
}

/**
 * @brief WS 快照推送任务：每 200ms 读一次 data_store 并广播
 *
 * 收尾方式（刻意为之）：退出循环后**挂起而不是自删**，由 web_console_stop()
 * 统一 vTaskDelete()。如果任务自删、stop() 又在超时后强删，两条删除路径
 * 可能撞在同一瞬间（删到一个已销毁的任务是未定义行为）；「只删不自删」
 * 把删除收敛到单一调用方，彻底消除这个竞态窗口。
 */
static void ws_push_task(void *arg)
{
    (void)arg;
    char json[WEB_CONSOLE_JSON_BUF_LEN];

    while (!s_push_stop_req) {
        if (build_state_json(json, sizeof(json)) == ESP_OK) {
            ws_broadcast(json);
        }
        hist_tick(); /* F9：分钟级历史记录（内部 60s 墙钟节拍去重） */
        vTaskDelay(pdMS_TO_TICKS(WEB_CONSOLE_PUSH_PERIOD_MS));
    }

    s_push_parked = true; /* 告诉 stop()：随时可以安全删除本任务 */
    for (;;) {
        vTaskSuspend(NULL);
    }
}

#else /* !CONFIG_HTTPD_WS_SUPPORT */

/**
 * @brief 未启用 CONFIG_HTTPD_WS_SUPPORT 时的 /ws 兜底：回 501
 *
 * 页面的 JS 会因握手失败自动退化为「每 500ms 轮询 /api/state」，
 * 功能不缺失，只是少了推送。
 */
static esp_err_t ws_unsupported_handler(httpd_req_t *req)
{
    return send_json(req, "501 Not Implemented",
                     "{\"ok\":false,\"code\":\"ws_disabled\","
                     "\"msg\":\"CONFIG_HTTPD_WS_SUPPORT is not enabled\"}");
}

#endif /* CONFIG_HTTPD_WS_SUPPORT */

/* ============================ 生命周期 ============================ */

esp_err_t web_console_set_config_handler(web_console_config_handler_t handler)
{
    /* 32 位指针写入在 Xtensa 上是原子的，无需加锁；
     * 正在执行的旧回调继续跑完，新请求用新回调 */
    s_config_handler = handler;
    return ESP_OK;
}

esp_err_t web_console_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "已在运行，重复 start 返回 ESP_ERR_INVALID_STATE");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_CONSOLE_PORT;
    config.max_open_sockets = WEB_CONSOLE_MAX_CLIENTS;
    config.max_uri_handlers = 5; /* / 、/ws 、/api/state 、/api/config 、/api/export.csv */
    config.max_resp_headers = 4;
    config.stack_size = 6144;    /* URI 处理器要拼 JSON + 回调上层写 NVS，比默认 4096 留宽些 */
    config.lru_purge_enable = true; /* 连接满时淘汰最久未用的，避免僵尸会话占坑 */
    config.core_id = 0;          /* 与工程约定一致：Core 0 = 交互与通信 */

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败：%s", esp_err_to_name(err));
        return err;
    }

    /* URI 注册：任何一个失败都要整体回滚，不能留半个服务器 */
    static const httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    static const httpd_uri_t uri_state = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = api_state_get_handler,
    };
    static const httpd_uri_t uri_export = {
        .uri = "/api/export.csv",
        .method = HTTP_GET,
        .handler = api_export_csv_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t uri_config = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = api_config_post_handler,
    };
#if CONFIG_HTTPD_WS_SUPPORT
    static const httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true, /* v6.1：握手由 httpd 内部处理，不进 handler */
    };
#else
    static const httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_unsupported_handler,
    };
#endif

    if (httpd_register_uri_handler(server, &uri_root) != ESP_OK ||
        httpd_register_uri_handler(server, &uri_ws) != ESP_OK ||
        httpd_register_uri_handler(server, &uri_state) != ESP_OK ||
        httpd_register_uri_handler(server, &uri_config) != ESP_OK ||
        httpd_register_uri_handler(server, &uri_export) != ESP_OK) {
        ESP_LOGE(TAG, "URI 注册失败");
        httpd_stop(server);
        return ESP_FAIL;
    }

    s_server = server; /* 先落句柄，推送任务的广播要用它 */

#if CONFIG_HTTPD_WS_SUPPORT
    s_push_stop_req = false;
    TaskHandle_t task = NULL;
    /* 任务栈放 PSRAM（MALLOC_CAP_SPIRAM）：WS 推送任务不做 ISR 相关操作，
     * 不需要内部 RAM 栈；内部 RAM 仅剩极少余量（v0.5.0 实测），4KB 内部栈
     * 会创建失败（ESP_ERR_NO_MEM）。⚠ 测量采样任务仍必须内部 RAM 栈
     * （见架构文档：ISR 在任务栈上运行且不能访问 PSRAM）。 */
    if (xTaskCreatePinnedToCoreWithCaps(ws_push_task, "web_console_ws",
                                        WEB_CONSOLE_PUSH_TASK_STACK, NULL,
                                        WEB_CONSOLE_PUSH_TASK_PRIO, &task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "WS 推送任务创建失败");
        s_server = NULL;
        httpd_stop(server);
        return ESP_ERR_NO_MEM;
    }
    /* 任务句柄由创建接口返回（而不是任务自己登记），
     * 避免 stop() 紧随 start() 时读到「任务还没登记」的空句柄 */
    s_push_task = task;
#endif
    ESP_LOGI(TAG, "网页控制台已启动：http://<设备IP>:%d/（WS %s）",
             WEB_CONSOLE_PORT,
#if CONFIG_HTTPD_WS_SUPPORT
             "已启用，约 5Hz 推送"
#else
             "未启用（CONFIG_HTTPD_WS_SUPPORT=n），页面将退化为轮询"
#endif
    );
    return ESP_OK;
}

esp_err_t web_console_stop(void)
{
    if (s_server == NULL) {
        ESP_LOGW(TAG, "未在运行，stop 返回 ESP_ERR_INVALID_STATE");
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_HTTPD_WS_SUPPORT
    /* 先停推送任务，再停服务器：
     * 顺序反了的话，推送任务可能拿着已销毁的句柄去排发送 */
    s_push_stop_req = true;
    for (int i = 0; i < 100 && s_push_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10)); /* 推送周期 200ms，等 1s 足够收尾 */
    }
    if (s_push_task != NULL) {
        ESP_LOGW(TAG, "WS 推送任务未按时退出，强制删除");
        vTaskDelete((TaskHandle_t)s_push_task);
        s_push_task = NULL;
    }
#endif

    httpd_handle_t server = s_server;
    s_server = NULL; /* 先清句柄，让可能残留的异步回调失去目标 */
    esp_err_t err = httpd_stop(server);
    ESP_LOGI(TAG, "网页控制台已停止（%s）", esp_err_to_name(err));
    return err;
}
