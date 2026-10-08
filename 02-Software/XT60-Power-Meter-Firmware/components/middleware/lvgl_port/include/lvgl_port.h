/**
 * @file lvgl_port.h
 * @brief LVGL 与 LCD / 触摸 / 节拍的适配层
 *
 * 职责：把 lcd_nv3030b 与 touch_cst816d 注册为 LVGL 显示与输入设备，
 * 提供 LVGL 心跳，并提供线程安全的锁，供非 UI 任务访问 LVGL。
 *
 * 实现要点见 src/lvgl_port.c 文件头，其中最关键的一条：
 *   esp_lcd 的颜色数据传输是**异步**的，因此必须在
 *   lcd_nv3030b_register_flush_done_cb() 的回调里才调用
 *   lv_display_flush_ready()，否则 LVGL 会与 DMA 抢绘图缓冲导致画面错乱。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "lcd_nv3030b.h"
#include "lvgl.h"

/**
 * @brief 初始化 LVGL 适配层（注册显示/输入设备并启动心跳）
 * @return ESP_OK 成功
 */
esp_err_t lvgl_port_init(void);

/* ============================================================
 * 中文字体管理（tiny_ttf + 嵌入子集化 TTF）
 *
 * 字体数据：components/middleware/lvgl_port/fonts/notosanssc_subset.ttf
 *   由 simhei.ttf 子集化而来，仅收录本工程 UI 用到的 CJK 字符（约 25KB）。
 *   重新生成：见同目录下的 README（若有）或用 fonttools subset。
 *
 * 字号策略：在 lvgl_port_init() 里一次性创建若干常用字号的 lv_font_t，
 *   UI 层直接取用，避免每次渲染都重建字体（tiny_ttf 建字体较重）。
 * ============================================================ */

/** 常用字号枚举（按需增减） */
typedef enum {
    LVGL_PORT_FONT_12 = 0,
    LVGL_PORT_FONT_14,
    LVGL_PORT_FONT_16,
    LVGL_PORT_FONT_20,
    LVGL_PORT_FONT_24,
    LVGL_PORT_FONT_32,
    LVGL_PORT_FONT_40,
    LVGL_PORT_FONT_MAX,
} lvgl_port_font_size_t;

/**
 * @brief 获取指定字号的中文字体指针
 * @param size 字号（见 lvgl_port_font_size_t）
 * @return 字体指针；若未初始化或字号非法返回 NULL
 */
const lv_font_t *lvgl_port_get_font(lvgl_port_font_size_t size);

/**
 * @brief 切换显示旋转（同步面板方向、LVGL 分辨率与触摸坐标映射）
 *
 * 这是「触摸方向自动跟随屏幕旋转」的统一入口：内部会
 *   ① 调 lcd_nv3030b_set_rotation() 写 MADCTL；
 *   ② 更新 LVGL 显示设备的分辨率；
 *   ③ 更新触摸坐标映射表（indev 读取时按当前档位换算）。
 * 供后续 IMU 自动转屏 / 用户设置复用。
 *
 * @param rot 目标档位（见 lcd_nv3030b_rotation_t）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 适配层未初始化
 */
esp_err_t lvgl_port_set_rotation(lcd_nv3030b_rotation_t rot);

/**
 * @brief 取当前显示旋转档位
 * @return 当前档位
 */
lcd_nv3030b_rotation_t lvgl_port_get_rotation(void);

/**
 * @brief 获取 LVGL 互斥锁
 *
 * 任何非 UI 任务在调用 LVGL API 前必须先加锁，操作完成后调用
 * lvgl_port_unlock() 释放，避免与 UI 任务并发访问。
 *
 * @param timeout_ms 等待超时（毫秒），0 表示不等待
 * @return true 加锁成功；false 超时未获取到锁
 */
bool lvgl_port_lock(uint32_t timeout_ms);

/**
 * @brief 释放 LVGL 互斥锁
 */
void lvgl_port_unlock(void);
