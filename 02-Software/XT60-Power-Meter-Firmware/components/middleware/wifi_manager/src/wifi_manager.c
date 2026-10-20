/**
 * @file wifi_manager.c
 * @brief WiFi 连接管理实现：事件驱动的 STA/AP 状态机 + NVS 凭据持久化
 *
 * ─────────────────────────────────────────────────────────────
 * 整体结构（谁在什么时候做事）
 * ─────────────────────────────────────────────────────────────
 * ① esp_event 回调（wifi_event_handler / ip_event_handler）：
 *    运行在系统默认事件循环的任务里，只做三件“几条指令”的事——
 *    拷贝事件里的关键字段、置事件位、结束。绝不写 NVS、不做耗时操作，
 *    也不在这里做状态迁移（状态迁移涉及退避计时，交给下面的任务）。
 *
 * ② 内部任务 wifi_manager_task（唯一的状态迁移执行者）：
 *    等事件位（可超时），醒后依次处理：外部命令 → WiFi 事件 → 退避重试。
 *    所有 NVS 写入（保存/擦除凭据）都只发生在本任务上下文——
 *    这正是“回调不得做 NVS 写、挂到自己的任务里做”的落点。
 *
 * ③ 公开 API（wifi_manager_connect / forget）：
 *    把请求打包成命令投递给内部任务，然后等一个“处理完成”事件位。
 *    用命令队列而不是直接调用内部函数，是为了让 NVS 写与模式切换
 *    全部串行化在一个任务里，调用方无需与事件回调抢任何锁。
 *
 * ─────────────────────────────────────────────────────────────
 * 状态机（与 wifi_manager.h 的五态契约一一对应）
 * ─────────────────────────────────────────────────────────────
 *   IDLE ──start(有凭据)──▶ CONNECTING ──GOT_IP──▶ CONNECTED
 *     │                        │  ▲                  │
 *     │                        │  └──退避重试─────────┤（意外掉线）
 *     │                        └──重试耗尽──▶ FAILED │
 *     └──start(无凭据)──▶ AP_MODE ◀──forget()────────┘
 *   任意状态 ──stop()──▶ IDLE
 *
 * 退避重试：首次连接失败后最多再试 WIFI_MANAGER_RETRY_MAX 次，
 * 间隔 1s / 2s / 4s / 8s / 8s（指数增长、封顶）；耗尽后进入 FAILED
 * 并停在那里——不重启设备、不擦凭据，保留上层用 wifi_manager_connect()
 * 再试或 wifi_manager_forget() 回配网的选择权。
 *
 * ─────────────────────────────────────────────────────────────
 * 其他设计取舍
 * ─────────────────────────────────────────────────────────────
 * · NVS 不可用时的降级：只告警不中止——无凭据就进 AP 配网模式，
 *   保存凭据时仅内存生效。绝不擦除整个 NVS 分区：分区里还有
 *   services/settings（xt60cfg）等别人的数据，本组件无权处置。
 * · esp_wifi 的存储模式设为 WIFI_STORAGE_RAM：凭据由本组件自管到
 *   命名空间 "wifi"，避免 WiFi 驱动再往 NVS 落一份不可控的副本。
 * · 共享数据（状态 / 凭据缓存 / IP / 扫描结果）用自旋锁保护，
 *   临界区里只做小段拷贝，绝不打日志（与 settings.c 同一套做法）。
 * · stop() 保留默认事件循环与 netif 对象：它们由多个组件共享
 *   （esp_netif_init() 约定“整个应用只调一次”），只反初始化 WiFi 驱动。
 */
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_bit_defs.h"    /* BIT0~BIT7 位掩码宏（事件位定义用） */
#include "esp_event.h"       /* 事件循环：注册 WIFI_EVENT / IP_EVENT 处理器 */
#include "esp_log.h"         /* ESP-IDF 日志库 */
#include "esp_mac.h"         /* esp_read_mac：取芯片 MAC 生成配网热点名 */
#include "esp_netif.h"       /* esp_netif_init / esp_netif_create_default_wifi_* */
#include "esp_wifi.h"        /* WiFi 驱动配置与连接控制 */
#include "esp_wifi_default.h" /* esp_netif_create_default_wifi_sta/ap 的声明 */
#include "freertos/FreeRTOS.h" /* FreeRTOS 核心（portMUX_TYPE、TickType_t 等） */
#include "freertos/event_groups.h" /* 事件组：事件回调 → 内部任务的“发生了什么” */
#include "freertos/queue.h"  /* 队列：公开 API → 内部任务的命令通道 */
#include "freertos/semphr.h" /* 互斥锁：串行化 connect/forget 两条命令通道 */
#include "freertos/task.h"   /* 任务：状态机执行体 */
#include "nvs.h"             /* NVS 键值读写（凭据持久化） */
#include "nvs_flash.h"       /* NVS 分区初始化（仅在尚未初始化时兜底） */

/** 日志标签 */
static const char *TAG = "wifi_manager";

/* ============================================================
 * 常量配置
 * ============================================================ */

/** NVS 命名空间与键名（自管，不碰 xt60cfg / pm；NVS 键名限 15 字符） */
#define WIFI_MANAGER_NVS_NAMESPACE "wifi"
#define WIFI_MANAGER_NVS_KEY_SSID  "ssid"
#define WIFI_MANAGER_NVS_KEY_PASS  "pass"
/** 工作模式偏好键（v0.6.0 新增）：u8 类型，取值见 wifi_manager_mode_t */
#define WIFI_MANAGER_NVS_KEY_MODE  "mode"

/** 连接失败后的最大重试次数（不含首次尝试）；耗尽即进入 FAILED */
#define WIFI_MANAGER_RETRY_MAX 5

/** 重试退避：首两次间隔 1s，逐次翻倍，封顶 8s */
#define WIFI_MANAGER_RETRY_BASE_MS 1000
#define WIFI_MANAGER_RETRY_MAX_MS  8000

/** 内部任务栈大小与优先级
 * 栈要装得下 NVS 写与 WiFi 配置调用；优先级取 4：
 * 高于 settings 落盘（2），低于 UI——网络不应抢交互的响应。 */
#define WIFI_MANAGER_TASK_STACK 4096
#define WIFI_MANAGER_TASK_PRIO  4

/** 扫描结果缓存条数（配网页按信号强度展示前若干个足够用） */
#define WIFI_MANAGER_SCAN_MAX_AP 16

/** 命令队列深度与命令处理超时（毫秒） */
#define WIFI_MANAGER_CMD_QUEUE_LEN  4
#define WIFI_MANAGER_CMD_TIMEOUT_MS 5000

/** 配网热点信道（2.4G 中间信道，干扰与覆盖的折中） */
#define WIFI_MANAGER_AP_CHANNEL 6

/** 配网热点密码（WPA2-PSK，≥8 字符）；量产改设备唯一码贴机身 */
#define WIFI_MANAGER_AP_PASS "xt60config"

/** 配网热点最多允许接入的终端数（配网场景 1~2 台，留点余量） */
#define WIFI_MANAGER_AP_MAX_CONN 4

/** IP 字符串最短缓冲（"255.255.255.255" + 结尾 0 = 16） */
#define WIFI_MANAGER_IP_STR_LEN 16

/* ============================================================
 * 事件位与命令定义
 * ============================================================ */

