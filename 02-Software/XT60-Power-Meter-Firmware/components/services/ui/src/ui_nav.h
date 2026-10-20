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
 *
 * 模拟器与"整体刷新"场景使用；真机 ui.c 已按页面拆分为独立刷新节奏
 * （测量页按用户设定刷新率、波形页固定 25Hz），不再统一走本函数。
 */
void ui_nav_refresh(const data_store_snapshot_t *snap);

/** @brief 仅刷新共享状态栏（运行时间/页码点/电量/WiFi），任何页面都可调 */
void ui_nav_refresh_statusbar(const data_store_snapshot_t *snap);

/** @brief 查询当前所在页（供 ui.c 决定每页的刷新节奏） */
ui_page_t ui_nav_current_page(void);

/**
 * @brief 处理一个实体按键事件（由 UI 任务从输入队列取出后调用）
 */
void ui_nav_handle_key(button_id_t id, button_event_t ev);
