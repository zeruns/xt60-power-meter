/**
 * @file wifi_manager.h
 * @brief WiFi 连接管理（STA / AP 模式与配网状态机）
 *
 * 说明：ESP-IDF 默认把 WiFi 任务钉在 Core 0，与本工程
 *       “Core 0 负责交互与通信、Core 1 负责 ADS131M02 采样” 的划分一致，
 *       无需修改 CONFIG_WIFI_TASK_CORE_ID。
 *
 * ─────────────────────────────────────────────────────────────
 * 设计取舍（在骨架契约之上补充配网所需的最小接口）
 * ─────────────────────────────────────────────────────────────
 * · 分层：本组件属 middleware，只依赖 ESP-IDF（esp_wifi / esp_netif /
 *   esp_event / nvs），不引用 services 各组件与 event_bus。上层（网页控制台 /
 *   UI）按需查询状态与扫描数据，本组件只做“数据面”，不承载配网页本身。
 * · 凭据持久化：自管 NVS 命名空间 "wifi"（键 ssid / pass），
 *   不触碰 xt60cfg / pm 等其他命名空间；同时把 WiFi 驱动的存储模式设为
 *   WIFI_STORAGE_RAM，避免 esp_wifi 自己再往 NVS 落一份凭据。
 * · 事件驱动：esp_event 回调只“记事件”（拷贝几个字节 + 置标志 + 唤醒
 *   内部任务），状态迁移、退避重试、NVS 写入全部由内部任务完成——
 *   回调里绝不做 NVS 写入与耗时操作，避免拖慢系统事件循环。
 * · 失败策略：STA 连接做有限次指数退避重试，耗尽后停在 FAILED 状态
 *   （不重启设备、不擦凭据），可由 wifi_manager_connect() 重新发起，
 *   或由 wifi_manager_forget() 清凭据回到 AP 配网模式。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h" /* wifi_ap_record_t：扫描结果直接透传，避免二次封装 */

/** WiFi 工作状态 */
typedef enum {
    WIFI_MANAGER_STATE_IDLE = 0,   /**< 未启动 */
    WIFI_MANAGER_STATE_CONNECTING, /**< 正在连接热点 */
    WIFI_MANAGER_STATE_CONNECTED,  /**< 已连接并获取到 IP */
    WIFI_MANAGER_STATE_AP_MODE,    /**< 工作在配网热点模式 */
    WIFI_MANAGER_STATE_FAILED,     /**< 连接失败 */
} wifi_manager_state_t;

/** 热点名（SSID）长度上限，与 esp_wifi 的 wifi_sta_config_t.ssid 字段宽度一致 */
#define WIFI_MANAGER_SSID_MAX_LEN 32

/** 密码缓冲区长度（WPA2-PSK 密码最长 63 字符 + 结尾 0，与 wifi_sta_config_t.password 对应） */
#define WIFI_MANAGER_PASS_MAX_LEN 64

/** 配网热点名缓冲区长度（热点名为 "XT60-Meter-XXXX" 共 15 字符 + 结尾 0） */
#define WIFI_MANAGER_AP_SSID_MAX_LEN 24

/**
 * @brief WiFi 工作模式偏好（v0.6.0 新增）
 *
 * 与 wifi_manager_state_t 的区别：state 是"当前实际在做什么"（实时状态），
 * mode 是"用户希望设备怎么工作"（偏好设置，掉电保留）。二者配合使用——
 * 例如偏好 STA 但尚未配网时，state 会是 FAILED（等配网），mode 仍是 STA。
 */
typedef enum {
    WIFI_MANAGER_MODE_AUTO = 0, /**< 自动（默认）：有凭据走 STA，无凭据起配网热点 */
    WIFI_MANAGER_MODE_STA,      /**< 仅 STA：不启动配网热点（无凭据时只等配网下发） */
    WIFI_MANAGER_MODE_AP,       /**< 强制配网热点：即使已有凭据也不连接路由器 */
} wifi_manager_mode_t;