/* 事件组位：esp_event 回调 → 内部任务（只表示“发生了什么”，可合并） */
#define WM_EVT_STA_START    BIT0 /**< WIFI_EVENT_STA_START：STA 接口已启动 */
#define WM_EVT_STA_STOP     BIT1 /**< WIFI_EVENT_STA_STOP：STA 接口已停止 */
#define WM_EVT_DISCONNECTED BIT2 /**< WIFI_EVENT_STA_DISCONNECTED：连接失败/掉线 */
#define WM_EVT_GOT_IP       BIT3 /**< IP_EVENT_STA_GOT_IP：已拿到 IP */
#define WM_EVT_LOST_IP      BIT4 /**< IP_EVENT_STA_LOST_IP：IP 丢失 */
#define WM_EVT_SCAN_DONE    BIT5 /**< WIFI_EVENT_SCAN_DONE：扫描完成 */
#define WM_EVT_CMD          BIT6 /**< 有外部命令待处理 */
#define WM_EVT_EXIT         BIT7 /**< stop()：内部任务请退出 */
#define WM_EVT_ALL          (BIT0 | BIT1 | BIT2 | BIT3 | BIT4 | BIT5 | BIT6 | BIT7)

/* 命令完成事件位（内部任务 → 等待中的 connect/forget 调用方） */
#define WM_CMD_DONE BIT0

/** 命令类型 */
typedef enum {
    WIFI_MANAGER_CMD_CONNECT = 0, /**< 保存凭据并切换到 STA 连接 */
    WIFI_MANAGER_CMD_FORGET,      /**< 清除凭据并回到 AP 配网模式 */
    WIFI_MANAGER_CMD_SET_MODE,    /**< 设置工作模式偏好（v0.6.0）：写 NVS + 切换模式 */
} wifi_manager_cmd_type_t;

/** 命令载荷（经队列整体拷贝，带足额缓冲避免悬垂指针） */
typedef struct {
    wifi_manager_cmd_type_t type;
    char ssid[WIFI_MANAGER_SSID_MAX_LEN + 1]; /**< 热点名（含结尾 0） */
    char pass[WIFI_MANAGER_PASS_MAX_LEN];     /**< 密码（含结尾 0，空串=开放热点） */
    uint8_t mode;                             /**< 目标模式（仅 SET_MODE 使用） */
} wifi_manager_cmd_t;

/* ============================================================
 * 内部状态
 * ============================================================ */

/**
 * 共享数据自旋锁
 *
 * 保护：状态、凭据缓存、IP 信息、扫描缓存。
 * 这些数据会被内部任务（写）与任意调用方/事件回调（读）并发访问；
 * 临界区里只做几字节~2KB 的拷贝，绝不打日志（见文件头）。
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/** 组件是否已启动（公开 API 的准入开关） */
static volatile bool s_started = false;

/** 当前工作状态（仅内部任务写，读方经自旋锁） */
static wifi_manager_state_t s_state = WIFI_MANAGER_STATE_IDLE;

/** 凭据缓存（NVS 的内存镜像，供 get_ssid 与重连使用） */
static char s_ssid[WIFI_MANAGER_SSID_MAX_LEN + 1];
static char s_pass[WIFI_MANAGER_PASS_MAX_LEN];
static bool s_have_creds = false;

/** 配网热点名（"XT60-Meter-XXXX"） */
static char s_ap_ssid[WIFI_MANAGER_AP_SSID_MAX_LEN];

/** 工作模式偏好（v0.6.0）：内存快照，内部任务写、任意调用方读（经自旋锁）。
 *  初值 AUTO 与"NVS 中还没有 mode 键"的语义一致（新设备/未设置过）。 */
static wifi_manager_mode_t s_mode = WIFI_MANAGER_MODE_AUTO;

/** STA 获取到的 IP 信息与有效标记 */
static esp_netif_ip_info_t s_ip_info;
static bool s_has_ip = false;

/** 扫描结果缓存（内部任务写、调用方读） */
static wifi_ap_record_t s_scan_list[WIFI_MANAGER_SCAN_MAX_AP];
static uint16_t s_scan_count = 0;

/** 扫描进行中标记（v0.6.0）：配网 UI 轮询它来判断"扫完了没有"。
 *  esp_wifi 没有公开的"是否在扫描"查询，故由本组件自己记账：
 *  scan_start() 置位，SCAN_DONE 处理完清零。 */
static volatile bool s_scan_busy = false;

/** 扫描取数暂存区（仅内部任务使用；先取到暂存再拷进缓存，避免读者撕裂） */
static wifi_ap_record_t s_scan_scratch[WIFI_MANAGER_SCAN_MAX_AP];

/** NVS 是否可用（不可用时凭据只存内存，见文件头“降级”说明） */
static bool s_nvs_ok = false;

/* ---- 以下仅内部任务/事件回调访问 ---- */

/** 内部任务句柄（任务退出前自清 NULL，stop() 据此等待收尾） */
static TaskHandle_t s_worker = NULL;

/** stop() 请求标记 */
static volatile bool s_exit = false;

/** 事件组与命令通道 */
static EventGroupHandle_t s_evt = NULL;
static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_cmd_mutex = NULL;
static EventGroupHandle_t s_cmd_done = NULL;
static volatile esp_err_t s_cmd_result = ESP_OK;

/** esp_event 处理器注册句柄（stop 时注销用） */
static esp_event_handler_instance_t s_wifi_event_inst = NULL;
static esp_event_handler_instance_t s_ip_event_inst = NULL;

/** TCP/IP 协议栈是否已初始化（esp_netif_init() 约定整个应用只调一次） */
static bool s_netif_inited = false;

/** netif 对象（创建一次，stop 后保留复用） */
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

/** 连接进度（仅内部任务维护） */
static bool s_sta_started = false;     /**< STA 接口是否已启动（决定能否直接 connect） */
static bool s_connect_pending = false; /**< 等 STA_START 到来后补发 esp_wifi_connect */
static int s_retry_count = 0;          /**< 本轮已重试次数 */
static bool s_retry_pending = false;   /**< 是否有挂起的退避重试 */
static TickType_t s_retry_deadline = 0; /**< 退避重试到点时刻 */
static int s_ignore_disconnect = 0;    /**< 主动断开产生的 DISCONNECTED 事件要吞掉的个数 */

/** 最近一次断线原因码（事件回调写、内部任务读，仅用于日志） */
static volatile uint8_t s_last_disconnect_reason = 0;

/* ============================================================
 * NVS 凭据读写（只允许在 start() 与内部任务上下文调用）
 * ============================================================ */

/**
 * @brief 确保 NVS 可用（幂等；不可用时只降级、不擦分区）
 *
 * 正常情况下 services/settings 的初始化已把 NVS 拉起来；本函数兜底
 * 处理“wifi_manager 单独使用”的场景。刻意不做 settings.c 那种
 * “擦除重建”恢复——NVS 分区里还有别人的数据（xt60cfg 等），
 * 本组件无权为了自己的一个命名空间把整盘清掉。
 *
 * @return ESP_OK NVS 可用（含命名空间尚未创建的情况）
 *         其他值：NVS 不可用，后续凭据读写降级为内存态
 */
