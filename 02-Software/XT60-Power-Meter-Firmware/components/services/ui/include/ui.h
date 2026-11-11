/**
 * @file ui.h
 * @brief LVGL 界面层（与非 UI 代码物理隔离）
 *
 * 结构：
 *   src/screens/  启动画面、测量页、波形页、设置页、波形设置滑板
 *   src/ui_nav    三页 tileview 导航、页面指示点、按键路由
 *
 * 数据只通过 middleware/data_store 的快照读取，不直接访问驱动层。
 * 按键事件由 button 驱动回调投递队列，在 UI 任务内消费。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief 创建 UI 任务并立即进入启动页阶段（绑定 Core 0）
 *
 * 依赖 middleware/lvgl_port 已初始化，且按键驱动已就绪（任务启动时会注册回调）。
 * 与旧版不同：主界面不再固定延时后构建，而是等到 ui_boot_finish() 被调用，
 * 因此本函数可在 app_init 的显示链路就绪后提前调用，让启动页尽早点亮。
 *
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 任务创建失败；ESP_ERR_INVALID_STATE 已创建
 */
esp_err_t ui_init(void);

/**
 * @brief 更新启动页真实进度（线程安全，由 app_init 的初始化循环调用）
 *
 * 只写一份共享快照（已步数/总步数/阶段文字），不直接操作 LVGL 对象；
 * UI 任务在启动页循环里读取快照并渲染，避免跨任务直接改控件。
 * 若 UI 任务尚未构建启动页，快照会保留，构建完成后立即应用。
 *
 * @param done  已完成步数（0 ~ total）
 * @param total 总步数（至少 1）
 * @param label 当前阶段短文本（如 "测量前端 17/23"，UTF-8，最长 47 字节）
 */
void ui_boot_set_progress(uint32_t done, uint32_t total, const char *label);

/**
 * @brief 通知启动页初始化结束，UI 任务随后构建主界面
 *
 * 由 app_tasks_start() 在测量任务启动后调用（仅显示链路无故障时）。
 * UI 任务尚未创建时为空操作，保证故障降级路径安全。
 */
void ui_boot_finish(void);

/**
 * @brief 告警弹窗是否正打开（模态中）
 *
 * 【历史声明：仓库内已无定义与调用】v0.6.8 起模态判定统一由 ui_msgbox 模块
 * 的 ui_msgbox_is_open() 提供（覆盖告警框、确认框、提示框），ui_nav 的手势
 * 抑制也已改用后者。本声明保留在原处未清理，请勿在新代码中引用。
 *
 * @return true=告警弹窗在屏上
 */
bool ui_alert_modal_is_open(void);

/**
 * @brief 取首页数据刷新率（Hz）
 */
uint8_t ui_get_display_rate_hz(void);

/**
 * @brief UI 任务主循环心跳（最近一轮的时间戳，微秒）
 *
 * 诊断用：与 app_task 的"冻结诊断"配合，判断 UI 任务本身是否也停摆
 * （渲染停摆但心跳在走 = taskLVGL 卡；两者都停 = UI 任务卡在锁内）。
 */
int64_t ui_get_last_loop_us(void);

/**
 * @brief 设置首页数据刷新率（1/5/10/20/30 Hz，立即生效并持久化到 NVS）
 *
 * 由测量页快速菜单调用。测量核固定以 40Hz 推送即时均值快照，
 * UI 按所选档位取历史窗口平均：低档（1Hz≈40 帧窗口）读数更稳，
 * 高档（30Hz=取最新帧）响应更快。该设置只影响测量页，波形页固定 25Hz。
 */
void ui_set_display_rate_hz(uint8_t hz);

/**
 * @brief 记录一次用户活动（v0.6.2 需求 11：自动降亮）
 *
 * 按键与触摸都应调用：刷新"最后活动时刻"以重置自动降亮计时，
 * 若屏幕已降亮到 10% 则立即恢复用户设置亮度。
 * 触摸侧由 ui_nav 的 indev 事件回调调用（那里本就处在 LVGL 锁内）。
 */
void ui_note_user_activity(void);