/**
 * @brief 启动 WiFi 管理（读取 NVS 凭据，按需进入 STA 或 AP 模式）
 *
 * 行为：
 *   · NVS 中有凭据 → STA 模式自动连接（有限次指数退避重试，
 *     重试耗尽进入 FAILED 状态，不重启设备）；
 *   · 无凭据 → AP 配网模式（WPA2-PSK 加密热点 "XT60-Meter-XXXX"，XXXX 为
 *     芯片 MAC 后两字节，密码见 wifi_manager_get_ap_pass()；APSTA 模式，
 *     STA 保持可扫描供配网页列出热点）。
 *
 * 本函数只负责“把 WiFi 拉起来并发起动作”，不等待连接结果——
 * 连接进度由 wifi_manager_get_state() 异步查询。
 *
 * @return ESP_OK 成功（已开始连接或已进入配网模式）
 *         ESP_ERR_INVALID_STATE 重复调用（组件已启动）
 *         ESP_ERR_NO_MEM 内部任务/同步对象创建失败
 *         其他值：esp_netif / esp_wifi 初始化失败的错误码
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief 停止 WiFi 并释放相关资源
 *
 * 注销事件处理器、退出内部任务、停止并反初始化 WiFi 驱动；
 * 默认事件循环与 netif 对象保留（可被下次 wifi_manager_start() 复用）。
 * 与 wifi_manager_connect() / wifi_manager_forget() 不可并发调用。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚未启动
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief 获取当前 WiFi 状态
 * @return 当前状态
 */
wifi_manager_state_t wifi_manager_get_state(void);

/**
 * @brief 保存凭据并尝试以 STA 模式连接（配网成功后“切回 STA”的入口）
 *
 * 凭据写入 NVS 命名空间 "wifi"，随后切换到 STA 模式发起连接；
 * 若当前正在连接或已连接其他热点，会先断开再连新热点。
 * 内部由专用任务完成实际动作，本函数等待其完成，因此会阻塞调用方
 * 最长约数秒（超时返回 ESP_ERR_TIMEOUT）。
 *
 * @param ssid 热点名（1 ~ 32 字符），不可为 NULL
 * @param pass 密码（0 ~ 63 字符），NULL 或空串表示开放热点
 * @return ESP_OK 凭据已保存且已发起连接（是否连上由状态机后续推进）
 *         ESP_ERR_INVALID_STATE 组件未启动
 *         ESP_ERR_INVALID_ARG 参数非法
 *         ESP_ERR_TIMEOUT 等待内部任务超时
 *         其他值：NVS 写入失败的错误码（此时连接仍已发起，但凭据掉电即丢）
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *pass);

/**
 * @brief 忘记网络：清除 NVS 凭据并回到 AP 配网模式
 *
 * 与 wifi_manager_connect() 相同，本函数会等待内部任务完成。
 *
 * @return ESP_OK 凭据已清除且已回到 AP 模式
 *         ESP_ERR_INVALID_STATE 组件未启动
 *         ESP_ERR_TIMEOUT 等待内部任务超时
 *         其他值：NVS 擦除失败的错误码（此时仍会回到 AP 模式，
 *                 但凭据未真正清除，下次上电会重连）
 */
esp_err_t wifi_manager_forget(void);

/**
 * @brief 查询 STA 当前获取到的 IPv4 地址（点分十进制字符串）
 *
 * @param out 输出缓冲区，不可为 NULL
 * @param out_len 输出缓冲区长度（至少 16 字节才能装下 "255.255.255.255"）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚未连接 / 尚未获取到 IP
 *         ESP_ERR_INVALID_ARG 参数非法或缓冲区太小
 */
esp_err_t wifi_manager_get_ip(char *out, size_t out_len);

/**
 * @brief 查询当前凭据中的热点名（已连接或正在连接的目标）
 *
 * @param out 输出缓冲区，不可为 NULL
 * @param out_len 输出缓冲区长度（至少 WIFI_MANAGER_SSID_MAX_LEN + 1）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚无凭据（未配网）
 *         ESP_ERR_INVALID_ARG 参数非法或缓冲区太小
 */
esp_err_t wifi_manager_get_ssid(char *out, size_t out_len);

/**
 * @brief 查询配网热点名（AP 模式下手机/电脑要连的那个热点）
 *
 * @param out 输出缓冲区，不可为 NULL
 * @param out_len 输出缓冲区长度（至少 WIFI_MANAGER_AP_SSID_MAX_LEN）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 组件未启动
 *         ESP_ERR_INVALID_ARG 参数非法或缓冲区太小
 */
esp_err_t wifi_manager_get_ap_ssid(char *out, size_t out_len);

/**
 * @brief 查询配网热点密码（v0.6.2 需求 4：设备端把密码显示给用户才能连接）
 *
 * 当前实现返回编译期常量 WIFI_MANAGER_AP_PASS（WPA2-PSK）。设备 UI 的
 * WiFi 设置页在配网模式下显示"热点名 + 密码"，用户照此输入即可连接热点。
 *
 * @param out 输出缓冲区，不可为 NULL
 * @param out_len 输出缓冲区长度（至少 sizeof(WIFI_MANAGER_AP_PASS)，约 12 字节）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 参数非法或缓冲区太小
 */
esp_err_t wifi_manager_get_ap_pass(char *out, size_t out_len);

