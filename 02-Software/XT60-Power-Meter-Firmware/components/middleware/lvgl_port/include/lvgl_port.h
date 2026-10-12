/**
 * @file lvgl_port.h
 * @brief 应用侧 LVGL 适配层（基于 ESP 官方组件 espressif/esp_lvgl_port）
 *
 * ─────────────────────────────────────────────────────────────
 * 相对早期“自研 + vendor LVGL”方案的变化
 * ─────────────────────────────────────────────────────────────
 * 1. 图形库 LVGL 与移植层 esp_lvgl_port 均**不再 vendor 入库**，改由 ESP 官方
 *    组件管理器托管（见本组件 idf_component.yml 与工程根 dependencies.lock）；
 * 2. LVGL 内核初始化（lv_init）、心跳定时器（lv_tick_inc）、LVGL 任务
 *    （循环调用 lv_timer_handler）与访问互斥锁，全部交由官方 esp_lvgl_port 负责。
 *    本头文件直接 `#include "esp_lvgl_port.h"`，因此 `lvgl_port_lock() /
 *    lvgl_port_unlock()` 等即为**官方实现**，调用方无需改动。
 * 3. 本组件只保留“应用特有”的部分：
 *      · 中文字体：SPIFFS 上的 Noto Sans SC 子集，启动时整读入 PSRAM 后用
 *        tiny_ttf 建多档字号（见 lvgl_port.c 文件头说明，运行期零 flash 访问）；
 *      · 显示注册：复用官方 lvgl_port_add_disp()，但以 `sw_rotate=1` 保护面板
 *        由 MADCTL 完成的“硬旋转”（否则官方会调用本面板不支持的 swap_xy
 *        并改写 MADCTL，导致镜像/花屏）；
 *      · 首帧计数：供 UI 层在“首帧真正渲染后”再打开背光，避免开机先见黑屏；
 *      · 旋转档位：封装面板 MADCTL + LVGL 分辨率 + 触摸坐标映射三者同步；
 *      · 电容触摸输入：由本组件自行注册 LVGL indev（见下方命名冲突说明）。
 *
 * ─────────────────────────────────────────────────────────────
 * 为什么触摸不用官方的 lvgl_port_add_touch
 * ─────────────────────────────────────────────────────────────
 * 官方 lvgl_port_add_touch() 要求传入 esp_lcd_touch 句柄（即需要引入
 * esp_lcd_touch 组件并改用其驱动）。本工程使用的是自研 CST816D I2C 驱动
 * （touch_cst816d，含上板实测标定的旋转坐标映射），故自行创建 indev，
 * 复用自有驱动读取触摸点。显示链路仍完全走官方移植层。
 *
 * ─────────────────────────────────────────────────────────────
 * 函数命名说明（避免与官方符号冲突）
 * ─────────────────────────────────────────────────────────────
 * 官方已占用 `lvgl_port_init / lvgl_port_lock / lvgl_port_unlock /
 * lvgl_port_add_disp / lvgl_port_flush_ready ...` 等名字。因此本组件的
 * 初始化入口命名为 `lvgl_port_app_init()`（而非 lvgl_port_init），以免与
 * 官方声明/定义冲突。其余自定义函数（字体/旋转/计数）官方未占用，保持原名。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#include "esp_lvgl_port.h" /* 官方移植层：lvgl_port_init/lock/unlock、lvgl_port_add_disp */
#include "lcd_nv3030b.h"

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
 * @brief 初始化应用侧 LVGL 适配层
 *
 * 内部流程：
 *   ① 调官方 lvgl_port_init()：初始化 LVGL、创建 LVGL 任务（绑定 Core 0）
 *      与心跳定时器；
 *   ② 加载中文字体（SPIFFS → PSRAM → tiny_ttf，多档字号）；
 *   ③ 调官方 lvgl_port_add_disp() 注册显示（由官方分配缓冲并接管传输完成回调）；
 *   ④ 用中文字体重建默认主题（官方控件默认字体改为中文）；
 *   ⑤ 注册电容触摸输入设备（自研驱动 + 旋转映射）。
 *
 * 前置条件：fs_storage（字体所在分区）与 lcd_nv3030b / touch_cst816d 已初始化。
 *
 * @return ESP_OK 成功；其余为错误码
 */
esp_err_t lvgl_port_app_init(void);

/**
 * @brief 获取指定字号的常规字重中文字体
 * @param size 字号（见 lvgl_port_font_size_t）
 * @return 字体指针；文件字体不可用时回退内置 ASCII 字体，不返回 NULL（参数非法除外）
 */
const lv_font_t *lvgl_port_get_font(lvgl_port_font_size_t size);

/**
 * @brief 获取指定字号的粗体中文字体（标题/大数值用）
 * @param size 字号
 * @return 字体指针；回退规则同 lvgl_port_get_font()
 */
const lv_font_t *lvgl_port_get_font_bold(lvgl_port_font_size_t size);

/**
 * @brief 查询显示器已完成上屏的 flush 次数
 *
 * 用途：开机时背光保持关闭，UI 任务在「首帧真正渲染」后再打开背光，
 * 这样用户看到的第一帧就是完整的启动画面，不会先看到黑屏或局部条带。
 *
 * @return 累计完成渲染的次数（单调递增）
 */
uint32_t lvgl_port_flush_count(void);

/**
 * @brief 切换显示旋转（同步面板方向、LVGL 分辨率与触摸坐标映射）
 *
 * 这是「触摸方向自动跟随屏幕旋转」的统一入口：内部会
 *   ① 调 lcd_nv3030b_set_rotation() 写 MADCTL；
 *   ② 记录档位并更新触摸映射；
 *   ③ 更新 LVGL 显示设备的分辨率。
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
