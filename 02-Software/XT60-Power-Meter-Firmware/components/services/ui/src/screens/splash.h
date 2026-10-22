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
 * @brief 切换启动页文案/字体（v0.6.9 字体异步加载）
 *
 * 字体未就绪时启动页显示内置 ASCII 文案；中文字库加载完成后由 ui.c
 * 调用本函数切换为中文文案与 Noto 字体。必须在 LVGL 锁内调用。
 *
 * @param cjk true=中文；false=ASCII
 */
void splash_set_cjk(bool cjk);

/**
 * @brief 按真实初始化进度（0~100）更新底部加载条
 */
void splash_set_progress(uint32_t pct);

/**
 * @brief 更新进度条下方的进度项目文字（如"测量前端 17/23"）
 *
 * 用于向用户传达当前启动阶段。传 NULL 或空串清空文字。
 */
void splash_set_progress_text(const char *text);
