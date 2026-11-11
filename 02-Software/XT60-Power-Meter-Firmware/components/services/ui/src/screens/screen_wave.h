/**
 * @file screen_wave.h
 * @brief 波形页：双轴折线图（电压/电流/功率/电能）+ 上滑设置滑板
 */
#pragma once

#include "lvgl.h"

#include "button.h"
#include "power_measure.h"

/**
 * @brief 在指定 tile 内构建波形页
 * @param tile tileview 页容器
 */
void screen_wave_create(lv_obj_t *tile);

/** @brief 定时刷新曲线（真机固定 25Hz，由 ui.c 每 40ms 调用一次） */
void screen_wave_refresh(void);

/** @brief 打开底部波形设置滑板（K2 长按 / 触摸上滑） */
void screen_wave_open_menu(void);

/**
 * @brief 波形页滑动处理（ui_nav indev 级滑动识别调用）：上滑开 / 下滑关
 *
 * 只处理竖向滑动；横向滑动仍归 tileview 翻页（本函数不参与）。
 */
void screen_wave_swipe_cb(lv_dir_t dir);

/** @brief 关闭滑板（apply=true 时读取 roller 选择并应用） */
void screen_wave_close_menu(bool apply);

/** @brief 滑板是否处于打开状态 */
bool screen_wave_menu_is_open(void);

/**
 * @brief 滑板打开期间的按键处理（消费返回 true，未消费返回 false）
 *
 * 单击 K1/K2 = 焦点 roller 上一项/下一项；双击 K2 = 切换焦点 roller；
 * 长按 K2 = 关闭并应用、长按 K1 = 关闭并放弃修改。
 */
bool screen_wave_menu_key(button_id_t id, button_event_t ev);

/** @brief 当前时间窗 / 模式（供标题显示） */
pm_wave_window_t screen_wave_get_window(void);
pm_wave_mode_t screen_wave_get_mode(void);
