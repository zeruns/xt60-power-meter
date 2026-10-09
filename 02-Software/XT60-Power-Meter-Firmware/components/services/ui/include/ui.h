/**
 * @file ui.h
 * @brief LVGL 界面层（与非 UI 代码物理隔离）
 *
 * 结构：
 *   src/screens/  启动画面、测量页、波形页、设置页、波形设置滑板
 *   src/ui_nav    三页 tileview 导航、页面指示点、按键路由
 *
 * 数据只通过 middleware/data_store 的快照读取，不直接访问驱动层。
 * 按键事件由 button 驱动回调投递队列，在 UI 任务内消费。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 构建全部界面并启动 UI 任务（绑定 Core 0）
 *
 * 依赖 middleware/lvgl_port 已初始化。
 *
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t ui_init(void);