static esp_err_t nvs_ensure_ready(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_OK) {
        nvs_close(handle);
        s_nvs_ok = true;
        return ESP_OK;
    }

    /* 命名空间不存在 ≠ NVS 坏了：说明 NVS 已初始化、只是还没配过网 */
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_nvs_ok = true;
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "NVS 打开失败: %s，凭据将无法持久化", esp_err_to_name(err));
        s_nvs_ok = false;
        return err;
    }

    /* NVS 尚未初始化：只做 init，失败也不擦分区（理由见函数注释） */
    err = nvs_flash_init();
    s_nvs_ok = (err == ESP_OK);
    if (!s_nvs_ok) {
        ESP_LOGW(TAG, "nvs_flash_init 失败: %s，凭据将无法持久化", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 从 NVS 读取凭据
 *
 * @param ssid 热点名输出缓冲
 * @param ssid_len 热点名缓冲长度
 * @param pass 密码输出缓冲
 * @param pass_len 密码缓冲长度
 * @return ESP_OK 读到凭据
 *         ESP_ERR_NVS_NOT_FOUND 尚未配网（或命名空间不存在）
 *         其他值：读取出错（按未配网处理即可）
 */
static esp_err_t nvs_load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = ssid_len;
    err = nvs_get_str(handle, WIFI_MANAGER_NVS_KEY_SSID, ssid, &len);
    if (err == ESP_OK) {
        /* 密码键可缺省：开放热点没有密码 */
        len = pass_len;
        esp_err_t pass_err = nvs_get_str(handle, WIFI_MANAGER_NVS_KEY_PASS, pass, &len);
        if (pass_err == ESP_ERR_NVS_NOT_FOUND) {
            pass[0] = '\0';
        } else if (pass_err != ESP_OK) {
            err = pass_err;
        }
    }

    nvs_close(handle);
    return err;
}

/**
 * @brief 把凭据写入 NVS（仅内部任务调用）
 *
 * @param ssid 热点名，不可为 NULL
 * @param pass 密码，不可为 NULL（开放热点传空串）
 * @return ESP_OK 写入成功
 */
static esp_err_t nvs_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, WIFI_MANAGER_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_MANAGER_NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        /* nvs_commit：把缓冲的写操作真正落到 Flash，缺它等于没写 */
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

/**
 * @brief 擦除 NVS 中的凭据（仅内部任务调用）
 *
 * 只擦本命名空间的两个键，不用 nvs_erase_all——后者会波及同命名空间
 * 未来可能出现的其他键，语义过宽。
 *
 * @return ESP_OK 擦除成功（键不存在也算成功）
 */
static esp_err_t nvs_erase_creds(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t ssid_err = nvs_erase_key(handle, WIFI_MANAGER_NVS_KEY_SSID);
    esp_err_t pass_err = nvs_erase_key(handle, WIFI_MANAGER_NVS_KEY_PASS);
    if (ssid_err != ESP_OK && ssid_err != ESP_ERR_NVS_NOT_FOUND) {
        err = ssid_err;
    } else if (pass_err != ESP_OK && pass_err != ESP_ERR_NVS_NOT_FOUND) {
        err = pass_err;
    } else {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

/* ============================================================
 * NVS 工作模式偏好读写（只允许在 start() 与内部任务上下文调用）
 * ============================================================ */

/**
 * @brief 从 NVS 读取工作模式偏好（v0.6.0）
 *
 * 键不存在（新设备 / 恢复出厂后）返回 ESP_ERR_NVS_NOT_FOUND，
 * 调用方按 WIFI_MANAGER_MODE_AUTO 处理即可。
 *
 * @param out 输出模式，不可为 NULL
 * @return ESP_OK 读到；ESP_ERR_NVS_NOT_FOUND 尚未设置过；其他值读取失败
 */
static esp_err_t nvs_load_mode(wifi_manager_mode_t *out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t v = (uint8_t)WIFI_MANAGER_MODE_AUTO;
    err = nvs_get_u8(handle, WIFI_MANAGER_NVS_KEY_MODE, &v);
    if (err == ESP_OK && v > (uint8_t)WIFI_MANAGER_MODE_AP) {
        /* 越界值（旧固件写入/数据损坏）：按自动模式处理，不信任脏数据 */
        ESP_LOGW(TAG, "NVS 中的 WiFi 模式值异常(%u)，按自动模式处理", (unsigned)v);
        v = (uint8_t)WIFI_MANAGER_MODE_AUTO;
        err = ESP_ERR_NVS_NOT_FOUND;
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        *out = (wifi_manager_mode_t)v;
    }
    return err;
}

/**
 * @brief 把工作模式偏好写入 NVS（仅内部任务调用）
 *
 * @param mode 目标模式
 * @return ESP_OK 写入成功；其他值 NVS 错误（此时只影响掉电保持，当前运行不受影响）
 */
static esp_err_t nvs_save_mode(wifi_manager_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, WIFI_MANAGER_NVS_KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(handle); /* 缺 nvs_commit 等于没写（只改了 RAM 缓冲） */
    }
    nvs_close(handle);
    return err;
}

/* ============================================================
 * 内部小工具
 * ============================================================ */
/**
 * @brief 迁移状态（内部任务调用；经自旋锁保证读方一致）
 */
static void set_state(wifi_manager_state_t state)
{
    portENTER_CRITICAL(&s_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 读状态（不经锁的内部版本，供内部任务自身使用）
 */
static wifi_manager_state_t get_state_raw(void)
{
    wifi_manager_state_t state;
    portENTER_CRITICAL(&s_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

/**
 * @brief 生成配网热点名 "XT60-Meter-XXXX"
 *
 * XXXX 取芯片 MAC 后两字节（十六进制）：同一批设备靠它区分，
 * 又不泄露完整 MAC。esp_read_mac() 读 eFuse 基础 MAC 推导，
 * 在 WiFi 初始化前即可调用。
 *
 * @param out 输出缓冲区
 * @param out_len 输出缓冲区长度（至少 WIFI_MANAGER_AP_SSID_MAX_LEN）
 */
static void build_ap_ssid(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "XT60-Meter-%02X%02X", mac[4], mac[5]);
}

/**
 * @brief 进入 AP 配网模式（内部任务调用）
 *
 * 用 WIFI_MODE_APSTA 而不是纯 AP：保持 STA 接口活着，
 * 配网页才能一边列热点一边工作（扫描必须有 STA 接口）。
 *
 * @return ESP_OK 成功
 */
static esp_err_t enter_ap_mode(void)
{
    build_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    size_t ap_len = strlen(s_ap_ssid);
    memcpy(ap_cfg.ap.ssid, s_ap_ssid, ap_len);
    ap_cfg.ap.ssid_len = (uint8_t)ap_len;
    ap_cfg.ap.channel = WIFI_MANAGER_AP_CHANNEL;
    /* WPA2-PSK 加密（不再用开放热点）：① Windows 对开放网络常静默拒绝连接
     * （2026-10 实测扫描不到/连接即断）；② 开放热点会把配网通道暴露给任何
     * 旁人。！量产应改为设备唯一密码（贴机身），此处为开发期统一密码。 */
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy((char *)ap_cfg.ap.password, WIFI_MANAGER_AP_PASS, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.max_connection = WIFI_MANAGER_AP_MAX_CONN;
    ap_cfg.ap.ssid_hidden = 0;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换 APSTA 模式失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 AP 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    set_state(WIFI_MANAGER_STATE_AP_MODE);
    ESP_LOGI(TAG, "已进入 AP 配网模式（热点 %s，WPA2 密码 %s）", s_ap_ssid,
             WIFI_MANAGER_AP_PASS);
    return ESP_OK;
}

/**
 * @brief 把凭据缓存到内存（经自旋锁）
 */
static void cache_creds(const char *ssid, const char *pass)
{
    portENTER_CRITICAL(&s_lock);
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, pass, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';
    s_have_creds = true;
    portEXIT_CRITICAL(&s_lock);
}

/**
 * @brief 清空内存凭据缓存（经自旋锁）
 */
static void clear_creds(void)
{
    portENTER_CRITICAL(&s_lock);
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_have_creds = false;
    s_has_ip = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    portEXIT_CRITICAL(&s_lock);
}

/* ============================================================
 * esp_event 回调：只“记事件”，动作留给内部任务
 * ============================================================ */

/**
 * @brief WIFI_EVENT 事件回调
 *
 * 注意：回调运行在事件循环任务里，必须短平快——
 * 这里只拷贝几个字节、置事件位，绝不做 NVS 写入或阻塞操作。
 */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (s_evt == NULL) {
        return;
    }

    switch (id) {
    case WIFI_EVENT_STA_START:
        xEventGroupSetBits(s_evt, WM_EVT_STA_START);
        break;

    case WIFI_EVENT_STA_STOP:
        xEventGroupSetBits(s_evt, WM_EVT_STA_STOP);
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        /* 事件数据仅在回调期间有效，原因码先拷出来供日志用 */
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        s_last_disconnect_reason = (ev != NULL) ? ev->reason : 0;
        xEventGroupSetBits(s_evt, WM_EVT_DISCONNECTED);
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        xEventGroupSetBits(s_evt, WM_EVT_SCAN_DONE);
        break;

    default:
        /* AP 侧事件（AP_START / STACONNECTED 等）当前无需处理 */
        break;
    }
}

/**
 * @brief IP_EVENT 事件回调
 *
 * ip_event_got_ip_t 只在回调期间有效，IP 信息必须在这里拷走。
 */
static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (s_evt == NULL) {
        return;
    }

    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        if (ev != NULL) {
            portENTER_CRITICAL(&s_lock);
            s_ip_info = ev->ip_info;
            s_has_ip = true;
            portEXIT_CRITICAL(&s_lock);
        }
        xEventGroupSetBits(s_evt, WM_EVT_GOT_IP);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        portENTER_CRITICAL(&s_lock);
        s_has_ip = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        portEXIT_CRITICAL(&s_lock);
        xEventGroupSetBits(s_evt, WM_EVT_LOST_IP);
    }
}

/* ============================================================
 * 内部任务：状态迁移与退避重试的唯一执行者
 * ============================================================ */

/**
 * @brief 安排一次退避重试；重试耗尽则定格 FAILED
 *
 * 重试是“有限次”的：连续 WIFI_MANAGER_RETRY_MAX 次仍不成功就停手，
 * 不再无限循环、更不重启设备——设备必须保持可测量、可救回。
 */
static void schedule_retry(void)
{
    s_retry_count++;

    if (s_retry_count > WIFI_MANAGER_RETRY_MAX) {
        s_retry_pending = false;
        set_state(WIFI_MANAGER_STATE_FAILED);
        ESP_LOGW(TAG, "连接失败：已重试 %d 次仍不成功（原因码 %u），进入 FAILED 并保持现场",
                 WIFI_MANAGER_RETRY_MAX, (unsigned)s_last_disconnect_reason);
        return;
    }

    uint32_t delay_ms = WIFI_MANAGER_RETRY_BASE_MS << (s_retry_count - 1);
    if (delay_ms > WIFI_MANAGER_RETRY_MAX_MS) {
        delay_ms = WIFI_MANAGER_RETRY_MAX_MS;
    }

    s_retry_pending = true;
    s_retry_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    set_state(WIFI_MANAGER_STATE_CONNECTING);
    ESP_LOGI(TAG, "第 %d/%d 次重试将在 %ums 后发起（原因码 %u）", s_retry_count,
             WIFI_MANAGER_RETRY_MAX, (unsigned)delay_ms, (unsigned)s_last_disconnect_reason);
}

/**
 * @brief 发起一次连接尝试；esp_wifi_connect() 立即失败时转入重试轨道
 *
 * 为什么不“失败就完事”：esp_wifi_connect() 只回一个错误码时不会再来
 * DISCONNECTED 事件，不接上重试轨道就会永远卡在 CONNECTING。
 *
 * @param why 调用场景，仅用于日志定位
 */
static void issue_connect(const char *why)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "发起连接失败（%s）: %s，转入重试", why, esp_err_to_name(err));
        schedule_retry();
    }
}