/**
 * @brief 触发一次 STA 热点扫描（AP 配网模式下同样可用，供配网页列热点）
 *
 * 异步接口：立即返回，结果由 WIFI_EVENT_SCAN_DONE 完成后缓存，
 * 用 wifi_manager_scan_get_ap_list() 读取。上一次扫描的结果在
 * 本次扫描完成前仍可读到。注意：若 STA 正在连接热点，扫描会返回
 * ESP_ERR_WIFI_STATE（esp_wifi 的限制），请在连接空闲时发起。
 *
 * @return ESP_OK 扫描已发起
 *         ESP_ERR_INVALID_STATE 组件未启动
 *         其他值：esp_wifi_scan_start() 的错误码
 */
esp_err_t wifi_manager_scan_start(void);

/**
 * @brief 查询是否正在扫描（v0.6.0 新增）
 *
 * esp_wifi 没有公开的"扫描是否进行中"接口，本组件按 scan_start()/
 * SCAN_DONE 自行记账。配网 UI 用它轮询"扫完了没有"（扫完即可取列表）。
 *
 * @return true=扫描进行中
 */
bool wifi_manager_scan_is_running(void);

/**
 * @brief 读取最近一次扫描到的热点列表（按信号强度降序，取前若干个）
 *
 * @param out 输出数组（元素为 wifi_ap_record_t），由调用方提供，不可为 NULL
 * @param inout_count 入参 = 数组容量；出参 = 实际写入条数，不可为 NULL
 * @return ESP_OK 成功（出参条数可能小于容量）
 *         ESP_ERR_INVALID_STATE 组件未启动或尚无扫描结果
 *         ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t wifi_manager_scan_get_ap_list(wifi_ap_record_t *out, uint16_t *inout_count);

/**
 * @brief 查询当前的 WiFi 工作模式偏好（v0.6.0 新增）
 *
 * @return 当前模式（读内存快照，非阻塞）
 */
wifi_manager_mode_t wifi_manager_get_mode(void);

/**
 * @brief 设置 WiFi 工作模式偏好（v0.6.0 新增，**非阻塞**）
 *
 * 行为：
 *   · 立即更新内存快照（wifi_manager_get_mode 随后即返回新值）；
 *   · 把"写 NVS + 切换 esp_wifi 模式"排给内部任务异步执行，
 *     本函数写完队列即返回，**不等待结果**。
 *
 * 为什么设计成非阻塞：调用方是设备 UI 的模式按钮回调（LVGL 事件回调，
 * 运行在持有 LVGL 锁的 UI 主循环里）。若像 connect()/forget() 那样等待
 * 内部任务完成，界面会冻结（NVS 写 + esp_wifi_set_mode 可达数百 ms）。
 *
 * 切换语义：
 *   · AUTO：有凭据 → 连路由器；无凭据 → 起配网热点；
 *   · STA ：关闭配网热点（WIFI_MODE_STA），有凭据则连接；无凭据则只等
 *           配网下发（此时设备不可通过热点访问，UI 会提示先配网）；
 *   · AP  ：强制起配网热点（WIFI_MODE_APSTA），并断开当前 STA 连接。
 *
 * @param mode 目标模式
 * @return ESP_OK 请求已受理；ESP_ERR_INVALID_ARG 模式非法；
 *         ESP_ERR_INVALID_STATE 组件未启动；ESP_ERR_TIMEOUT 队列已满
 */
esp_err_t wifi_manager_set_mode(wifi_manager_mode_t mode);

/**
 * @brief 查询当前 STA 连接的路由器信号强度（v0.6.0 新增，供状态栏图标用）
 *
 * @param out_rssi 输出信号强度（dBm，负值；数值越接近 0 说明信号越强），不可为 NULL
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 参数非法
 *         ESP_ERR_INVALID_STATE 组件未启动 / 当前未连接路由器（AP 模式或无凭据）
 */
esp_err_t wifi_manager_get_rssi(int8_t *out_rssi);

/**
 * @brief RSSI → 信号格数（1~4），v0.6.0 新增
 *
 * 分档写死在这里（而不是各 UI 各写一套），保证设备状态栏图标与
 * WiFi 页列表里的信号条档位一致：
 *   ≥ -60dBm → 4 格（优秀）；≥ -70 → 3 格（良好）；
 *   ≥ -80 → 2 格（一般）；其余 → 1 格（弱）。
 *
 * @param rssi 信号强度（dBm）
 * @return 1~4 格
 */
static inline int wifi_manager_rssi_level(int8_t rssi)
{
    if (rssi >= -60) {
        return 4;
    }
    if (rssi >= -70) {
        return 3;
    }
    if (rssi >= -80) {
        return 2;
    }
    return 1;
}
