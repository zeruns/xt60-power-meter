/**
 * @file splash.h
 * @brief 启动画面（白底 + 品牌色 + Bold 标题 + 加载进度条）
 */
#pragma once

#include "lvgl.h"

/**
 * @brief 在当前活动屏幕上构建启动画面
 * @param scr 目标屏幕
 */
void splash_build(lv_obj_t *scr);

/**
 * @brief 按启动停留进度（0~100）更新底部加载条
 */
void splash_set_progress(uint32_t pct);

/**
 * @brief 更新进度条下方的进度项目文字（如"正在初始化传感器…"）
 *
 * 用于向用户传达当前启动阶段。传 NULL 或空串清空文字。
 */
void splash_set_progress_text(const char *text);
