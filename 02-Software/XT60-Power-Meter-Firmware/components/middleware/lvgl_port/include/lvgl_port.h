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
 *      · 中文字体：启动页用编进固件的极小子集（splash_font_data.c，首帧即
 *        完整中文）；主界面用 SPIFFS 上的全量 Noto Sans SC 子集，由后台
 *        任务整读入 PSRAM 后用 tiny_ttf 建多档字号（运行期零 flash 访问）；
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
    LVGL_PORT_FONT_36, /* 首页大数字用（36px：4 行 44px 行高内的可读性/宽度平衡点） */
    LVGL_PORT_FONT_40,
    LVGL_PORT_FONT_MAX,
} lvgl_port_font_size_t;

/**
 * @brief 初始化应用侧 LVGL 适配层
 *
 * 内部流程：
 *   ① 调官方 lvgl_port_init()：初始化 LVGL、创建 LVGL 任务（绑定 Core 0）
 *      与心跳定时器；
 *   ② 同步创建启动页嵌入子集字体（不读 SPIFFS，几十毫秒）；
 *   ③ 调官方 lvgl_port_add_disp() 注册显示（由官方分配缓冲并接管传输完成回调）；
 *   ④ 注册电容触摸输入设备（自研驱动 + 旋转映射）。
 * 全量中文字体由 lvgl_port_start_font_load() 在 app_tasks_start() 里后台加载，
 * 加载完成后重建默认主题，主界面再使用全量 Noto 字体。
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
 * @brief 获取启动页专用的嵌入子集字体（v0.6.9）
 *
 * 子集只有约 190 个字形（启动页文案 + 初始化进度标签），编进 app 分区，
 * lvgl_port_app_init() 内同步创建，**不依赖 SPIFFS 与后台字库任务**；
 * 因此启动页第一帧就能显示完整中文，不必先等 4MB 全量字库。
 *
 * 子集未就绪或该字号缺失时自动回退到全量字体链（最终回退 Montserrat），
 * 不返回 NULL（参数非法除外）。
 *
 * @param size 字号：启动页用到 12（标识）/14（版本/进度）/16（副标题），
 *             粗体 40（主标题）
 * @return 字体指针
 */
const lv_font_t *lvgl_port_get_splash_font(lvgl_port_font_size_t size);

/** @brief 获取启动页专用子集的粗体（主标题 40px） */
const lv_font_t *lvgl_port_get_splash_font_bold(lvgl_port_font_size_t size);

/**
 * @brief 启动页嵌入子集是否就绪
 *
 * 正常情况下 lvgl_port_app_init() 返回时即为 true（同步创建，耗时几十毫秒）；
 * 只有 PSRAM 分配失败或 tiny_ttf 创建失败时才为 false，此时启动页回退
 * ASCII 文案（由 ui.c 判断后调用 splash_set_cjk(false)）。
 */
bool lvgl_port_splash_fonts_ready(void);

/**
 * @brief 中文字体是否已异步加载完成（v0.6.9）
 *
 * lvgl_port_app_init() 现在只创建后台字库任务并立即返回；在此之前
 * lvgl_port_get_font() 全部回退内置 Montserrat（仅 ASCII）。
 * 启动页据此选择「ASCII 文案」或「中文文案」，主界面也应等本函数返回
 * true 后再构建，避免中文显示成方框。
 *
 * @return true=中文字体已就绪（或加载失败已降级）；false=仍在加载
 */
bool lvgl_port_fonts_ready(void);

/**
 * @brief 启动中文字体后台加载任务（v0.6.9）
 *
 * 由 app_tasks_start() 在显示链路确认无误后调用：
 *   · 此时启动页已经显示（ASCII），初始化也已完成，字体加载不再拖慢
 *     屏幕点亮与测量任务启动；
 *   · 加载完成后 lvgl_port_fonts_ready() 返回 true，启动页切中文、
 *     UI 任务随后构建主界面。
 *
 * @return ESP_OK 已启动/或此前已启动；ESP_ERR_INVALID_STATE 显示未就绪；
 *         ESP_ERR_NO_MEM 任务创建失败（此时自动降级为 ASCII 字体）
 */
esp_err_t lvgl_port_start_font_load(void);

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

/**
 * @brief 屏幕角度（用户语义）：以默认横屏为 0°，顺时针 90/180/270°
 *
 * 与面板枚举的对应（MIPI/TFT_eSPI 旋转约定，与校准过的 0° 触摸映射自洽）：
 *   0° → ROT_90（出厂默认横屏）/ 90° → ROT_180（竖屏）/
 *   180° → ROT_270（横屏倒）/ 270° → ROT_0（竖屏倒）。
 * 非 0/90/180/270 的入参返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t lvgl_port_set_rotation_angle(uint16_t deg);
uint16_t lvgl_port_get_rotation_angle(void);
