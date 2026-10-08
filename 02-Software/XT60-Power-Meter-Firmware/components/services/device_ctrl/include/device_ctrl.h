/**
 * @file device_ctrl.h
 * @brief 设备控制服务：把"系统状态"翻译成"用户能感知的行为"
 *
 * ─────────────────────────────────────────────────────────────
 * 本模块是"依赖倒置"（方案 B）的落地点
 * ─────────────────────────────────────────────────────────────
 * 问题：系统状态机需要让别人把状态显示出来（指示灯、蜂鸣器），
 *       但如果状态机直接调用 status_led / buzzer 的接口，
 *       业务层就依赖了驱动层，分层被打破，而且以后新增一种提示方式
 *       还得回头改状态机。
 *
 * 方案 B 的做法：
 *   状态机（system_state）只广播事件，完全不知道谁在听；
 *   本模块（device_ctrl，业务层）主动订阅事件，负责把状态映射成具体动作。
 *
 *   依赖方向：device_ctrl ──> system_state   （订阅"我关心你"）
 *             system_state ──> event_bus     （只发通知，不认人）
 *             system_state  不再 ──> status_led / buzzer  ✗（依赖已切断）
 *
 * ─────────────────────────────────────────────────────────────
 * 本模块负责的输出映射
 * ─────────────────────────────────────────────────────────────
 * | 系统状态  | 指示灯       | 蜂鸣器                    |
 * |-----------|--------------|---------------------------|
 * | INIT      | 常亮         | 静音                      |
 * | RUNNING   | 慢闪 1 Hz    | 静音                      |
 * | DEGRADED  | 双闪 + 长灭  | 静音                      |
 * | FAULT     | 快闪 5 Hz    | 每 1 秒长鸣一声（报警）   |
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 初始化设备控制服务
 *
 * 做两件事：
 *   ① 订阅系统状态变化事件；
 *   ② 立即按当前状态同步一次输出（因为订阅之前可能已经发生过状态变化，
 *      只靠事件会漏掉——这是订阅式编程常见的"首帧同步"问题）。
 *
 * 前置条件：event_bus、status_led、buzzer、system_state 均已初始化。
 *           其中 buzzer 必须先于本模块初始化，否则首帧同步时无法设置报警。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_NO_MEM 订阅表已满
 */
esp_err_t device_ctrl_init(void);

/**
 * @brief 通知"启动流程已全部通过"，播放一声短提示音
 *
 * 为什么不在 buzzer_init() 里直接发声？
 *   因为"初始化完成提示音"表达的是"整个系统启动通过"这一系统级含义，
 *   而 buzzer_init() 只负责配置硬件。把发声放在驱动初始化里会产生
 *   难以预期的副作用（比如将来单独测试蜂鸣器时会莫名其妙响一声）。
 *   因此由业务层在启动结束时显式调用本函数。
 *
 * 若蜂鸣器本身已被登记为故障，会跳过发声并返回错误码（不视为致命问题）。
 *
 * @return ESP_OK 已播放
 *         ESP_ERR_INVALID_STATE 蜂鸣器故障或尚未初始化
 */
esp_err_t device_ctrl_notify_boot_ok(void);

/**
 * @brief 设置蜂鸣器响度（供 UI 调用）
 *
 * ── 分层说明：为什么 UI 不直接调用 buzzer_set_volume() ──
 * UI 属于业务层，不应直接依赖驱动层（会破坏分层，且将来要加"音量上限
 * 限制""静音模式""调节时播放试听音"等业务规则时无处安放）。
 * 因此统一由本模块作为业务的入口，内部负责：
 *   ① 调 buzzer_set_volume() 让新响度立即生效（从下一声开始）；
 *   ② 调 settings_set_buzzer_volume() 更新配置缓存并安排延迟落盘。
 *
 * 落盘采用防抖策略（见 settings.h），因此可以在滑块拖动过程中
 * 放心地连续调用本函数，不会加速 Flash 磨损。
 *
 * @param percent 响度 1~100
 * @return ESP_OK 响度已生效（落盘失败只记录告警，不影响本次生效）
 *         ESP_ERR_INVALID_ARG 超出范围
 *         ESP_ERR_INVALID_STATE 蜂鸣器尚未初始化
 */
esp_err_t device_ctrl_set_buzzer_volume(uint8_t percent);

/**
 * @brief 读取当前蜂鸣器响度
 *
 * @param percent 输出响度，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为 NULL
 */
esp_err_t device_ctrl_get_buzzer_volume(uint8_t *percent);

/**
 * @brief 立即把用户设置写入 NVS（忽略防抖延迟）
 *
 * 适用场合：用户离开设置页面、或设备即将关机前，确保设置不会丢失。
 *
 * @return ESP_OK 成功；其余为 NVS 错误码（当前运行不受影响）
 */
esp_err_t device_ctrl_save_settings(void);
