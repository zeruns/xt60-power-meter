/**
 * @file settings.h
 * @brief 配置持久化服务（NVS）：保存用户在 UI 上调整的偏好设置
 *
 * ─────────────────────────────────────────────────────────────
 * 为什么需要"运行时配置"而不是只用 Kconfig？
 * ─────────────────────────────────────────────────────────────
 * Kconfig 里的值在编译期就固定了，用户无法在设备上更改。
 * 而蜂鸣器响度这类参数属于"用户偏好"，需要：
 *   · 运行时可通过 UI 调整（立即生效）；
 *   · 掉电后仍能记住（持久化到 NVS）；
 *   · 且第一次上电要有合理初值（取 Kconfig 里的出厂默认值）。
 *
 * 因此本服务的职责分工是：
 *   Kconfig  —— 只作为"出厂默认值"来源；
 *   NVS      —— 保存用户实际调整后的值；
 *   内存缓存 —— 提供快速读取（避免每次都访问 Flash）。
 *
 * ─────────────────────────────────────────────────────────────
 * 关于写入磨损（重要设计考虑）
 * ─────────────────────────────────────────────────────────────
 * NVS 存放在 Flash 上，每个扇区约 10 万次擦写寿命。
 * 如果 UI 用滑块调节音量，拖动过程中会连续产生几十次数值变化——
 * 若每次都立刻写 Flash，会显著加速磨损。
 *
 * 所以本服务采用**防抖（debounce）**策略：
 *   每次数值变化只更新内存缓存并重置一个定时器；
 *   当连续 SETTINGS_SAVE_DELAY_MS 毫秒内不再有新变化时，才真正写 NVS。
 * 这样无论 UI 怎么连续拖动，一次调节最多只产生一次写入。
 *
 * 另外提供 settings_save_now() 供需要"立即落盘"的场合使用
 * （例如关机前、或用户在设置页点击"完成"时）。
 *
 * ─────────────────────────────────────────────────────────────
 * 注意：写 Flash 会短暂暂停缓存
 * ─────────────────────────────────────────────────────────────
 * ESP32 写 Flash 期间 cache 会被禁用，所有执行 Flash 中代码的任务都会
 * 短暂停滞（约十几到几十毫秒）。这是硬件特性，无法通过换任务规避。
 * 对测量精度敏感的场景，建议把落盘时机放在测量空闲时（如用户停止调节后）。
 * 本服务的防抖策略恰好也把写入集中到了"用户停止调节"之后。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 应用配置（可持久化的用户偏好）
 *
 * 新增配置项时：
 *   ① 在结构体里加字段；
 *   ② 在 settings.c 的 load_defaults() 里给出出厂默认值；
 *   ③ 在 settings.c 的载入/保存函数里各加一行读写。
 * 采用"逐项独立存取"而不是整体结构体 blob，好处是新增字段不会破坏
 * 已存数据、也不需要做版本迁移。
 */
typedef struct {
    uint8_t buzzer_volume;  /**< 蜂鸣器响度 1~100（出厂默认取 CONFIG_BUZZER_VOLUME_PERCENT） */
    uint8_t lcd_brightness; /**< 屏幕背光亮度 10~100（出厂默认 80；下限 10 防止全黑无法操作） */
    uint8_t display_rate_hz; /**< 首页数据刷新率（1/5/10/20/30 Hz，出厂默认 5） */
    uint8_t orientation_auto; /**< 屏幕是否随重力自动旋转（1=自动，0=手动，出厂默认 0） */
    uint16_t orientation_angle; /**< 手动方向角度（0/90/180/270，默认 0=横屏） */
    uint8_t wave_smooth; /**< 波形曲线平滑（1=开启，0=关闭，出厂默认 0） */
} settings_t;

/**
 * @brief 告警阈值配置（v0.2.0 仪器功能批）
 *
 * 三项独立使能 + 阈值；判定在测量核块边界做（见 power_measure.c），
 * 本结构只负责持久化用户设定。
 */
typedef struct {
    uint8_t v_en;   /**< 过压告警使能（1=开启） */
    uint8_t i_en;   /**< 过流告警使能 */
    uint8_t p_en;   /**< 过功率告警使能 */
    float v_limit;  /**< 过压阈值（V） */
    float i_limit;  /**< 过流阈值（A） */
    float p_limit;  /**< 过功率阈值（W） */
} settings_alarm_t;

/**
 * @brief 初始化配置服务：初始化 NVS 并把配置载入内存缓存
 *
 * 即使 NVS 不可用（首次上电、分区损坏等）也会成功返回——
 * 此时缓存中全部是出厂默认值，设备仍能正常工作，只是无法记住用户设置。
 * 因此本模块在启动表中属于"非关键"模块。
 *
 * @return ESP_OK 成功（含"NVS 不可用但已用默认值"的情况）
 *         ESP_ERR_NO_MEM 内存不足
 */
esp_err_t settings_init(void);

/**
 * @brief 读取当前生效的配置（从内存缓存，速度很快）
 *
 * @param out 输出配置，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_get(settings_t *out);

/**
 * @brief 更新蜂鸣器响度：写入内存缓存并安排延迟落盘
 *
 * 本函数不会立即写 Flash（防抖），因此可以放心地在滑块拖动过程中频繁调用。
 *
 * @param percent 响度 1~100
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 超出范围
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_set_buzzer_volume(uint8_t percent);

/**
 * @brief 更新屏幕背光亮度：写入内存缓存并安排延迟落盘
 *
 * 与响度相同采用防抖策略：设置页滑块拖动期间可放心高频调用，
 * 由调用方负责同步调用 lcd_nv3030b_set_backlight() 让亮度立即生效。
 *
 * @param percent 亮度 10~100（下限 10 防止把屏幕调到全黑后无法操作）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 超出范围
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_set_lcd_brightness(uint8_t percent);

/**
 * @brief 更新首页数据刷新率（测量页快速菜单中可调）
 *
 * 合法档位：1 / 5 / 10 / 20 / 30 Hz。测量核固定 40Hz 推送即时快照，
 * 低档位由 UI 层对快照历史做窗口平均（更稳），高档位提高界面响应速度；
 * 只影响测量页（波形页固定 25Hz）。
 *
 * @param hz 刷新率（仅接受 1/5/10/20/30）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 非合法档位；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_set_display_rate_hz(uint8_t hz);

/**
 * @brief 更新屏幕方向设置（显示设置页调用；立即防抖落盘）
 *
 * @param auto_on 是否启用重力自动旋转（1/0）
 * @param angle   手动方向角度（auto_on=0 时生效；仅接受 0/90/180/270）
 */
esp_err_t settings_set_orientation(uint8_t auto_on, uint16_t angle);

/**
 * @brief 设置波形曲线平滑开关
 * @param on 1=开启平滑，0=关闭（默认）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t settings_set_wave_smooth(uint8_t on);

/**
 * @brief 立即把内存缓存写入 NVS（忽略防抖）
 *
 * 典型用法：用户离开设置页、或设备即将关机前调用。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚未初始化
 *         ESP_ERR_NVS_* NVS 写入失败（内存缓存仍然有效，不影响当前运行）
 */
esp_err_t settings_save_now(void);

/**
 * @brief 读取当前告警阈值配置（内存缓存）
 * @param out 输出，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_get_alarm(settings_alarm_t *out);

/**
 * @brief 更新告警阈值配置：写入内存缓存并安排延迟落盘（防抖）
 *
 * 非法输入（阈值 ≤0、NaN/inf）拒绝并保持原值不变。
 *
 * @param in 新配置，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_set_alarm(const settings_alarm_t *in);
