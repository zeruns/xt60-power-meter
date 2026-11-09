/**
 * @file splash.c
 * @brief 启动画面实现（参考 RD/UM34 类桌面功率计的简洁风格）
 *
 * 版式（白底，横屏 284×240 / 竖屏 240×284 自适应）：
 *   ┌──────────────────────────────┐
 *   │                              │
 *   │        XT60 功率计           │  Bold 40px，近黑
 *   │    ────────────              │  标题下品牌色短分隔线（40px 宽）
 *   │    作者博客：blog.zeruns.com  │  16px 中灰
 *   │        固件版本 vX.Y.Z        │  14px 浅灰（取自 esp_app_desc）
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
 * 进度条语义（v0.6.9 起）：进度来自 app_init 的真实初始化进度——
 * app_init 在显示链路就绪后提前创建 UI 任务并显示本页，之后每执行一个
 * 初始化步骤/自检项就通过 ui_boot_set_progress() 更新一次快照；
 * ui.c 的启动页循环读取快照并调用本文件的 setter 渲染。
 * 进度跑完后若存在外设故障（初始化失败或自检未通过），ui.c 会弹故障
 * 提示框列出不可用功能（结论不在本页呈现，本页只负责"跑条 + 阶段文字"）。
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
static lv_obj_t *s_brand_lbl = NULL;     /**< 顶部英文小标识 */
static lv_obj_t *s_title_lbl = NULL;     /**< 主标题（ASCII/中文两套文案） */
static lv_obj_t *s_sub_lbl = NULL;       /**< 作者博客行 */
static lv_obj_t *s_ver_lbl = NULL;       /**< 固件版本行 */
static bool s_cjk = false;               /**< 当前启动页是否已切换为中文文案 */

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
 * @brief 切换启动页文案语言（ASCII 内置字体 / 中文嵌入子集）
 *
 * v0.6.9：启动页改用**编进固件的极小子集**（splash_font_data.c，约 190 字），
 * lvgl_port_app_init() 内同步创建，因此启动页首帧就是完整中文，不再经历
 * "先 ASCII、等 4MB 全量字库加载完再切中文"的过程。
 *
 * 仍保留 ASCII 分支作为兜底：仅当子集创建失败（PSRAM 不足/tiny_ttf 异常）时
 * 才回退，避免中文渲染成方框。所有文本与字体必须成对切换——只换文案
 * 不换字体会出现中文方框，只换字体不换文案则排版偏小。
 *
 * @param cjk true=中文（嵌入子集）；false=ASCII（内置 Montserrat）
 */
static void splash_apply_language(bool cjk)
{
    if (s_brand_lbl == NULL || s_title_lbl == NULL || s_sub_lbl == NULL || s_ver_lbl == NULL) {
        return; /* 尚未构建（splash_build 之前调用）：忽略 */
    }

    const lv_font_t *f14 = &lv_font_montserrat_14;
    lv_obj_set_style_text_font(s_brand_lbl,
                               cjk ? lvgl_port_get_splash_font(LVGL_PORT_FONT_12) : f14, 0);
    lv_obj_set_style_text_font(s_title_lbl,
                               cjk ? lvgl_port_get_splash_font_bold(LVGL_PORT_FONT_40)
                                   : &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_font(s_sub_lbl,
                               cjk ? lvgl_port_get_splash_font(LVGL_PORT_FONT_16) : f14, 0);
    lv_obj_set_style_text_font(s_ver_lbl,
                               cjk ? lvgl_port_get_splash_font(LVGL_PORT_FONT_14) : f14, 0);
    if (s_progress_lbl != NULL) {
        lv_obj_set_style_text_font(s_progress_lbl,
                                   cjk ? lvgl_port_get_splash_font(LVGL_PORT_FONT_14) : f14, 0);
    }

    lv_label_set_text(s_brand_lbl,
                      cjk ? "X T 6 0   P O W E R   M E T E R" : "XT60 POWER METER");
    lv_label_set_text(s_title_lbl, cjk ? "XT60 功率计" : "XT60 METER");
    lv_label_set_text(s_sub_lbl, cjk ? "作者博客：blog.zeruns.com" : "blog.zeruns.com");

    char ver_text[48];
    lv_snprintf(ver_text, sizeof(ver_text), cjk ? "固件版本 v%s" : "Firmware v%s",
                esp_app_get_description()->version);
    lv_label_set_text(s_ver_lbl, ver_text);

    s_cjk = cjk;
}

void splash_set_cjk(bool cjk)
{
    if (s_cjk != cjk) {
        splash_apply_language(cjk);
    }
}

/**
 * @brief 在当前活动屏幕上构建启动画面（居中文字列 + 加载条 + 阶段文字）
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

    /* 英文小标识（蓝色大写字母间距感由空格模拟）
     * 中文字体由 splash_apply_language 统一换成嵌入子集（首帧即完整中文）。 */
    s_brand_lbl = make_label(cont, "XT60 POWER METER", &lv_font_montserrat_14, COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(s_brand_lbl, 1, 0); /* LVGL：字符间距 +1px，增强标识感 */
    lv_obj_set_style_pad_bottom(s_brand_lbl, 6, 0); /* 与主标题之间再多留 6px */

    /* 主标题：中文子集就绪时用 Bold 40px（见 splash_apply_language） */
    s_title_lbl = make_label(cont, "XT60 METER", &lv_font_montserrat_32, COLOR_TITLE);

    /* 标题下品牌色短分隔线（40×3px 圆头）：替代旧顶部通栏的品牌识别职责，
     * 居中由父容器 flex 交叉轴居中保证。 */
    lv_obj_t *rule = lv_obj_create(cont);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 40, 3);
    lv_obj_set_style_bg_color(rule, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rule, 2, 0);
    lv_obj_set_scrollable(rule, false);

    s_sub_lbl = make_label(cont, "blog.zeruns.com", &lv_font_montserrat_14, COLOR_SUB);

    /* 版本号不写死：esp_app_get_description() 是 ESP-IDF 接口，
     * 返回编译进固件的应用描述（版本号取自项目版本配置），
     * 每次发版不用改 UI 代码。文案由 splash_apply_language 填。 */
    s_ver_lbl = make_label(cont, "", &lv_font_montserrat_14, COLOR_VER);

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
    lv_obj_set_style_text_font(s_progress_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_progress_lbl, COLOR_SUB, 0);
    lv_obj_set_width(s_progress_lbl, w * 90 / 100); /* 预留 90% 屏宽，够放下最长阶段文字 */
    lv_obj_set_style_text_align(s_progress_lbl, LV_TEXT_ALIGN_CENTER, 0); /* LVGL：文本居中 */
    lv_obj_set_style_pad_top(s_progress_lbl, 6, 0); /* 与加载条之间留 6px */

    /* 构建完成后按嵌入子集状态统一应用文案/字体：
     * 子集随固件启动同步创建，正常情况下首帧即为完整中文；
     * 仅子集创建失败时保持 ASCII（splash_set_cjk(false) 兜底）。 */
    splash_apply_language(lvgl_port_splash_fonts_ready());
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