/**
 * @brief 处理 STA 断线/连接失败事件
 */
static void handle_disconnect(void)
{
    wifi_manager_state_t state = get_state_raw();

    /* AP 配网模式下 STA 不连热点；IDLE/FAILED 也不该被断线事件打扰 */
    if (state == WIFI_MANAGER_STATE_AP_MODE || state == WIFI_MANAGER_STATE_IDLE) {
        return;
    }
    if (state == WIFI_MANAGER_STATE_FAILED) {
        return;
    }

    /* 主动断开（connect 换网 / forget）产生的事件：吞掉，不算失败 */
    if (s_ignore_disconnect > 0) {
        s_ignore_disconnect--;
        ESP_LOGI(TAG, "忽略主动断开事件（剩余 %d 个）", s_ignore_disconnect);
        return;
    }

    schedule_retry();
}

/**
 * @brief 处理拿到 IP 事件
 */
static void handle_got_ip(void)
{
    s_retry_count = 0;
    s_retry_pending = false;
    set_state(WIFI_MANAGER_STATE_CONNECTED);

    portENTER_CRITICAL(&s_lock);
    uint32_t addr = s_ip_info.ip.addr;
    portEXIT_CRITICAL(&s_lock);

    /* esp_netif 的 IPv4 地址是网络字节序（大端）存进 uint32，但主机是小端，
     * 故 uint32 的**最低字节**才是点分第一段——与标准宏 IP2STR 一致
     * （esp_ip4_addr1_16 = addr & 0xff）。此前按最高字节取，显示顺序整个
     * 反了（192.168.1.12 → 12.1.168.192，2026-10 上板实测）。 */
    ESP_LOGI(TAG, "已连接并获取 IP: %u.%u.%u.%u", (unsigned)(addr & 0xff),
             (unsigned)((addr >> 8) & 0xff), (unsigned)((addr >> 16) & 0xff),
             (unsigned)((addr >> 24) & 0xff));
}

/**
 * @brief 处理 IP 丢失事件（链路还在、DHCP 地址没了）
 */
static void handle_lost_ip(void)
{
    if (get_state_raw() != WIFI_MANAGER_STATE_CONNECTED) {
        return;
    }
    /* 回到 CONNECTING：链路未断，等待重新拿到 IP 即可回到 CONNECTED */
    set_state(WIFI_MANAGER_STATE_CONNECTING);
    ESP_LOGW(TAG, "IP 丢失，等待重新获取");
}

/**
 * @brief 取回扫描结果并缓存（回调只报“扫完了”，取数在任务里做）
 */
static void handle_scan_done(void)
{
    uint16_t count = WIFI_MANAGER_SCAN_MAX_AP;
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_scan_scratch);
    s_scan_busy = false; /* 无论取数成功与否，扫描都结束了（UI 轮询据此收尾） */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取扫描结果失败: %s", esp_err_to_name(err));
        return;
    }

    /* 先取到暂存区、再整块拷进缓存：读者持锁拷贝时不会看到半成品 */
    portENTER_CRITICAL(&s_lock);
    s_scan_count = count;
    if (count > 0) {
        memcpy(s_scan_list, s_scan_scratch, (size_t)count * sizeof(wifi_ap_record_t));
    }
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "扫描完成，共 %u 个热点", (unsigned)count);
}

