/**
 * @file screen_settings.h
 * @brief 设置页：2×3 功能网格入口 + 各功能二级页
 *
 * 二级页覆盖在设置 tile 内（不跨页），打开期间由 screen_settings_refresh
 * 持续刷新页内的实时数值（如校准向导的当前读数、电能统计）。
 */
#pragma once

#include "lvgl.h"

#include "button.h"
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

/**
 * @brief 查询设置页是否需要接管按键（ui_nav 按键路由的门）
 *
 * 返回 true 的情形：① 二级页打开中；② 弹窗打开中（模态）；
 * ③ 当前就是设置页首页——首页网格入口（长按 K2 进入网格焦点模式）
 * 也要靠按键打开功能格，所以首页同样需要接管。
 * 未进入网格模式时 screen_settings_key 会返回 false 把按键交还给
 * ui_nav 的三页导航（翻页/回测量页键位不变）。
 *
 * @return true=应把按键交给 screen_settings_key 先处理
 */
bool screen_settings_subpage_is_open(void);

/**
 * @brief 查询设置页是否有模态浮层（二级页/弹窗）打开——不管当前在哪一页
 *
 * 供 ui_nav 的滑动识别使用：模态浮层打开时抑制"环形翻页手势"，避免一次
 * 横滑把承载浮层的设置 tile 整个翻走、浮层悬空（连带按键路由错乱）。
 *
 * @return true=有模态浮层打开中
 */
bool screen_settings_modal_is_open(void);

/**
 * @brief 设置页二级页/弹窗的纯按键操作（焦点链机制，见 screen_settings.c）
 *
 * 键位（与测量/波形页快速菜单同心智，只用 K1/K2 两键）：
 *   · 二级页内单击 K1/K2 = 焦点在可操作控件间循环移动（焦点有蓝色描边）；
 *   · 长按 K2 = 激活焦点控件（按钮=点击、开关=切换、滑条=进"调节模式"，
 *     调节模式内单击 K1/K2 = 减/加步进、长按 K2 = 退出调节模式）；
 *   · 长按 K1 = 返回/关闭当前子页（与触屏"返回"按钮等效）；
 *   · 弹窗打开时固定键位：K1=取消、K2=确定。
 *
 * @param id 按键 ID（BUTTON_ID_BOOT=K1 / BUTTON_ID_USER=K2）
 * @param ev 按键事件（单击/双击/长按起始等）
 * @return true=事件已消费（ui_nav 不再翻页/开菜单）；false=本页未接管
 */
bool screen_settings_key(button_id_t id, button_event_t ev);