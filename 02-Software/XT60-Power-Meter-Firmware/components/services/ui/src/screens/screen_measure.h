/**
 * @file screen_measure.h
 * @brief 测量数据页：状态栏 + 电压/电流/功率/电能大数值卡片 + 温湿度小字
 */
#pragma once

#include "lvgl.h"

#include "data_store.h"

/**
 * @brief 在指定 tile 内构建测量页
 * @param tile tileview 提供的页容器
 */
void screen_measure_create(lv_obj_t *tile);

/**
 * @brief 用最新快照刷新测量页（5Hz 左右）
 */
void screen_measure_refresh(const data_store_snapshot_t *snap);
