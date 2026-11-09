/**
 * @file screen_measure.h
 * @brief 测量数据页：状态栏 + 电压/电流/功率/电能大数值卡片 + 温湿度小字
 */
#pragma once

#include "lvgl.h"

#include "button.h"
#include "data_store.h"

/**
 * @brief 在指定 tile 内构建测量页
 * @param tile tileview 提供的页容器
 */
void screen_measure_create(lv_obj_t *tile);

/**
 * @brief 用最新快照刷新测量页（刷新率由快速菜单配置，1~30Hz）
 */
void screen_measure_refresh(const data_store_snapshot_t *snap);

/**
 * @brief 打开快速菜单（K2 长按触发）
 *
 * 菜单含两项：① 清除电能统计数据；② 数据刷新率（1/5/10/20/30 Hz）。
 */
void screen_measure_open_menu(void);

/**
 * @brief 测量页滑动处理（ui_nav indev 级滑动识别调用）
 *
 * 上滑（LV_DIR_TOP）出快速菜单，下滑（LV_DIR_BOTTOM）关闭。
 */
void screen_measure_swipe_cb(lv_dir_t dir);

/**
 * @brief 快速菜单是否处于打开状态
 */
bool screen_measure_menu_is_open(void);

/**
 * @brief 快速菜单按键处理
 *
 * 打开期间：长按（K1/K2）关闭菜单；单击 K1/K2 = 焦点上/下移，
 * 双击 = 激活当前焦点行（其余事件不消费，交回 ui_nav）。
 * @return true 表示事件已被菜单消费
 */
bool screen_measure_menu_key(button_id_t id, button_event_t ev);