/**
 * @brief 切到 STA 模式并用给定凭据发起连接（handle_cmd_connect / handle_cmd_set_mode 共用）
 *
 * 只做"切模式 + 下发 STA 配置 + 发起连接"，不含 NVS 写入与凭据缓存
 * （那两件事由各自的调用方负责，避免职责重叠）。
 *
 * @param ssid 热点名（调用方已保证非空且长度合法）
 * @param pass 密码（空串 = 开放热点）
 * @return ESP_OK 已发起连接（能否连上由状态机后续推进）；其他值 esp_wifi 错误码
 */
static esp_err_t start_sta_connect(const char *ssid, const char *pass)
{
    /* 若已有 STA 活动（已连接/连接中），先断开再连新热点。
     * esp_wifi_connect() 在已连接状态下会失败，必须先断。
     * 这次主动断开产生的 DISCONNECTED 事件用 s_ignore_disconnect 吞掉，
     * 免得被重试逻辑误当成"连接失败"多计一次数。 */
    wifi_manager_state_t state = get_state_raw();
    if (state == WIFI_MANAGER_STATE_CONNECTED || state == WIFI_MANAGER_STATE_CONNECTING) {
        if (esp_wifi_disconnect() == ESP_OK) {
            s_ignore_disconnect++;
        }
    }

    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    memcpy(sta_cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(sta_cfg.sta.password, pass, strlen(pass));

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换 STA 模式失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 STA 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 发起连接（新一轮，重试计数清零） */
    s_retry_count = 0;
    s_retry_pending = false;
    set_state(WIFI_MANAGER_STATE_CONNECTING);

    if (s_sta_started) {
        s_connect_pending = false;
        issue_connect("命令下发");
    } else {
        /* STA 接口尚未启动（极端时序）：挂起，等 STA_START 补发 */
        s_connect_pending = true;
    }
    return ESP_OK;
}

/**
 * @brief 命令处理：保存凭据并切 STA 连接
 *
 * 所有 NVS 写入都发生在本函数（= 内部任务上下文），见文件头说明。
 */
static esp_err_t handle_cmd_connect(const wifi_manager_cmd_t *cmd)
{
    /* 1. 持久化凭据（NVS 不可用/写失败都只降级，不阻断本次连接） */
    esp_err_t save_err = ESP_OK;
    if (s_nvs_ok) {
        save_err = nvs_save_creds(cmd->ssid, cmd->pass);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "凭据保存失败: %s（本次仍尝试连接，但掉电后会丢失）",
                     esp_err_to_name(save_err));
        }
    } else {
        ESP_LOGW(TAG, "NVS 不可用，凭据仅存内存（本次会话有效）");
    }

    cache_creds(cmd->ssid, cmd->pass);

    /* 2. 切 STA 并连接（NVS/缓存已就绪，余下动作与模式切换路径共用） */
    esp_err_t err = start_sta_connect(cmd->ssid, cmd->pass);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "已发起连接（SSID=%s）", cmd->ssid);

    /* 凭据写失败时把错误码带回给调用方，但连接动作已按要求发起 */
    return save_err;
}

/**
 * @brief 命令处理：设置工作模式偏好（v0.6.0）
 *
 * 顺序：写 NVS（失败只降级）→ 更新内存快照 → 按目标模式落地。
 * 落地动作与 start() 的"进入 STA 或 AP"分支同语义，但**不能复用** start()
 * （它含 netif/驱动初始化，这里驱动已在运行）。
 *
 * @param mode_raw 目标模式（已由 set_mode 校验过范围，这里再兜一次底）
 * @return ESP_OK 或 esp_wifi 错误码；NVS 写失败也返回 ESP_OK（仅掉电不保留）
 */
