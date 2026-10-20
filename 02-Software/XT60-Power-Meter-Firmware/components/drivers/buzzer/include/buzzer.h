/**
 * @file buzzer.h
 * @brief 无源蜂鸣器驱动（BOARD_PIN_BUZZER / IO1，经 Q5 低边 N-MOS 驱动）
 *
 * ─────────────────────────────────────────────────────────────
 * 为什么无源蜂鸣器必须用 PWM，不能像 LED 那样直接给高低电平？
 * ─────────────────────────────────────────────────────────────
 * 蜂鸣器分两类：
 *   · 有源蜂鸣器：内部自带振荡电路，只要给它直流电压就响，音调固定；
 *   · 无源蜂鸣器（本板使用）：内部只是一个压电陶瓷片/线圈，没有振荡源，
 *     必须由外部输入交变方波才能发声，方波频率决定音调。
 * 因此本驱动用 LEDC 产生约 4kHz 的方波——4kHz 是 BUZZER1 的谐振频率，
 * 在这个频率上发声效率最高、最响。
 *
 * ─────────────────────────────────────────────────────────────
 * 三种提示方式
 * ─────────────────────────────────────────────────────────────
 *   1. buzzer_play()      —— 播放一次提示音（按键音/成功音/失败音），非阻塞；
 *   2. buzzer_set_alarm() —— 周期报警：每隔 1 秒长鸣一声，用于异常状态；
 *   3. buzzer_stop()      —— 立即静音并关闭报警。
 *
 * 实现上使用 esp_timer 而不是 FreeRTOS 任务（与 status_led 保持一致）：
 * 不额外占用任务栈，也不参与调度竞争。
 *
 * 优先级约定：报警优先。报警开启期间调用 buzzer_play() 会被忽略，
 * 避免提示音与报警互相打断，听起来时断时续。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 语义化提示音类型 */
typedef enum {
    BUZZER_TONE_CLICK = 0, /**< 按键点击音（最短，用于操作反馈） */
    BUZZER_TONE_OK,        /**< 操作成功 / 启动完成 */
    BUZZER_TONE_ERROR,     /**< 操作失败 */
} buzzer_tone_t;

/**
 * @brief 初始化蜂鸣器
 *
 * 配置 LEDC 输出方波，并创建内部定时器；初始化后处于静音状态。
 *
 * 响度初值取 Kconfig 的 CONFIG_BUZZER_VOLUME_PERCENT（即"出厂默认响度"）。
 * 运行期可通过 buzzer_set_volume() 修改，用户设定值的保存与恢复由
 * settings 服务负责（本驱动不关心持久化）。
 *
 * 说明：本函数刻意不在内部发声。"初始化完成提示音"属于系统级行为
 * （表示整个启动流程通过），由业务层在启动结束时调用
 * device_ctrl_notify_boot_ok() 触发，而不是塞在驱动里产生副作用。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 重复初始化
 *         ESP_ERR_NO_MEM 定时器创建失败
 */
esp_err_t buzzer_init(void);

/**
 * @brief 设置响度（运行时可调，供 UI 使用）
 *
 * 内部通过调整 PWM 占空比实现，不重配定时器，因此不会影响音调。
 *
 * 生效时机：**从下一次发声开始生效**。
 *   之所以不做"立即改写正在播放的提示音"，是因为发声可能正由
 *   esp_timer 回调结束（把占空比清零），而本函数由 UI 任务调用，
 *   两者并发时若强行改写占空比，存在"回调已清零、本函数又写回非零"的
 *   竞态窗口，会导致蜂鸣器持续鸣响且无人负责关掉它。
 *   实际使用中提示音都很短（几十到几百毫秒），且 UI 调音时通常会播放
 *   试听音，因此"下一声生效"在体验上没有差别。
 *
 * @param percent 响度 1~100（100 为最响）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 超出范围
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t buzzer_set_volume(uint8_t percent);

/**
 * @brief 读取当前响度
 *
 * @param percent 输出响度，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为 NULL；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t buzzer_get_volume(uint8_t *percent);

/**
 * @brief 播放一次提示音（非阻塞，响完自动停止）
 *
 * @param tone 提示音类型
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 类型非法
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t buzzer_play(buzzer_tone_t tone);

/**
 * @brief 开启 / 关闭周期报警
 *
 * 开启后立即响第一声，之后每隔 1 秒长鸣一声，用于异常状态提醒。
 * 关闭时会立即静音。
 *
 * @param enable true 开启报警，false 关闭报警
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t buzzer_set_alarm(bool enable);

/**
 * @brief 立即静音，并关闭报警
 *
 * 与 buzzer_set_alarm(false) 的区别：本函数还会中断当前正在播放的提示音，
 * 是一个"彻底安静下来"的操作。
 *
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t buzzer_stop(void);
