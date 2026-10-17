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
 * @brief 构建全部界面并启动 UI 任务（绑定 Core 0）
 *
 * 依赖 middleware/lvgl_port 已初始化。
 *
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t ui_init(void);

/**
 * @brief 告警弹窗是否正打开（模态中）
 *
 * 供 ui_nav 的滑动识别抑制手势：告警弹窗期间不允许横滑环形翻页/
 * 上滑开快速菜单（弹窗挂在顶层图层，翻页会把承载页抽走）。
 * 模拟器无 ui.c，由 stubs.c 返回 false。
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