static esp_err_t handle_cmd_set_mode(uint8_t mode_raw)
{
    if (mode_raw > (uint8_t)WIFI_MANAGER_MODE_AP) {
        return ESP_ERR_INVALID_ARG;
    }
    const wifi_manager_mode_t mode = (wifi_manager_mode_t)mode_raw;

    /* 1. 持久化偏好（失败不阻断本次切换，只影响掉电后的默认值） */
    esp_err_t nvs_err = ESP_OK;
    if (s_nvs_ok) {
        nvs_err = nvs_save_mode(mode);
        if (nvs_err != ESP_OK) {
            ESP_LOGW(TAG, "模式偏好保存失败: %s（本次切换仍生效，掉电后回到旧模式）",
                     esp_err_to_name(nvs_err));
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_mode = mode;
    portEXIT_CRITICAL(&s_lock);

    /* 2. 取消一切挂起的连接动作：切模式＝重新决定"怎么工作" */
    s_retry_count = 0;
    s_retry_pending = false;
    s_connect_pending = false;

    /* 3. 取一份凭据快照（s_ssid/s_pass 受自旋锁保护，不能直接传出去用） */
    char ssid[WIFI_MANAGER_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_MANAGER_PASS_MAX_LEN] = {0};
    bool have_creds = false;
    portENTER_CRITICAL(&s_lock);
    have_creds = s_have_creds;
    if (have_creds) {
        memcpy(ssid, s_ssid, sizeof(ssid));
        memcpy(pass, s_pass, sizeof(pass));
    }
    portEXIT_CRITICAL(&s_lock);

    /* 4. 落地 */
    if (mode == WIFI_MANAGER_MODE_AP) {
        /* 强制配网热点：先断开 STA（吞掉 DISCONNECTED），再进 AP 模式 */
        wifi_manager_state_t st = get_state_raw();
        if (st == WIFI_MANAGER_STATE_CONNECTED || st == WIFI_MANAGER_STATE_CONNECTING) {
            if (esp_wifi_disconnect() == ESP_OK) {
                s_ignore_disconnect++;
            }
        }
        esp_err_t err = enter_ap_mode();
        ESP_LOGI(TAG, "WiFi 模式 → 强制配网热点（%s）", err == ESP_OK ? "已切换" : "切换失败");
        return err;
    }

    if (!have_creds) {
        if (mode == WIFI_MANAGER_MODE_STA) {
            /* 仅 STA 且无凭据：关掉配网热点，停在 FAILED 等配网下发。
             * 此时设备不可通过热点访问，UI 会提示"请先配网"。 */
            esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
            set_state(WIFI_MANAGER_STATE_FAILED);
            ESP_LOGW(TAG, "WiFi 模式 → 仅 STA，但尚无凭据：已关闭配网热点，等待配网下发");
            return err;
        }
        /* AUTO 且无凭据：与既有行为一致，回到配网热点 */
        esp_err_t err = enter_ap_mode();
        ESP_LOGI(TAG, "WiFi 模式 → 自动（无凭据，进入配网热点）");
        return err;
    }

    /* 有凭据的 STA / AUTO：直接连路由器 */
    esp_err_t err = start_sta_connect(ssid, pass);
    ESP_LOGI(TAG, "WiFi 模式 → %s（有凭据，发起连接 SSID=%s）",
             (mode == WIFI_MANAGER_MODE_STA) ? "仅 STA" : "自动", ssid);
    return err;
}

/**
 * @brief 命令处理：清凭据并回 AP 配网模式
 */
static esp_err_t handle_cmd_forget(void)
{
    /* 1. 擦凭据（失败也继续回 AP，但把错误码带回——否则下次上电会偷偷重连） */
    esp_err_t erase_err = ESP_OK;
    if (s_nvs_ok) {
        erase_err = nvs_erase_creds();
        if (erase_err != ESP_OK) {
            ESP_LOGW(TAG, "凭据擦除失败: %s", esp_err_to_name(erase_err));
        }
    }

    /* 2. 清内存态、取消一切挂起的连接动作 */
    clear_creds();
    s_retry_count = 0;
    s_retry_pending = false;
    s_connect_pending = false;

    wifi_manager_state_t state = get_state_raw();
    if (state == WIFI_MANAGER_STATE_CONNECTED || state == WIFI_MANAGER_STATE_CONNECTING) {
        if (esp_wifi_disconnect() == ESP_OK) {
            s_ignore_disconnect++;
        }
    }

    /* 3. 按模式偏好决定"忘记之后去哪"（v0.6.0）：
     *   · 自动 / 强制热点 → 回配网热点（旧行为）；
     *   · 仅 STA          → 不起热点，停在 FAILED 等配网下发，
     *                       否则会违背用户"只做 STA 客户端"的选择。 */
    portENTER_CRITICAL(&s_lock);
    wifi_manager_mode_t mode = s_mode;
    portEXIT_CRITICAL(&s_lock);
    if (mode == WIFI_MANAGER_MODE_STA) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        set_state(WIFI_MANAGER_STATE_FAILED);
        ESP_LOGW(TAG, "凭据已清除；模式偏好=仅 STA，保持 STA 等待重新配网");
        return (err == ESP_OK) ? erase_err : err;
    }

    esp_err_t err = enter_ap_mode();
    if (err != ESP_OK) {
        return err;
    }

    return erase_err;
}

/**
 * @brief 计算下一次等待时长（有退避重试挂起时等到点，否则一直等事件）
 *
 * 用 TickType_t 回绕安全的减法判断剩余时间；差值异常大只可能是
 * 状态不一致，此时按“已到点”处理，宁可早醒不能睡死。
 */
static TickType_t compute_wait_ticks(void)
{
    if (!s_retry_pending) {
        return portMAX_DELAY;
    }

    TickType_t left = s_retry_deadline - xTaskGetTickCount();
    if (left > pdMS_TO_TICKS(WIFI_MANAGER_RETRY_MAX_MS) + pdMS_TO_TICKS(1000)) {
        return 0;
    }
    return left;
}

/**
 * @brief 内部任务主体：事件驱动的状态机循环
 *
 * 唤醒条件三选一：事件位、命令到达、退避重试到点（等待超时）。
 * 循环内不做无超时的阻塞等待，保证 stop() 能及时把它叫停。
 */
static void wifi_manager_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "内部任务已启动");

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_evt, WM_EVT_ALL, pdTRUE, pdFALSE,
                                               compute_wait_ticks());

        if (((bits & WM_EVT_EXIT) != 0) || s_exit) {
            break;
        }

        /* ---- 1. 外部命令（NVS 写与模式切换都发生在这里） ---- */
        wifi_manager_cmd_t cmd;
        while ((s_cmd_queue != NULL) && (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE)) {
            esp_err_t result;
            if (cmd.type == WIFI_MANAGER_CMD_CONNECT) {
                result = handle_cmd_connect(&cmd);
            } else if (cmd.type == WIFI_MANAGER_CMD_FORGET) {
                result = handle_cmd_forget();
            } else if (cmd.type == WIFI_MANAGER_CMD_SET_MODE) {
                result = handle_cmd_set_mode(cmd.mode);
            } else {
                result = ESP_ERR_INVALID_ARG;
            }
            s_cmd_result = result;
            if (s_cmd_done != NULL) {
                xEventGroupSetBits(s_cmd_done, WM_CMD_DONE);
            }
        }

        /* ---- 2. WiFi 事件 ---- */
        if ((bits & WM_EVT_STA_START) != 0) {
            s_sta_started = true;
            if (s_connect_pending) {
                s_connect_pending = false;
                issue_connect("STA 启动补发");
            }
        }
        if ((bits & WM_EVT_STA_STOP) != 0) {
            s_sta_started = false;
        }
        if ((bits & WM_EVT_DISCONNECTED) != 0) {
            handle_disconnect();
        }
        if ((bits & WM_EVT_GOT_IP) != 0) {
            handle_got_ip();
        }
        if ((bits & WM_EVT_LOST_IP) != 0) {
            handle_lost_ip();
        }
        if ((bits & WM_EVT_SCAN_DONE) != 0) {
            handle_scan_done();
        }

        /* ---- 3. 退避重试到点（等待超时且无新事件） ---- */
        if ((bits == 0) && s_retry_pending) {
            s_retry_pending = false;
            issue_connect("退避重试");
        }
    }

    ESP_LOGI(TAG, "内部任务退出");
    s_worker = NULL;
    vTaskDelete(NULL);
}

/* ============================================================
 * 同步对象 / 事件注册的创建与释放
 * ============================================================ */

/**
 * @brief 创建事件组、命令队列与互斥锁（失败时全部回滚）
 */
static esp_err_t create_sync_objects(void)
{
    s_evt = xEventGroupCreate();
    s_cmd_queue = xQueueCreate(WIFI_MANAGER_CMD_QUEUE_LEN, sizeof(wifi_manager_cmd_t));
    s_cmd_mutex = xSemaphoreCreateMutex();
    s_cmd_done = xEventGroupCreate();

    if ((s_evt == NULL) || (s_cmd_queue == NULL) || (s_cmd_mutex == NULL) || (s_cmd_done == NULL)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief 释放同步对象（内部任务已退出后才可调用）
 */
static void destroy_sync_objects(void)
{
    if (s_cmd_queue != NULL) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    if (s_cmd_done != NULL) {
        vEventGroupDelete(s_cmd_done);
        s_cmd_done = NULL;
    }
    if (s_cmd_mutex != NULL) {
        vSemaphoreDelete(s_cmd_mutex);
        s_cmd_mutex = NULL;
    }
    if (s_evt != NULL) {
        vEventGroupDelete(s_evt);
        s_evt = NULL;
    }
}

/**
 * @brief 注销 esp_event 处理器（幂等）
 */
static void unregister_handlers(void)
{
    if (s_wifi_event_inst != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    s_wifi_event_inst);
        s_wifi_event_inst = NULL;
    }
    if (s_ip_event_inst != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_event_inst);
        s_ip_event_inst = NULL;
    }
}

/**
 * @brief 收尾：摘事件 → 停内部任务 → 反初始化 WiFi 驱动 → 释放同步对象
 *
 * 顺序不能乱：先注销处理器，teardown 期间就不会再有回调去碰
 * 马上要销毁的事件组；再等内部任务退出，避免它用已销毁的对象。
 * 等待有上限（2s），绝不永久阻塞调用方。
 */
