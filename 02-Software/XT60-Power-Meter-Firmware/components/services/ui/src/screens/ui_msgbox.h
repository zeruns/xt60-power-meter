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
 * @brief 按当前屏幕宽度给对话框设置自适应宽度（v0.6.8 修复竖屏被裁）
 *
 * 宽度 = min(屏宽 − 2×12px 安全边距, 280px)，下限 120px：
 *   · 横屏 284 宽 → 260px；竖屏 240 宽 → 216px，两种方向都完整显示；
 *   · 280px 上限防止未来换更大屏时对话框被拉得过宽、长句一行太长难读。
 *
 * 为什么必须自适应：旧实现硬编码 250px，竖屏（240 宽）时左右各溢出 5px，
 * 弹窗两端被裁掉。LVGL 9 的 msgbox 正文标签宽度是 content 的 100%，
 * 因此缩窄对话框后长文本会自动换行，无需逐条改文案。
 *
 * 旋转时也需重新调用（lv_display_set_rotation 会广播
 * LV_EVENT_RESOLUTION_CHANGED，本模块与 ui.c 的告警框各自监听并重新
 * fit + center），否则弹窗开着转屏会再次裁切。
 *
 * @param box lv_msgbox_create 返回的对话框对象（NULL 时安全返回）
 */
void ui_msgbox_fit_width(lv_obj_t *box);

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
