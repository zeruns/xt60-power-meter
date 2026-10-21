/**
 * @file ui_msgbox.h
 * @brief 全局共享模态对话框（单按钮提示框 / 取消-确定确认框）
 *
 * 【v0.6.8 从 screen_settings.c 上移】原实现只服务设置页；首页快速菜单的
 * "清除电能统计"也要弹同样的确认框（破坏性操作必须二次确认），因此把
 * 整套对话框基础设施（顶层图层可点击技巧 + 压暗遮罩 + 中文字体 + 按键
 * 取消/确定 + 叠窗防御）上移为共享模块，设置页与测量页共用同一实现，
 * 保证"跟设置里的一样"不是口号而是同一份代码。
 *
 * 对话框挂在 LVGL 顶层图层（lv_layer_top）上，天然浮在三个页面之上；
 * 同一时刻至多一个对话框（弹新框前自动关旧框，防叠窗孤儿，见 .c 内注释）。
 *
 * 按键约定（ui_nav_handle_key 最优先路由给本模块）：
 *   · K1（BUTTON_ID_BOOT）= 取消 / 关闭；
 *   · K2（BUTTON_ID_USER）= 确定（确认框）；提示框两键都等于"确定"。
 */
#pragma once

#include <stdbool.h>

#include "button.h"
#include "lvgl.h"

/** 确认框"确定"后的业务动作（点"取消"或点遮罩关闭都不回调） */
typedef void (*ui_confirm_cb_t)(void);

/**
 * @brief 弹出单按钮提示框（"确定"，看完点掉）
 * @param title 标题文本
 * @param text  正文文本
 */
void ui_msgbox_notice(const char *title, const char *text);

/**
 * @brief 弹出确认框（取消 / 确定），确定后才执行 ok_cb
 * @param title 标题文本
 * @param text  正文文本（建议说明后果与"不可撤销"）
 * @param ok_cb 点"确定"后执行的业务动作；先关框再回调（回调里可再弹新框）
 */
void ui_msgbox_confirm(const char *title, const char *text, ui_confirm_cb_t ok_cb);

/**
 * @brief 同步关闭当前对话框（无则空操作）
 *
 * 供"即将弹新框/切页"的调用方先清场：单例遮罩/句柄被新框覆盖会造成
 * 旧框成孤儿关不掉（历史卡死 bug 的根治入口，见 .c 内注释）。
 */
void ui_msgbox_close_active(void);

/**
 * @brief 当前是否有对话框在屏上（模态中）
 *
 * ui_nav 据此做两件事：① 按键路由最优先交给 ui_msgbox_key()；
 * ② 抑制上下滑手势（弹窗期间不该顺手把快速菜单拉出来）。
 */
bool ui_msgbox_is_open(void);

/**
 * @brief 对话框的按键处理：K1=取消/关闭，K2=确定（单击与长按起始都认）
 * @param id 按键编号（BUTTON_ID_BOOT=K1 / BUTTON_ID_USER=K2）
 * @param ev 按键事件（仅单击/长按起始有动作，其余事件吞掉不外漏）
 */
void ui_msgbox_key(button_id_t id, button_event_t ev);
