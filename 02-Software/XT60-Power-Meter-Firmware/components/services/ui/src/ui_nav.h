/**
 * @file ui_nav.h
 * @brief 三页导航：lv_tileview 横向三页 + 共享状态栏 + 页面指示点 + 按键路由
 *
 * 页面顺序（手指/按键一致）：测量数据 → 波形 → 设置。
 */
#pragma once

#include "lvgl.h"

#include "button.h"
#include "data_store.h"

typedef enum {
    UI_PAGE_MEASURE = 0,
    UI_PAGE_WAVE,
    UI_PAGE_SETTINGS,
    UI_PAGE_MAX,
} ui_page_t;

/**
 * @brief 在当前活动屏幕上构建三页导航（启动画面销毁后调用）
 */
void ui_nav_build(void);

/**
 * @brief 周期刷新：共享状态栏（时间/电量）+ 当前页面内容（5Hz）
 */
void ui_nav_refresh(const data_store_snapshot_t *snap);

/**
 * @brief 处理一个实体按键事件（由 UI 任务从输入队列取出后调用）
 */
void ui_nav_handle_key(button_id_t id, button_event_t ev);
