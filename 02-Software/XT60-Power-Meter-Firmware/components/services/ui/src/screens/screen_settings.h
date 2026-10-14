/**
 * @file screen_settings.h
 * @brief 设置页：2×3 功能网格入口 + 各功能二级页
 *
 * 二级页覆盖在设置 tile 内（不跨页），打开期间由 screen_settings_refresh
 * 持续刷新页内的实时数值（如校准向导的当前读数、电能统计）。
 */
#pragma once

#include "lvgl.h"

#include "data_store.h"

/**
 * @brief 在设置 tile 上构建设置首页（网格）
 * @param tile tileview 的设置页
 */
void screen_settings_create(lv_obj_t *tile);

/**
 * @brief 数据刷新（由 ui_nav_refresh 转发当前快照，5Hz）
 *
 * 仅当设置页内有需要实时数值的二级页（校准向导/电能统计）打开时才有动作。
 * @param snap 最新测量快照，不可为 NULL
 */
void screen_settings_refresh(const data_store_snapshot_t *snap);