/**
 * @file ui.h
 * @brief LVGL 界面层（与非 UI 代码物理隔离）
 *
 * 规划的子目录：
 *   src/screens/   主界面、曲线、设置、校准、关于
 *   src/widgets/   自定义 LVGL 控件
 *   src/theme/     主题与样式
 *   assets/        字体、图标、图片等 LVGL 资源
 *
 * 数据只通过 middleware/data_store 的快照读取，不直接访问驱动层。
 *
 * 当前阶段：用 LVGL 官方 widgets demo 验证显示与触控；
 *   正式界面就绪后替换为 src/screens/ 下的界面构建。
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
