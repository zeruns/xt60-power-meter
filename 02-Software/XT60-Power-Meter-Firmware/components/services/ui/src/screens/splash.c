/**
 * @file splash.c
 * @brief 启动画面实现（参考 RD/UM34 类桌面功率计的简洁风格）
 *
 * 版式（白底，横屏 284×240 / 竖屏 240×284 自适应）：
 *   ┌──────────────────────────────┐
 *   │                              │
 *   │        XT60 功率计           │  Bold 40px，近黑
 *   │    ────────────              │  标题下品牌色短分隔线（40px 宽）
 *   │      作者博客：blog.zeruns    │  16px 中灰
 *   │        固件版本 v0.1.0        │  14px 浅灰
 *   │       ▓▓▓▓▓▓▓░░░░░            │  加载条（品牌色，屏宽 80%）
 *   │      正在初始化传感器…         │  阶段文字（14px 中灰）
 *   └──────────────────────────────┘
 *
 * 【v0.6.1 版式调整】去掉旧版"顶部 4px 品牌蓝通栏"——用户反馈观感突兀
 * （像系统残留的状态条）；品牌色改由「英文小标识 + 标题下短分隔线 +
 * 加载条填充段」三处呼应，整体更简洁。
 *
 * 布局策略：文字块 + 加载条全部放进同一 flex 列、整体居中。
 * 加载条是列内最后一个子项，天然水平居中且恒在版本行下方——
 * 横竖屏都自适应（旧版加载条用绝对坐标按 284 宽写死，竖屏时
 * 既不居中又压住版本行）。
 *
 * 进度条语义（初学者注意）：进度是"启动停留时间"的比例，不是真实
 * 初始化进度——真实初始化在 app_init 阶段已完成，ui.c 在 1800ms 停留期
 * 内按 0~100% 推进进度条并切换 4 段阶段文字（正在初始化系统…/正在加载字库…/
 * 正在初始化传感器…/正在准备界面…），给用户"系统在启动"的阶段感。
 * 进度跑完后若外设自检有失败项，ui.c 会弹故障提示框列出不可用功能
 * （自检结论不在本页呈现，本页只负责"跑条 + 阶段文字"）。
 *
 * 本文件只做静态版式 + 两个 setter（进度值/阶段文字），由 ui.c 的启动
 * 循环反复调用；构建一次（splash_build）后不再重建控件，只改文字和进度。
 */
#include "splash.h"

#include "esp_app_desc.h" /* ESP-IDF：读取固件版本等应用描述信息 */
#include "lvgl.h"
#include "lvgl_port.h"

/* 品牌与文字配色 */
#define COLOR_ACCENT  lv_color_hex(0x0066CC) /**< 品牌蓝（通栏/加载条/小标识） */
#define COLOR_TITLE   lv_color_hex(0x1A1A1A) /**< 主标题近黑 */
#define COLOR_SUB     lv_color_hex(0x666666) /**< 副标题/阶段文字中灰 */
#define COLOR_VER     lv_color_hex(0x999999) /**< 版本行浅灰（信息弱化） */
#define COLOR_BAR_BG  lv_color_hex(0xE3E7EC) /**< 加载条未走完部分的轨道底色 */

/** 加载条宽度占比（相对屏宽）、高度 */
#define BAR_WIDTH_PCT 80
#define BAR_H 6

static lv_obj_t *s_bar = NULL;         /**< 进度条控件（lv_bar） */
static lv_obj_t *s_progress_lbl = NULL;  /**< 进度条下方的阶段文字 */

/**
 * @brief 造一个标签的快捷封装（创建 + 设文本 + 设字体 + 设颜色）
 * @param parent 父容器
 * @param text   初始文本
 * @param font   字体（来自 lvgl_port_get_font()）
 * @param color  文字颜色
 * @return 标签对象指针
 */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent); /* LVGL：新建文本标签 */
    lv_label_set_text(lbl, text);            /* LVGL：设置标签文本 */
    lv_obj_set_style_text_font(lbl, font, 0); /* LVGL：设字体（part=0 即默认主体） */
    lv_obj_set_style_text_color(lbl, color, 0); /* LVGL：设文字颜色 */
    return lbl;
}

/**
 * @brief 在当前活动屏幕上构建启动画面（通栏 + 居中文字列 + 加载条 + 阶段文字）
 *
 * 调用时机：ui.c 启动流程最开始，在 LVGL 锁内调用一次；之后仅通过
 * splash_set_progress()/splash_set_progress_text() 更新内容。
 *
 * @param scr 目标屏幕（lv_screen_active()）
 */
