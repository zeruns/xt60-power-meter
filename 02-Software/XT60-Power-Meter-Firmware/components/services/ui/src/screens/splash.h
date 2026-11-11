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
 * @brief 切换启动页文案/字体（v0.6.9 起用编进固件的极小子集，首帧即中文）
 *
 * 正常路径无需外部调用：splash_build() 会按 lvgl_port_splash_fonts_ready()
 * 自动应用一次。本函数只在运行期语言状态与当前显示不一致时由 ui.c 调用。
 * cjk=false 是兜底分支（子集创建失败，如 PSRAM 不足 / tiny_ttf 异常），
 * 改用内置 Montserrat ASCII 字体，避免中文渲染成方框。必须在 LVGL 锁内调用。
 *
 * @param cjk true=中文（嵌入子集）；false=ASCII（内置 Montserrat）
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