static void stop_internal(void)
{
    unregister_handlers();

    s_exit = true;
    if (s_evt != NULL) {
        xEventGroupSetBits(s_evt, WM_EVT_EXIT);
    }
    for (int i = 0; (i < 40) && (s_worker != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_worker != NULL) {
        ESP_LOGW(TAG, "内部任务未在 2s 内退出，继续收尾");
    }

    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();

    destroy_sync_objects();

    clear_creds();
    portENTER_CRITICAL(&s_lock);
    s_scan_count = 0;
    s_state = WIFI_MANAGER_STATE_IDLE;
    portEXIT_CRITICAL(&s_lock);

    s_retry_count = 0;
    s_retry_pending = false;
    s_connect_pending = false;
    s_sta_started = false;
    s_ignore_disconnect = 0;
    s_exit = false;
}

/* ============================================================
 * 公开 API
 * ============================================================ */

/**
 * @brief 启动 WiFi 管理（读取 NVS 凭据，按需进入 STA 或 AP 模式）
 */
esp_err_t wifi_manager_start(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "重复启动：组件已在运行（state=%d）", (int)get_state_raw());
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 1. 读凭据（NVS 不可用只降级，见 nvs_ensure_ready） ---- */
    (void)nvs_ensure_ready();

    char ssid[WIFI_MANAGER_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_MANAGER_PASS_MAX_LEN] = {0};
    bool have_creds = false;

    if (s_nvs_ok) {
        esp_err_t err = nvs_load_creds(ssid, sizeof(ssid), pass, sizeof(pass));
        if (err == ESP_OK) {
            have_creds = true;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "读取凭据失败: %s，按未配网处理", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "NVS 不可用：凭据无法持久化，按未配网进入 AP 模式");
    }

    /* ---- 2. TCP/IP 协议栈与默认事件循环 ----
     * esp_netif_init() 约定“整个应用只调一次”，这里用标志兜底；
     * 默认事件循环若已被别的组件建好（ESP_ERR_INVALID_STATE），
     * 直接共用即可——注销时只摘自己的处理器，不拆别人的循环。 */
    if (!s_netif_inited) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
            return err;
        }
        s_netif_inited = true;
    }

    esp_err_t err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 3. netif 对象（创建一次，stop 后保留复用） ---- */
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "创建 STA netif 失败");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "创建 AP netif 失败");
            return ESP_ERR_NO_MEM;
        }
    }

    /* ---- 4. WiFi 驱动 ---- */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    /* 凭据由本组件自管到 NVS "wifi"，禁止 esp_wifi 再落一份 */
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /* ---- 5. 注册事件处理器 ---- */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, &s_wifi_event_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 WIFI_EVENT 处理器失败: %s", esp_err_to_name(err));
        (void)esp_wifi_deinit();
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL,
                                              &s_ip_event_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 IP_EVENT 处理器失败: %s", esp_err_to_name(err));
        unregister_handlers();
        (void)esp_wifi_deinit();
        return err;
    }

    /* ---- 6. 同步对象与内部任务 ---- */
    err = create_sync_objects();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建同步对象失败: %s", esp_err_to_name(err));
        unregister_handlers();
        destroy_sync_objects();
        (void)esp_wifi_deinit();
        return err;
    }

    if (xTaskCreate(wifi_manager_task, "wifi_mgr", WIFI_MANAGER_TASK_STACK, NULL,
                    WIFI_MANAGER_TASK_PRIO, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "创建内部任务失败");
        unregister_handlers();
        destroy_sync_objects();
        (void)esp_wifi_deinit();
        return ESP_ERR_NO_MEM;
    }

    /* ---- 7. 按「模式偏好 + 是否有凭据」进入 STA 或 AP（v0.6.0 扩展）----
     * 决策表：
     *   AUTO + 有凭据 → STA 连接（与旧行为一致）
     *   AUTO + 无凭据 → 配网热点（与旧行为一致）
     *   STA  + 有凭据 → STA 连接
     *   STA  + 无凭据 → 只起 STA 接口（不起热点），等配网下发
     *   AP            → 强制配网热点（即使有凭据也不连）
     * s_started 必须在 esp_wifi_start() 之前置位：
     * STA_START 事件会立刻到来，内部任务/命令通道要能工作。 */
    s_started = true;

    wifi_manager_mode_t mode = WIFI_MANAGER_MODE_AUTO;
    if (s_nvs_ok) {
        esp_err_t merr = nvs_load_mode(&mode);
        if (merr != ESP_OK && merr != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "读取模式偏好失败: %s，按自动模式处理", esp_err_to_name(merr));
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_mode = mode;
    portEXIT_CRITICAL(&s_lock);

    /* want_ap：只有"强制 AP"或"自动且无凭据"才起配网热点；
     * 仅 STA 模式无凭据时刻意不起热点（用户明确要求只做 STA 客户端）。 */
    const bool want_ap =
        (mode == WIFI_MANAGER_MODE_AP) || ((mode == WIFI_MANAGER_MODE_AUTO) && !have_creds);

    if (!want_ap && have_creds) {
        cache_creds(ssid, pass);

        wifi_config_t sta_cfg;
        memset(&sta_cfg, 0, sizeof(sta_cfg));
        memcpy(sta_cfg.sta.ssid, ssid, strlen(ssid));
        memcpy(sta_cfg.sta.password, pass, strlen(pass));

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "配置 STA 失败: %s", esp_err_to_name(err));
            stop_internal();
            s_started = false;
            return err;
        }

        /* 等 STA_START 事件到来后由内部任务补发 esp_wifi_connect */
        s_connect_pending = true;
        set_state(WIFI_MANAGER_STATE_CONNECTING);
        ESP_LOGI(TAG, "已找到凭据（SSID=%s），以 STA 模式开始连接（模式偏好=%d）", ssid, (int)mode);
    } else if (!want_ap) {
        /* 仅 STA 且无凭据：起 STA 接口但无人可连，停在 FAILED 等配网下发 */
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "切换到仅 STA 模式失败: %s", esp_err_to_name(err));
            stop_internal();
            s_started = false;
            return err;
        }
        set_state(WIFI_MANAGER_STATE_FAILED);
        ESP_LOGW(TAG, "模式偏好=仅 STA，但尚无凭据：不启动配网热点，等待配网下发");
    } else {
        err = enter_ap_mode();
        if (err != ESP_OK) {
            stop_internal();
            s_started = false;
            return err;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
        stop_internal();
        s_started = false;
        return err;
    }

    return ESP_OK;
}

/**
 * @brief 停止 WiFi 并释放相关资源
 */
esp_err_t wifi_manager_stop(void)
{
    if (!s_started) {
        ESP_LOGW(TAG, "尚未启动，无可停止");
        return ESP_ERR_INVALID_STATE;
    }

    s_started = false;
    stop_internal();
    ESP_LOGI(TAG, "已停止");
    return ESP_OK;
}

/**
 * @brief 获取当前 WiFi 状态
 */
wifi_manager_state_t wifi_manager_get_state(void)
{
    return get_state_raw();
}

/**
 * @brief 保存凭据并尝试以 STA 模式连接
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *pass)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((ssid == NULL) || (s_cmd_queue == NULL) || (s_cmd_mutex == NULL) || (s_cmd_done == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strnlen(ssid, WIFI_MANAGER_SSID_MAX_LEN + 1);
    if ((ssid_len == 0) || (ssid_len > WIFI_MANAGER_SSID_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t pass_len = (pass != NULL) ? strnlen(pass, WIFI_MANAGER_PASS_MAX_LEN) : 0;
    if (pass_len >= WIFI_MANAGER_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 多个调用方（如 web_console 的多个请求）串行进入命令通道 */
    if (xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(WIFI_MANAGER_CMD_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_manager_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = WIFI_MANAGER_CMD_CONNECT;
    memcpy(cmd.ssid, ssid, ssid_len);
    if (pass_len > 0) {
        memcpy(cmd.pass, pass, pass_len);
    }

    xEventGroupClearBits(s_cmd_done, WM_CMD_DONE);
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_TIMEOUT;
    }
    xEventGroupSetBits(s_evt, WM_EVT_CMD);

    /* 等内部任务把“NVS 写 + 发起连接”做完 */
    EventBits_t bits =
        xEventGroupWaitBits(s_cmd_done, WM_CMD_DONE, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(WIFI_MANAGER_CMD_TIMEOUT_MS));
    esp_err_t result = ((bits & WM_CMD_DONE) != 0) ? s_cmd_result : ESP_ERR_TIMEOUT;

    xSemaphoreGive(s_cmd_mutex);
    return result;
}

/**
 * @brief 忘记网络：清除 NVS 凭据并回到 AP 配网模式
 */
