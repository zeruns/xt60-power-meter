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

/** @brief 定时刷新曲线（建议 5Hz） */
void screen_wave_refresh(void);

/** @brief 打开底部波形设置滑板（K2 长按 / 触摸上滑） */
void screen_wave_open_menu(void);

/** @brief 关闭滑板（apply=true 时读取 roller 选择并应用） */
void screen_wave_close_menu(bool apply);

/** @brief 滑板是否处于打开状态 */
bool screen_wave_menu_is_open(void);

/**
 * @brief 滑板打开期间的按键处理（消费返回 true，未消费返回 false）
 */
bool screen_wave_menu_key(button_id_t id, button_event_t ev);

/** @brief 当前时间窗 / 模式（供标题显示） */
pm_wave_window_t screen_wave_get_window(void);
pm_wave_mode_t screen_wave_get_mode(void);