void splash_build(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0); /* 白底 */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false); /* 启动页固定不滚动 */

    /* 当前逻辑分辨率（旋转后重建时横竖屏各不同） */
    lv_display_t *d = lv_display_get_default(); /* LVGL：取默认显示器 */
    /* LVGL：读逻辑宽度，旋转后会变（284/240），加载条宽度据此算而非写死 */
    const int32_t w = (d != NULL) ? lv_display_get_horizontal_resolution(d) : 284;

    /* ① 顶部通栏已移除（v0.6.1）：品牌色改由小标识/分隔线/加载条承担，
     *    此处不再创建 topbar 对象。 */

    /* ② 纵向布局：小标识 + 标题 + 副标题 + 版本 + 加载条（整体居中）。
     * 容器铺满整屏、flex 三轴居中：内容高度变化时（横竖屏不同）自动保持
     * 垂直居中，无需在加完子项后重算坐标。 */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100)); /* 铺满整屏 */
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN); /* LVGL：flex 纵排，子项自上而下 */
    /* LVGL：flex 主轴（竖直）与两个交叉轴都居中，整块内容在屏上四面居中 */
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 8, 0); /* LVGL：flex 子项之间行距 8px */
    lv_obj_set_scrollable(cont, false);

    /* 英文小标识（蓝色大写字母间距感由空格模拟） */
    lv_obj_t *brand = make_label(cont, "X T 6 0   P O W E R   M E T E R",
                                 lvgl_port_get_font(LVGL_PORT_FONT_12), COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(brand, 1, 0); /* LVGL：字符间距 +1px，增强标识感 */
    lv_obj_set_style_pad_bottom(brand, 6, 0); /* 与主标题之间再多留 6px */

    /* 主标题：Bold 40px（字体文件不含时自动回退常规字重，不会方框） */
    make_label(cont, "XT60 功率计", lvgl_port_get_font_bold(LVGL_PORT_FONT_40), COLOR_TITLE);

    /* 标题下品牌色短分隔线（40×3px 圆头）：替代旧顶部通栏的品牌识别职责，
     * 居中由父容器 flex 交叉轴居中保证。 */
    lv_obj_t *rule = lv_obj_create(cont);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 40, 3);
    lv_obj_set_style_bg_color(rule, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rule, 2, 0);
    lv_obj_set_scrollable(rule, false);

    make_label(cont, "作者博客：blog.zeruns.com", lvgl_port_get_font(LVGL_PORT_FONT_16),
               COLOR_SUB);

    /* 版本号不写死：esp_app_get_description() 是 ESP-IDF 接口，
     * 返回编译进固件的应用描述（版本号取自项目版本配置），
     * 每次发版不用改 UI 代码。lv_snprintf 是 LVGL 自带的安全格式化。 */
    char ver_text[48];
    lv_snprintf(ver_text, sizeof(ver_text), "固件版本 v%s", esp_app_get_description()->version);
    make_label(cont, ver_text, lvgl_port_get_font(LVGL_PORT_FONT_14), COLOR_VER);

    /* ③ 加载条：列内最后一项（宽 = 屏宽 80%，高 6px，圆角胶囊） */
    s_bar = lv_bar_create(cont); /* LVGL：新建进度条控件 */
    lv_obj_set_size(s_bar, w * BAR_WIDTH_PCT / 100, BAR_H);
    lv_bar_set_range(s_bar, 0, 100); /* LVGL：进度条取值范围 0~100 */
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF); /* LVGL：设当前值；LV_ANIM_OFF=数值跳变不做补间 */
    lv_obj_set_style_bg_color(s_bar, COLOR_BAR_BG, 0); /* 轨道底色（未走完部分） */
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar, 3, 0); /* 轨道圆角（半高，成胶囊） */
    /* v9：bar 填充条是 INDICATOR 部件，用 bg_color/radius 样式控制 */
    lv_obj_set_style_bg_color(s_bar, COLOR_ACCENT, LV_PART_INDICATOR); /* 填充段品牌蓝 */
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_scrollable(s_bar, false);

    /* ④ 进度条下方的阶段文字（14px 浅灰，居中，单行）。
     * 初始为空，由 ui.c 在启动各阶段调用 splash_set_progress_text() 更新。 */
    s_progress_lbl = lv_label_create(cont);
    lv_label_set_text(s_progress_lbl, "");
    lv_obj_set_style_text_font(s_progress_lbl, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_progress_lbl, COLOR_SUB, 0);
    lv_obj_set_width(s_progress_lbl, w * 90 / 100); /* 预留 90% 屏宽，够放下最长阶段文字 */
    lv_obj_set_style_text_align(s_progress_lbl, LV_TEXT_ALIGN_CENTER, 0); /* LVGL：文本居中 */
    lv_obj_set_style_pad_top(s_progress_lbl, 6, 0); /* 与加载条之间留 6px */
}

/**
 * @brief 更新加载条进度
 * @param pct 进度百分比 0~100（超过 100 会被截断到 100）
 */
void splash_set_progress(uint32_t pct)
{
    if (s_bar != NULL) {
        if (pct > 100) {
            pct = 100;
        }
        lv_bar_set_value(s_bar, (int32_t)pct, LV_ANIM_OFF); /* LVGL：写进度值，不加补间动画 */
    }
}

/**
 * @brief 更新进度条下方的阶段文字
 * @param text 阶段文字（如"正在初始化传感器…"）；NULL 或空串清空
 */
void splash_set_progress_text(const char *text)
{
    if (s_progress_lbl != NULL) {
        lv_label_set_text(s_progress_lbl, (text != NULL) ? text : ""); /* 防御 NULL 指针 */
    }
}
