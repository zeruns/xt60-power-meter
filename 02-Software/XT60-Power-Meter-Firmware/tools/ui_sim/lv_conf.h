/**
 * @file lv_conf.h
 * @brief PC 模拟器（tools/ui_sim）的 LVGL 配置
 *
 * 目标：与真机（ESP-IDF Kconfig 生成的配置，见工程根 sdkconfig）保持一致的
 * 关键渲染参数 —— 这样同一份 UI 代码在模拟器与真机上的渲染结果才可对齐：
 *
 * 真机 sdkconfig 值                     本文件对应
 *   CONFIG_LV_COLOR_DEPTH=16              LV_COLOR_FORMAT_DEFAULT = RGB565
 *                                         （LVGL 9 已改用 LV_COLOR_FORMAT_DEFAULT，
 *                                          LV_COLOR_DEPTH 废弃、将在 v10 移除）
 *   CONFIG_LV_USE_STDLIB_MALLOC=1         LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
 *                                         （C 库 malloc；真机未启用内建内存池，
 *                                          因此 sdkconfig 里没有 LV_MEM_SIZE）
 *   CONFIG_LV_USE_OS=0                    LV_OS_NONE
 *   CONFIG_LV_USE_TINY_TTF=y              LV_USE_TINY_TTF 1
 *                                         （字形缓存 64/16；真机为 32/16，
 *                                          仅影响缓存容量，不改变字形）
 *   CONFIG_LV_FONT_MONTSERRAT_14=y        LV_FONT_MONTSERRAT_14 1（图标回退）
 *   CONFIG_LV_DRAW_SW_COMPLEX=y           LV_DRAW_SW_COMPLEX 1
 *   CONFIG_LV_DRAW_LAYER_SIMPLE_BUF_SIZE  LV_DRAW_LAYER_SIMPLE_BUF_SIZE 24KB
 *   CONFIG_LV_DRAW_BUF_STRIDE_ALIGN=1     LV_DRAW_BUF_STRIDE_ALIGN 1
 *   CONFIG_LV_DRAW_BUF_ALIGN=4            LV_DRAW_BUF_ALIGN 4
 *
 * 未在此列出的项走 LVGL 默认值；与真机不同的小差异已在对应条目里注明。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ---- 颜色与内存 ----
 * 注意：LVGL 9 已改用 LV_COLOR_FORMAT_DEFAULT，LV_COLOR_DEPTH 废弃（定义它会让
 * lv_conf_internal.h 发出 `#warning`，而 MSVC 不支持该指令、直接报 C1021 致命错误），
 * 统一改用 LV_COLOR_FORMAT_DEFAULT。真机 Kconfig 的 CONFIG_LV_COLOR_DEPTH=16
 * 由 ESP-IDF 侧转换，与此处配置互相独立。 */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565
/* ---- 内存分配 ----
 * 真机 sdkconfig 为 CONFIG_LV_USE_STDLIB_MALLOC=1（即 LV_STDLIB_CLIB）：
 * malloc/free 走 C 库堆，没有 LVGL 内建内存池、也没有固定 128KB 上限。
 * 模拟器保持一致（CLIB），因此"整套场景连续渲染（数十张截图）+ 多档字号 tiny_ttf
 * 字形缓存"不会因小内存池耗尽而失败；分配器来源不同也不影响渲染像素。
 * 唯一差异：真机字符串函数用 LVGL 内建实现（CONFIG_LV_USE_BUILTIN_STRING=y），
 * 这里换用 CRT（语义等价）。 */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
/* 仅作占位：CLIB 模式下 LVGL 不引用本宏（真机同样未设置 LV_MEM_SIZE） */
#define LV_MEM_SIZE (128 * 1024)

/* ---- 单线程（模拟器无需 RTOS；真机同样是 LV_USE_OS=0） ---- */
#define LV_USE_OS LV_OS_NONE

/* ---- 绘制缓冲对齐（对齐真机 CONFIG_LV_DRAW_BUF_*） ---- */
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (24 * 1024)
#define LV_DRAW_SW_COMPLEX 1

/* ---- 字体 ---- */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_32 1 /* 启动页 ASCII 模式大标题（与固件 sdkconfig 同步） */
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_CACHE_GLYPH_CNT 64
#define LV_TINY_TTF_CACHE_KERNING_CNT 16

/* ---- 日志（只输出 WARN 及以上，避免连续跑整套场景时刷屏） ---- */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* ---- 关闭与真机无关的模块 ---- */
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_MUSIC 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_FLEX_LAYOUT 0
#define LV_USE_DEMO_MULTILANG 0
#define LV_USE_DEMO_TRANSFORM 0
#define LV_USE_DEMO_SCROLL 0
#define LV_USE_DEMO_RENDER 0
#define LV_USE_DEMO_VECTOR_GRAPHIC 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/* ---- 第三方库：仅保留 tiny_ttf 所需，其余关闭 ---- */
#define LV_USE_FREETYPE 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_LZ4_INTERNAL 0
#define LV_USE_BMP 0
#define LV_USE_PNG 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0
#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_TJPGD 0
#define LV_USE_NEMA_GFX 0

#endif /* LV_CONF_H */