esp_err_t wifi_manager_forget(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((s_cmd_queue == NULL) || (s_cmd_mutex == NULL) || (s_cmd_done == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(WIFI_MANAGER_CMD_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_manager_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = WIFI_MANAGER_CMD_FORGET;

    xEventGroupClearBits(s_cmd_done, WM_CMD_DONE);
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xSemaphoreGive(s_cmd_mutex);
        return ESP_ERR_TIMEOUT;
    }
    xEventGroupSetBits(s_evt, WM_EVT_CMD);

    EventBits_t bits =
        xEventGroupWaitBits(s_cmd_done, WM_CMD_DONE, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(WIFI_MANAGER_CMD_TIMEOUT_MS));
    esp_err_t result = ((bits & WM_CMD_DONE) != 0) ? s_cmd_result : ESP_ERR_TIMEOUT;

    xSemaphoreGive(s_cmd_mutex);
    return result;
}

/**
 * @brief 查询 STA 当前获取到的 IPv4 地址
 */
esp_err_t wifi_manager_get_ip(char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < WIFI_MANAGER_IP_STR_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    bool has_ip = s_has_ip;
    uint32_t addr = s_ip_info.ip.addr;
    portEXIT_CRITICAL(&s_lock);

    if (!s_started || !has_ip) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 低字节在前（网络字节序 + 小端主机，与 IP2STR 同语义）：修复显示反序 */
    snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)(addr & 0xff),
             (unsigned)((addr >> 8) & 0xff), (unsigned)((addr >> 16) & 0xff),
             (unsigned)((addr >> 24) & 0xff));
    return ESP_OK;
}

/**
 * @brief 查询当前凭据中的热点名
 */
esp_err_t wifi_manager_get_ssid(char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < WIFI_MANAGER_SSID_MAX_LEN + 1)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    bool have = s_have_creds;
    if (have) {
        strncpy(out, s_ssid, out_len - 1);
        out[out_len - 1] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);

    return have ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/**
 * @brief 查询配网热点名
 */
esp_err_t wifi_manager_get_ap_ssid(char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < WIFI_MANAGER_AP_SSID_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    strncpy(out, s_ap_ssid, out_len - 1);
    out[out_len - 1] = '\0';
    portEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}

/**
 * @brief 查询配网热点密码（v0.6.2 需求 4：设备端需把密码显示给用户才能连接）
 *
 * 当前热点密码是编译期常量 WIFI_MANAGER_AP_PASS（WPA2-PSK，量产应改设备
 * 唯一码并贴机身）。本函数把它拷给调用方，供设备 UI 在 WiFi 设置页显示
 * "热点名 + 密码"，用户照着输密码即可手机连接热点完成配网。
 * 若将来密码改为运行期可配（例如从 NVS 读取），只需改本函数内部取值来源，
 * 调用方无需改动。
 *
 * @param out 输出缓冲区，不可为 NULL
 * @param out_len 输出缓冲区长度（至少 sizeof(WIFI_MANAGER_AP_PASS)）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 参数非法或缓冲区太小
 */
esp_err_t wifi_manager_get_ap_pass(char *out, size_t out_len)
{
    if ((out == NULL) || (out_len < sizeof(WIFI_MANAGER_AP_PASS))) {
        return ESP_ERR_INVALID_ARG;
    }
    /* sizeof(WIFI_MANAGER_AP_PASS) 含结尾 0，故 out_len 必然 ≥ strlen+1 */
    strncpy(out, WIFI_MANAGER_AP_PASS, out_len - 1);
    out[out_len - 1] = '\0';
    return ESP_OK;
}

/**
 * @brief 触发一次 STA 热点扫描
 */
esp_err_t wifi_manager_scan_start(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    /* config 传 NULL = 默认全信道主动扫描，show_hidden 关闭 */
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "发起扫描失败: %s", esp_err_to_name(err));
    } else {
        s_scan_busy = true; /* 已发起：SCAN_DONE 到来前 UI 轮询会看到"扫描中" */
    }
    return err;
}

/**
 * @brief 查询是否正在扫描（v0.6.0，供配网 UI 轮询）
 *
 * @return true=扫描进行中（结果尚未就绪）
 */
bool wifi_manager_scan_is_running(void)
{
    return s_scan_busy;
}

/**
 * @brief 读取最近一次扫描到的热点列表
 */
esp_err_t wifi_manager_scan_get_ap_list(wifi_ap_record_t *out, uint16_t *inout_count)
{
    if ((out == NULL) || (inout_count == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    uint16_t total = s_scan_count;
    uint16_t copy = (*inout_count < total) ? *inout_count : total;
    if (copy > 0) {
        memcpy(out, s_scan_list, (size_t)copy * sizeof(wifi_ap_record_t));
    }
    portEXIT_CRITICAL(&s_lock);

    *inout_count = copy;
    return (total > 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/**
 * @brief 查询当前的 WiFi 工作模式偏好（v0.6.0）
 */
wifi_manager_mode_t wifi_manager_get_mode(void)
{
    wifi_manager_mode_t mode;
    portENTER_CRITICAL(&s_lock);
    mode = s_mode;
    portEXIT_CRITICAL(&s_lock);
    return mode;
}

/**
 * @brief 设置 WiFi 工作模式偏好（v0.6.0，**非阻塞**）
 *
 * 本函数只做三件事：校验参数 → 更新内存快照 → 把命令排进队列（不等待结果）。
 * 真正的"写 NVS + 切 esp_wifi 模式 + 重连/起热点"由内部任务异步执行
 * （见 handle_cmd_set_mode）。之所以不等结果：调用方是设备 UI 的模式按钮
 * 回调（LVGL 事件回调，运行在持有 LVGL 锁的 UI 主循环里），等待会让界面冻结。
 */
esp_err_t wifi_manager_set_mode(wifi_manager_mode_t mode)
{
    if (mode > WIFI_MANAGER_MODE_AP) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((s_cmd_queue == NULL) || (s_evt == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 内存快照立刻更新：UI 的按钮高亮/状态行下一帧就能读到新值，
     * 不必等内部任务落地（与"乐观更新"同思路，失败时下一次刷新会纠正）。 */
    portENTER_CRITICAL(&s_lock);
    s_mode = mode;
    portEXIT_CRITICAL(&s_lock);

    wifi_manager_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = WIFI_MANAGER_CMD_SET_MODE;
    cmd.mode = (uint8_t)mode;

    /* xQueueSend 超时 0：队列满（极少见）就直接失败返回，绝不阻塞 UI */
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "命令队列已满，模式切换请求被丢弃（模式偏好仍已更新为 %d）", (int)mode);
        return ESP_ERR_TIMEOUT;
    }
    xEventGroupSetBits(s_evt, WM_EVT_CMD);
    return ESP_OK;
}

/**
 * @brief 查询当前 STA 连接的路由器信号强度（v0.6.0）
 *
 * esp_wifi_sta_get_ap_info()：读取"当前关联的 AP"信息（含 rssi）。
 * 未连接时会返回 ESP_ERR_WIFI_NOT_CONNECT，这里统一映射为
 * ESP_ERR_INVALID_STATE，调用方据此显示"无信号"。
 */
esp_err_t wifi_manager_get_rssi(int8_t *out_rssi)
{
    if (out_rssi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (get_state_raw() != WIFI_MANAGER_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE; /* 未连接路由器：无信号可报 */
    }

    wifi_ap_record_t info;
    memset(&info, 0, sizeof(info));
    esp_err_t err = esp_wifi_sta_get_ap_info(&info);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_rssi = info.rssi;
    return ESP_OK;
}
