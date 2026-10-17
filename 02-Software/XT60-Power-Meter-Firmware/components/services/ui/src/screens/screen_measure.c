/**
 * @file screen_measure.c
 * @brief 测量数据页：四行满宽彩色条（参考桌面 USB 表的经典版式）
 *
 * 版式（284×240，面板四角圆角 R≈22）：
 *   ┌ x=6 ───────────────────── x=277 ┐
 *   │ 00:34:38    •••    (WiFi) 88%  │ 状态栏（ui_nav 共享浮层，22px 高）
 *   │┌──────────────────────────────┐│ y=22  高 44
 *   ││ 19.97670                    V││ 电压：绿底
 *   │└──────────────────────────────┘│
 *   │┌──────────────────────────────┐│ y=68
 *   ││ 0.01886                   ▶A ││ 电流：蓝底（箭头示方向）
 *   │└──────────────────────────────┘│
 *   │┌──────────────────────────────┐│ y=114
 *   ││ 0.37683                     W││ 功率：红底
 *   │└──────────────────────────────┘│
 *   │┌──────────────────────────────┐│ y=160
 *   ││ 0.11800                    Wh││ 电能：紫底（容量 Ah 见小字第 2 行）
 *   │└──────────────────────────────┘│
 *   │   V 14.10/13.88  I 2.05/0.12   │ 峰谷小字行 1（V/I，斜杠前峰后谷）
 *   │  P 28.9/1.6  12.34Ah  32.5℃…  │ 小字行 2：P 峰谷+容量+温湿度
 *   └────────────────────────────────┘
 *   （告警横幅：alert_mask 非 0 时顶部红条 "⚠ 过压 过流"，覆盖状态栏下沿）
 *
 * 设计说明：
 *   · 数字用 36px Bold 白字填满 44px 行高（"贴近容器大小"，远看可读）；
 *     竖屏（90°/270°）多出的垂直空间用于加大到 40px Bold / 54px 行高；
 *   · 行左右只留 6px：四行都落在垂直中段，该区域内面板
 *     圆角不侵入，因此可以像成品表一样做到近满宽；
 *   · 单位小字（Bold 16）贴右内边距，电流行单位左侧再放方向箭头；
 *     箭头方向随屏幕角度自适应（0°→右 / 90°→下 / 180°→左 / 270°→上），
 *     负电流取反向——电流物理流向固定，屏幕转了箭头跟着转；
 *   · 动态小数位（≤9 字符）：|v|<1 用 5 位、<100 用 4 位、<10000 用 2 位；
 *     40px Bold 下最宽约 213px（极端 8 字符 182px），加单位/箭头不撞行宽；
 *   · K2 长按打开"快速菜单"（全程按键可操作：清除电能统计 / 刷新率 1~30Hz）。
 *
 * 刷新与交互的关键设计（初学者阅读本文件前先看这里）：
 *   · **数据刷新率与波形页相互独立**：本页刷新率（1/5/10/20/30Hz）由快速菜单
 *     设置，只影响四行大数字的更新频率，波形页固定 25Hz 不受影响（见 ui.c）；
 *     低档位时 ui.c 会对采样做滑动平均，读数更稳、更省 CPU/刷屏；
 *   · **纯按键可操作**：触屏只是"加速路径"，所有功能都能用 K1/K2 完成
 *     （长按开/关菜单、单击移焦点、双击执行），适配不带触摸的使用场景；
 *   · **操作完成自动关闭子菜单**：执行"清除电能统计"后菜单自动收起，
 *     减少一次手动退出的操作；
 *   · 字体统一走 lvgl_port_get_font()（本项目按 12~40px 共 7 档预生成
 *     Noto Sans SC tiny_ttf），不用 LVGL 内置字体，保证中英文风格一致。
 *
 * 页面生命周期：ui_nav 负责创建/刷新本页，屏幕旋转后 ui_nav 会销毁并重建
 * 本页（create 时重新取几何），因此本文件大量使用文件级 static 保存控件指针。
 */
#include "screen_measure.h"

#include <math.h>
#include <stdio.h>

#include "lvgl.h"
#include "lvgl_port.h"

#include "buzzer.h"
#include "power_measure.h"
#include "ui.h"

/* 四行配色（白字底）：与波形页同数字项的颜色语义保持一致 */
#define COL_V  lv_color_hex(0x1E9E4F) /* 电压 绿 */
#define COL_I  lv_color_hex(0x1B6FD6) /* 电流 蓝 */
#define COL_P  lv_color_hex(0xD0392B) /* 功率 红 */
#define COL_E  lv_color_hex(0x7D3CB5) /* 电能 紫 */
#define COL_ENV lv_color_hex(0x6B7280) /**< 环境行灰字 */
#define COL_ALERT lv_color_hex(0xD0392B) /**< 告警横幅红底（与功率红同色系） */
/* 快速菜单用色（与设置页同语义） */
#define COL_PRIMARY lv_color_hex(0x0066CC) /**< 品牌蓝：焦点边框 / 当前档位 */
#define COL_INK     lv_color_hex(0x1A1A1A) /**< 近黑正文 */
#define COL_CARD    lv_color_hex(0xF1F4F8) /* 卡片底：与白底拉开可辨差异 */
#define COL_BORDER  lv_color_hex(0xB4BEC9) /* 边框：约 2.2:1 对比度，边界清晰 */

/** 内容左右边距：四行都在垂直中段，圆角不侵入，故可做到近满宽 */
#define SAFE_X 6
#define ROW_GAP 2 /**< 相邻数据行间距（px） */
/** 行内左右内边距 */
#define ROW_PAD 8

/**
 * @brief 当前分辨率下的布局几何（横/竖屏各一套）
 *
 * 横屏 284×240 的取值为历史调优值（逐像素保持）；竖屏 240×284 时
 * 多出的垂直空间用于加大行高与数字字号（用户需求：竖屏底部空间大），
 * 环境行跟随，快速菜单的档位按钮收窄以放进 240 宽。
 * 旋转后 ui_nav 会重建本页，create 时重新取几何。
 */
typedef struct {
    int32_t w;            /**< 逻辑宽 */
    int32_t h;            /**< 逻辑高 */
    int32_t row_top;      /**< 第一行顶部 y */
    int32_t row_h;        /**< 数据行高 */
    int32_t row_gap;      /**< 行间距 */
    int32_t peak_y;       /**< 底部两行小字（峰值/容量 + 环境）起始 y */
    int32_t rate_btn_w;   /**< 快速菜单刷新率档位按钮宽 */
    lvgl_port_font_size_t val_font; /**< 大数字字号 */
} measure_geom_t;

/**
 * @brief 按当前屏幕方向计算布局几何
 *
 * 为什么放一个函数而不是写死常量：横竖屏共用同一套创建代码，
 * 每次重建页面时现取分辨率即可，旋转后无需另写一份竖屏布局。
 * @return 包含行高/字号等几何量的结构体（按宽高比自动选横/竖屏套值）
 */
static measure_geom_t measure_geom(void)
{
    lv_display_t *d = lv_display_get_default(); /* LVGL：取默认显示器对象 */
    /* LVGL：查询显示器当前逻辑分辨率；旋转后宽高互换，
     * 因此这里是运行时动态取值而非编译期常量 */
    int32_t w = (d != NULL) ? lv_display_get_horizontal_resolution(d) : 284;
    int32_t h = (d != NULL) ? lv_display_get_vertical_resolution(d) : 240;
    measure_geom_t g;
    g.w = w;
    g.h = h;
    if (w >= h) {
        /* 横屏 284×240（0°/180°）：历史调优值，逐像素保持 */
        g.row_top = 22;
        g.row_h = 44;
        g.row_gap = ROW_GAP;
        /* 底部两行小字（12px，行高约 16px 共 32px）：四行数据条底在 204
         * （22+4×44+3×2），206..238 两行正好落进底部空档，不压数据条。 */
        g.peak_y = 206;
        g.rate_btn_w = 48;
        g.val_font = LVGL_PORT_FONT_36;
    } else {
        /* 竖屏 240×284（90°/270°）：行高 44→54、数字 36→40px。
         * 40px Bold 下最宽 8 字符≈182px，加单位/箭头 ≤207px < 行内可用
         * 212px，不溢出。行间距由 4 收到 2：四行底 30+4×54+3×2=252，
         * 底部空档 252..284=32px 恰好放下两行 16px 小字（252/268），
         * 既不压数据条也不贴出屏底。 */
        g.row_top = 30;
        g.row_h = 54;
        g.row_gap = 2;
        g.peak_y = 252;
        g.rate_btn_w = 38;
        g.val_font = LVGL_PORT_FONT_40;
    }
    return g;
}

/**
 * @brief 一行数据条的三个子控件（供刷新函数按图索骥）
 *
 * 只有文字内容会随数据变，所以刷新时只拿这三个指针改文本，
 * 不需要每帧重建控件（重建控件开销大且会闪烁）。
 */
typedef struct {
    lv_obj_t *row;   /**< 彩色条本体 */
    lv_obj_t *value; /**< 大数字（白字） */
    lv_obj_t *unit;  /**< 单位小字 */
    lv_obj_t *arrow; /**< 方向箭头（仅电流用） */
} metric_t;

static metric_t s_v, s_i, s_p, s_e; /**< 电压/电流/功率/电能四行控件集 */
static lv_obj_t *s_lbl_env;         /**< 底部小字第 2 行：P 峰谷+容量+温湿度 */
static lv_obj_t *s_lbl_peak;        /**< 底部小字第 1 行：V/I 峰谷对 */
static lv_obj_t *s_lbl_alert;       /**< 顶部告警横幅（alert_mask 非 0 时显示） */

static void create_quick_menu(lv_obj_t *tile); /* 定义见文件末尾 */

/**
 * @brief 创建一行数据条（彩色底 + 左大数字 + 右侧箭头/单位）
 *
 * 行内用 LVGL 的 flex 布局：数字在最左、单位贴最右，两端对齐。
 * 用 flex 而不是手动算 x 坐标，是因为数字宽度随小数位动态变化，
 * 两端对齐（SPACE_BETWEEN）能保证单位始终贴右内边距、数字跳动不推挤单位。
 *
 * @param parent   父容器（测量页 tile）
 * @param y        该行顶部 y 坐标
 * @param row_w    行宽（随分辨率：横屏 272 / 竖屏 228）
 * @param row_h    行高（横屏 44 / 竖屏 54）
 * @param val_font 大数字字号（横屏 36 / 竖屏 40）
 * @param bg       彩色底色（COL_V/COL_I/COL_P/COL_E 之一）
 * @param m        [out] 输出三个子控件指针，供 screen_measure_refresh() 使用
 * @param with_arrow 是否在单位左侧放方向箭头（仅电流行为 true）
 */
static void create_row(lv_obj_t *parent, int y, int row_w, int row_h,
                       lvgl_port_font_size_t val_font, lv_color_t bg, metric_t *m,
                       bool with_arrow)
{
    lv_obj_t *row = lv_obj_create(parent); /* LVGL：新建通用对象（容器） */
    lv_obj_remove_style_all(row); /* LVGL：清掉主题默认样式，从"裸对象"起步自己画 */
    lv_obj_set_size(row, row_w, row_h); /* LVGL：设宽高（像素） */
    lv_obj_set_pos(row, SAFE_X, y); /* LVGL：设相对父容器的绝对坐标（x, y） */
    lv_obj_set_style_bg_color(row, bg, 0); /* LVGL：设背景色；part=0 即 LV_PART_MAIN 主体 */
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0); /* LVGL：背景不透明（默认可能被主题设为透明） */
    lv_obj_set_style_radius(row, 6, 0); /* LVGL：圆角半径 6px */
    lv_obj_set_style_pad_hor(row, ROW_PAD, 0); /* LVGL：左右内边距（内容不贴边） */
    lv_obj_set_style_pad_ver(row, 0, 0); /* LVGL：上下内边距清零（行高全给数字） */
    lv_obj_set_scrollable(row, false); /* LVGL：禁用滚动（内容不溢出，免得出滚动条） */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW); /* LVGL：开启 flex 布局，子项横排 */
    /* LVGL：主轴两端对齐（数字靠左、单位靠右）、交叉轴居中 */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    m->row = row;

    /* 左：大数字（白字），Bold 填满行高（横屏 36px/44px 行，竖屏 40px/54px 行） */
    m->value = lv_label_create(row); /* LVGL：新建文本标签 */
    lv_label_set_text(m->value, "0.00000"); /* LVGL：设文本（先放占位，刷新时替换） */
    /* LVGL：设字体；lvgl_port_get_font_bold() 取项目预生成的中文 Bold 字体 */
    lv_obj_set_style_text_font(m->value, lvgl_port_get_font_bold(val_font), 0);
    lv_obj_set_style_text_color(m->value, lv_color_white(), 0); /* LVGL：设文字颜色 */

    /* 右：箭头（可选）+ 单位 */
    lv_obj_t *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    /* LVGL：LV_SIZE_CONTENT 表示尺寸随内容自适应（箭头+单位的整体宽度不写死） */
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(right, false);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW); /* 箭头与单位横排 */
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 3, 0); /* LVGL：flex 子项之间的列间距 3px */

    if (with_arrow) {
        m->arrow = lv_label_create(right);
        lv_label_set_text(m->arrow, "▶");
        lv_obj_set_style_text_font(m->arrow, lvgl_port_get_font(LVGL_PORT_FONT_20), 0);
        lv_obj_set_style_text_color(m->arrow, lv_color_white(), 0);
    } else {
        m->arrow = NULL;
    }

    m->unit = lv_label_create(right);
    lv_label_set_text(m->unit, "");
    lv_obj_set_style_text_font(m->unit, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    lv_obj_set_style_text_color(m->unit, lv_color_white(), 0);
}

/**
 * @brief 在 tile 内构建测量页（四个数据条 + 环境行 + 快速菜单）
 *
 * 布局取舍：整页不用 flex 而用绝对坐标——四行数据条离面板圆角的距离
 * 必须逐像素可控，flex 的自动间距做不到"只留 6px"这种贴边布局。
 * 调用方为 ui_nav（tileview 的一页），旋转后会被重新调用。
 *
 * @param tile tileview 提供的页容器（见 screen_measure.h）
 */
void screen_measure_create(lv_obj_t *tile)
{
    measure_geom_t g = measure_geom();

    lv_obj_set_style_bg_color(tile, lv_color_white(), 0); /* 整页白底 */
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(tile, false); /* 页面固定不滚动（数据条是绝对定位的） */
    /* 不用 flex：这里全部用绝对坐标，才能精确控制与圆角的距离 */
    lv_obj_set_style_pad_all(tile, 0, 0); /* LVGL：四边内边距清零，坐标原点贴边 */

    /* 行宽 = 屏宽 - 两侧 SAFE_X；行距 = 行高 + 间隙，四行按 pitch 等距排开 */
    const int row_w = (int)g.w - 2 * SAFE_X;
    const int row_h = (int)g.row_h;
    const int row_pitch = row_h + (int)g.row_gap;
    create_row(tile, (int)g.row_top, row_w, row_h, g.val_font, COL_V, &s_v, false);
    create_row(tile, (int)g.row_top + row_pitch, row_w, row_h, g.val_font, COL_I, &s_i, true);
    create_row(tile, (int)g.row_top + 2 * row_pitch, row_w, row_h, g.val_font, COL_P, &s_p, false);
    create_row(tile, (int)g.row_top + 3 * row_pitch, row_w, row_h, g.val_font, COL_E, &s_e, false);

    /* 底部两行小字（见 measure_geom 的 peak_y）：单行装不下示例文本
     * （272px 宽仅容约 40 字符 @12px，示例约 60 字符），按"峰值行与环境行
     * 合并为两行小字"处理。格式约定：斜杠前=峰（max）后=谷（min）。
     * 行 1 = V/I 峰谷对；行 2 = P 峰谷 + 累计容量 Ah + 温湿度。 */
    s_lbl_peak = lv_label_create(tile);
    lv_label_set_text(s_lbl_peak, "V ----/----  I ----/----");
    lv_obj_set_style_text_font(s_lbl_peak, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(s_lbl_peak, COL_ENV, 0);
    lv_obj_set_width(s_lbl_peak, g.w); /* 宽度撑满整屏，靠文本居中实现水平居中 */
    lv_obj_set_style_text_align(s_lbl_peak, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_peak, 0, g.peak_y);

    /* 行 2：P 峰谷 + 容量 + 环境温湿度（原独立环境行并入，节省一行空间） */
    s_lbl_env = lv_label_create(tile);
    lv_label_set_text(s_lbl_env, "P ----/----  ----Ah  --.-℃  --%RH"); /* 占位符：没读到数据前不显示 0 */
    lv_obj_set_style_text_font(s_lbl_env, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(s_lbl_env, COL_ENV, 0);
    lv_obj_set_width(s_lbl_env, g.w);
    lv_obj_set_style_text_align(s_lbl_env, LV_TEXT_ALIGN_CENTER, 0); /* LVGL：文本居中对齐 */
    lv_obj_set_pos(s_lbl_env, 0, g.peak_y + 16); /* 行高约 16px，紧接行 1 */

    /* 告警横幅：顶部红条（覆盖在状态栏区域上层），alert_mask 为 0 时隐藏。
     * 用绝对定位压在 row_top 之上，不挤动四行数据条布局。 */
    s_lbl_alert = lv_label_create(tile);
    lv_label_set_text(s_lbl_alert, "⚠ 告警");
    lv_obj_set_style_text_font(s_lbl_alert, lvgl_port_get_font_bold(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_lbl_alert, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_lbl_alert, COL_ALERT, 0);
    lv_obj_set_style_bg_opa(s_lbl_alert, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_lbl_alert, 4, 0);
    lv_obj_set_style_pad_hor(s_lbl_alert, 4, 0);
    /* 宽度撑满内容区、高 16px：贴着 row_top 上沿（横屏 4..20，竖屏 12..28），
     * 与状态栏共享浮层轻微交叠可接受（安全相关提示优先可见）。 */
    lv_obj_set_width(s_lbl_alert, g.w - 2 * SAFE_X);
    lv_obj_set_style_text_align(s_lbl_alert, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_alert, SAFE_X, (int)g.row_top - 18);
    /* 初始无告警：隐藏（refresh 里按 alert_mask 显隐） */
    lv_obj_add_flag(s_lbl_alert, LV_OBJ_FLAG_HIDDEN);

    create_quick_menu(tile);
}

/**
 * @brief 动态小数位格式化：保证输出 ≤9 字符（36px Bold 下约 194px，加单位不撞行宽）
 *
 *   |v| < 1        → 5 位小数（0.01886）
 *   1 ≤ |v| < 100  → 4 位小数（19.9767）
 *   100 ≤ |v| <1e4 → 2 位小数（1234.56）
 *   ≥ 1e4          → 0 位小数（12345）
 *   NaN / ±inf     → "----"（坏帧占位，不冒充真实读数）
 *
 * 为什么要分级：固定小数位会在小数值上丢精度（0.01886 显示成 0.02），
 * 在大数值上又超宽挤掉单位；按数量级换挡兼顾"看得出变化"与"不换行"。
 *
 * @param dst [out] 输出缓冲
 * @param n   缓冲大小（snprintf 保证不越界）
 * @param v   待格式化的测量值
 */
static void fmt_val(char *dst, size_t n, float v)
{
    if (!isfinite(v)) { /* 数学库：排除 NaN/±inf */
        /* 【U3 修复】坏帧（采样异常/除零产生的 NaN、inf）不能显示成 "0.0000"——
         * 那会被用户当成"真的 0V/0A"（负载断了？），与真实零读数无法区分。
         * 显示 "----"（半角连字符）作占位：字体子集含 ASCII 连字符，渲染无风险，
         * 且四个等宽字符与数字位宽接近，行布局不跳动。 */
        snprintf(dst, n, "----");
        return;
    }
    float a = fabsf(v); /* 数学库：取绝对值（分级只看量级，不分正负） */
    if (a < 1.0f) {
        snprintf(dst, n, "%.5f", (double)v);
    } else if (a < 100.0f) {
        snprintf(dst, n, "%.4f", (double)v);
    } else if (a < 10000.0f) {
        snprintf(dst, n, "%.2f", (double)v);
    } else if (a < 1e6f) {
        snprintf(dst, n, "%.0f", (double)v);
    } else {
        snprintf(dst, n, "%.2e", (double)v); /* 科学计数法兜底，如 1.23e+06 */
    }
}

/**
 * @brief 用最新测量快照刷新四行数字、峰值行、告警横幅与环境行
 *
 * 由 ui.c 的 UI 循环按用户设定的刷新率（1~30Hz）调用；低档位时
 * 传入的 snap 已由 ui.c 做过滑动平均，本函数只负责"贴到屏上"。
 * 只更新文本不动控件结构，所以刷新本身不会引起布局跳动。
 * 峰值行显示本次上电的 V/I/P 峰谷值与累计容量 Ah；告警横幅按
 * snap->alert_mask 显隐（安全相关，每次刷新都判）。
 *
 * @param snap 最新数据快照（NULL 时直接返回）
 */
void screen_measure_refresh(const data_store_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }

    char buf[24];

    fmt_val(buf, sizeof(buf), snap->voltage_v);
    lv_label_set_text(s_v.value, buf); /* LVGL：替换标签文本 */
    lv_label_set_text(s_v.unit, "V");

    fmt_val(buf, sizeof(buf), snap->current_a);
    lv_label_set_text(s_i.value, buf);
    lv_label_set_text(s_i.unit, "A");
    if (s_i.arrow != NULL) {
        /* 箭头随屏幕角度自适应：电流的物理流向固定，屏幕转到哪个角度，
         * 箭头就指向屏幕坐标系里的对应方向（0°→右/90°→下/180°→左/270°→上），
         * 负电流取反向。下标 = 角度/90。 */
        static const char *const arrow_pos[4] = { "▶", "▼", "◀", "▲" };
        static const char *const arrow_neg[4] = { "◀", "▲", "▶", "▼" };
        /* lvgl_port_get_rotation_angle() 取当前屏幕旋转角（0/90/180/270），
         * /90 再 &3 得 0~3 下标，正好落进上面两张方向表 */
        int t = (int)(lvgl_port_get_rotation_angle() / 90) & 3;
        lv_label_set_text(s_i.arrow, (snap->current_a < 0.0f) ? arrow_neg[t] : arrow_pos[t]);
    }

    fmt_val(buf, sizeof(buf), snap->power_w);
    lv_label_set_text(s_p.value, buf);
    lv_label_set_text(s_p.unit, "W");

    fmt_val(buf, sizeof(buf), snap->energy_wh);
    lv_label_set_text(s_e.value, buf);
    lv_label_set_text(s_e.unit, "Wh");

    /* 底部两行小字：行 1 = V/I 峰谷对，行 2 = P 峰谷 + 容量 + 温湿度。
     * 峰谷值用 fmt_val（NaN 显示 "----"，与大数字同规），斜杠前=峰、后=谷。 */
    char sv_max[12], sv_min[12], si_max[12], si_min[12];
    char sp_max[12], sp_min[12], sah[12];
    char peak[64], env[80]; /* fmt_val 输出 ≤9 字符，缓冲区按最坏 11 字节/字段预留 */
    fmt_val(sv_max, sizeof(sv_max), snap->v_max);
    fmt_val(sv_min, sizeof(sv_min), snap->v_min);
    fmt_val(si_max, sizeof(si_max), snap->i_max);
    fmt_val(si_min, sizeof(si_min), snap->i_min);
    fmt_val(sp_max, sizeof(sp_max), snap->p_max);
    fmt_val(sp_min, sizeof(sp_min), snap->p_min);
    fmt_val(sah, sizeof(sah), snap->capacity_ah);
    snprintf(peak, sizeof(peak), "V %s/%s  I %s/%s", sv_max, sv_min, si_max, si_min);
    lv_label_set_text(s_lbl_peak, peak);

    /* 告警横幅：alert_mask 非 0 时显示顶部红条（文字按位拼），为 0 隐藏。
     * 显隐只改 flag 不重建控件，不影响四行数据条布局。 */
    if (s_lbl_alert != NULL) {
        if (snap->alert_mask != 0) {
            char alert[48];
            snprintf(alert, sizeof(alert), "⚠%s%s%s",
                     (snap->alert_mask & DATA_ALERT_OVP) ? " 过压" : "",
                     (snap->alert_mask & DATA_ALERT_OCP) ? " 过流" : "",
                     (snap->alert_mask & DATA_ALERT_OPP) ? " 过功率" : "");
            lv_label_set_text(s_lbl_alert, alert);
            lv_obj_clear_flag(s_lbl_alert, LV_OBJ_FLAG_HIDDEN); /* LVGL：取消隐藏 */
        } else {
            lv_obj_add_flag(s_lbl_alert, LV_OBJ_FLAG_HIDDEN); /* LVGL：隐藏 */
        }
    }

    /* AHT20 尚未读出有效值时（测量核写 NaN，见 power_measure.c）显示占位符；
     * 【B19 修复】真实 0℃/0%RH 不再被误判为"无数据"（原双零启发式已废） */
    if (isnan(snap->temperature_c) || isnan(snap->humidity_pct)) {
        snprintf(env, sizeof(env), "P %s/%s  %sAh  --.-℃  --%%RH",
                 sp_max, sp_min, sah); /* %% 才输出一个字面 % */
    } else {
        snprintf(env, sizeof(env), "P %s/%s  %sAh  %.1f℃  %.0f%%RH",
                 sp_max, sp_min, sah, (double)snap->temperature_c,
                 (double)snap->humidity_pct);
    }
    lv_label_set_text(s_lbl_env, env);
}

/* ============================================================
 * 快速菜单（全程按键可操作，不依赖触屏）
 *
 * 打开：测量页 K2 长按。关闭：K1 或 K2 长按（开关式）。
 * 菜单内：
 *   · K1 单击 = 焦点上移，K2 单击 = 焦点下移（两行循环，带焦点高亮 + 反馈音）；
 *   · K2 双击 = 激活当前焦点行：
 *       -「清除电能统计数据」→ 执行清零，**自动关闭菜单**；
 *       -「数据刷新率」→ 在 1/5/10/20/30Hz 间循环切换并立即持久化。
 * 触屏同样可用：点任意档位按钮直接切换，点清除行直接清零并关闭。
 *
 * 状态机：s_menu（控件）恒存在，s_menu_open（已开）/ s_menu_animating
 * （升降动画中）两个标志组合判断忙闲，动画期间拒绝重复开/关（防重入）。
 * ============================================================ */
#define MENU_H 164 /* 内边距16 + 把手8+4 + 标题区38+4 + 清除38+4 + 说明14+4 + 档位行34 */
#define MENU_FOCUS_CNT 2 /* 0=清除行，1=刷新率行 */
static lv_obj_t *s_menu = NULL;  /**< 快速菜单滑板容器 */
static bool s_menu_open = false; /**< 菜单是否完全展开（动画结束后置位） */
static bool s_menu_animating = false; /**< 滑板升降动画进行中（防重入） */
static int s_focus = 0;          /**< 按键焦点行：0=清除行，1=刷新率行 */
static lv_obj_t *s_clear_btn = NULL; /**< 焦点行 0：清除电能按钮 */
static lv_obj_t *s_rate_row = NULL;  /**< 焦点行 1：刷新率档位行（焦点描边画在它身上） */
static lv_obj_t *s_rate_btns[5];     /**< 五个档位按钮 */
static const uint8_t s_rate_vals[5] = { 1, 5, 10, 20, 30 };      /**< 档位实际 Hz 值 */
static const char *s_rate_texts[5] = { "1Hz", "5Hz", "10Hz", "20Hz", "30Hz" }; /**< 档位按钮文字 */

/**
 * @brief 造一个菜单内迷你按钮（白底描边 + 居中文字，按下变浅蓝）
 * @param parent 父容器
 * @param text   按钮文字
 * @param w,h    宽高（支持 LV_PCT() 百分比宽）
 * @param fsize  项目字号档位（lvgl_port_get_font 按档取字体）
 * @return 按钮对象指针
 */
static lv_obj_t *menu_btn(lv_obj_t *parent, const char *text, int32_t w, int32_t h,
                          lvgl_port_font_size_t fsize)
{
    lv_obj_t *b = lv_button_create(parent); /* LVGL：新建按钮（自带按压反馈等主题样式） */
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    /* LVGL：第 3 参是 state——这里只在 LV_STATE_PRESSED（被按住）时变浅蓝，
     * 松手自动恢复默认态颜色，不用手写按下/弹起逻辑 */
    lv_obj_set_style_bg_color(b, lv_color_hex(0xE8F0FE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0); /* LVGL：描边颜色 */
    lv_obj_set_style_border_width(b, 1, 0);          /* LVGL：描边宽度 1px */
    lv_obj_set_style_radius(b, 8, 0);                /* 圆角 8px（与菜单卡片统一） */
    lv_obj_set_style_shadow_width(b, 0, 0); /* LVGL：去掉按钮主题自带的投影，扁平化 */

    lv_obj_t *l = lv_label_create(b); /* 文字标签作为按钮子对象 */
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, lvgl_port_get_font(fsize), 0);
    lv_obj_set_style_text_color(l, COL_INK, 0);
    lv_obj_center(l); /* LVGL：把子对象摆到父容器正中（比算坐标省事） */
    return b;
}

/**
 * @brief 刷新按键焦点高亮
 *
 * 被聚焦的整行描橙/蓝色粗边（2px），未聚焦的恢复 1px 灰边。
 * 焦点画在"整行"而不是单个按钮上，是因为刷新率行有 5 个按钮，
 * 行级描边让"当前操作对象是谁"一眼可见（纯按键操作必需）。
 */
static void menu_focus_refresh(void)
{
    const lv_coord_t fw = s_focus == 0 ? 2 : 1;
    lv_color_t fc = s_focus == 0 ? COL_PRIMARY : COL_BORDER;
    if (s_clear_btn != NULL) {
        lv_obj_set_style_border_width(s_clear_btn, fw, 0);
        lv_obj_set_style_border_color(s_clear_btn, fc, 0);
    }
    if (s_rate_row != NULL) {
        lv_obj_set_style_border_width(s_rate_row, s_focus == 1 ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_rate_row, COL_PRIMARY, 0);
    }
}

/**
 * @brief 刷新档位按钮高亮（当前档蓝底白字，其余白底黑字）
 */
static void menu_rate_refresh(void)
{
    uint8_t cur = ui_get_display_rate_hz();
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = s_rate_btns[i];
        if (b == NULL) {
            continue;
        }
        bool sel = (s_rate_vals[i] == cur);
        lv_obj_set_style_bg_color(b, sel ? COL_PRIMARY : lv_color_white(), 0);
        lv_obj_t *l = lv_obj_get_child(b, 0); /* LVGL：取第 0 个子对象（按钮里的文字标签） */
        if (l != NULL) {
            lv_obj_set_style_text_color(l, sel ? lv_color_white() : COL_INK, 0);
        }
    }
}

/** 关闭快速菜单（滑板下沉动画；位置按当前分辨率沉到屏外） */
static void menu_close(void); /* 前向声明：手势回调先于定义使用 */

/**
 * @brief LVGL 动画执行回调：把菜单滑到 y=v
 * @param var 动画作用对象（lv_anim_set_var 传入的 s_menu）
 * @param v   本帧的 y 坐标
 */
static void anim_menu_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v); /* LVGL：直接改对象的 y 坐标 */
}

/**
 * @brief 关闭动画完成回调：隐藏菜单并复位状态标志
 * @param a 动画描述符（未使用）
 */
static void menu_close_done(lv_anim_t *a)
{
    (void)a;
    lv_obj_set_hidden(s_menu, true); /* LVGL：隐藏对象（不绘制也不响应输入） */
    s_menu_animating = false;
    s_menu_open = false;
}

/**
 * @brief 打开动画完成回调：只置状态标志（面板已到位）
 * @param a 动画描述符（未使用）
 */
static void menu_open_done(lv_anim_t *a)
{
    (void)a;
    s_menu_animating = false;
    s_menu_open = true;
}

/**
 * @brief 测量页滑动手势分发（ui_nav 在输入设备层识别滑动后调用）
 *
 * 为什么滑动识别放在 ui_nav 而不是页面内：滑动要和 tileview 翻页
 * 抢手势，必须在同一个输入回调里统一仲裁，页面只接收裁决结果。
 *
 * @param dir 滑动方向：LV_DIR_TOP=上滑开菜单，LV_DIR_BOTTOM=下滑关菜单
 */
void screen_measure_swipe_cb(lv_dir_t dir)
{
    if (dir == LV_DIR_TOP) {
        screen_measure_open_menu();
    } else if (dir == LV_DIR_BOTTOM) {
        menu_close();
    }
}

/**
 * @brief 关闭快速菜单（滑板下沉 180ms，完成时才真正隐藏）
 *
 * 动画期间 s_menu_animating=true，可挡住重复关闭/打开；
 * 用动画而不是瞬移，是让"菜单从哪来回哪去"有空间感。
 */
static void menu_close(void)
{
    if (s_menu == NULL || (!s_menu_open && !s_menu_animating)) {
        return;
    }
    measure_geom_t g = measure_geom();
    s_menu_open = false;
    s_menu_animating = true;
    lv_anim_t anim;
    lv_anim_init(&anim); /* LVGL：初始化动画描述符为默认值 */
    lv_anim_set_var(&anim, s_menu); /* LVGL：动画作用对象 */
    lv_anim_set_values(&anim, g.h - MENU_H, g.h); /* 沉回屏外（起值→终值） */
    lv_anim_set_duration(&anim, 180); /* LVGL：动画时长 180ms */
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in); /* LVGL：缓入曲线（先慢后快，像"掉下去"） */
    lv_anim_set_exec_cb(&anim, anim_menu_y); /* LVGL：每帧调用它写入新 y */
    lv_anim_set_completed_cb(&anim, menu_close_done); /* LVGL：播完回调（隐藏+清标志） */
    lv_anim_start(&anim); /* LVGL：启动动画（交给 LVGL 定时器逐帧驱动） */
}

/**
 * @brief 清零动作（触屏/按键共用）：执行清零 + 成功音 + 自动关闭菜单
 *
 * "操作完成自动关闭"是刻意设计：清零是一次性动作，做完留着菜单
 * 只会挡住数据，自动收起省一次手动退出。
 */
static void menu_do_clear(void)
{
    (void)power_measure_reset_energy(); /* 业务：清零电能统计（含运行时间） */
    (void)buzzer_play(BUZZER_TONE_OK);  /* 业务：播成功提示音（非阻塞） */
    if (s_menu_open) {
        menu_close();
    }
}

/**
 * @brief 刷新率切到下一档（1→5→10→20→30→1 循环）
 *
 * 用循环表而不是 ++：档位不是等差数列，且天然收尾回绕；
 * ui_set_display_rate_hz() 内部会写 settings 立即持久化（掉电不丢）。
 */
static void menu_rate_cycle(void)
{
    static const uint8_t order[5] = { 1, 5, 10, 20, 30 };
    uint8_t cur = ui_get_display_rate_hz();
    uint8_t next = order[0];
    for (int i = 0; i < 5; i++) {
        if (order[i] == cur) {
            next = order[(i + 1) % 5]; /* %5 实现尾档回绕到首档 */
            break;
        }
    }
    ui_set_display_rate_hz(next);
    menu_rate_refresh();
    (void)buzzer_play(BUZZER_TONE_CLICK); /* 业务：切换档位的按键反馈音 */
}

/**
 * @brief 清除按钮的点击回调（触屏路径）
 * @param e LVGL 事件对象（未使用）
 */
static void menu_clear_clicked_cb(lv_event_t *e)
{
    (void)e;
    menu_do_clear();
}

/**
 * @brief 档位按钮的点击回调（触屏路径直接选档，无需循环切换）
 *
 * 目标档位通过注册回调时的 user_data 传进来（见 create_quick_menu），
 * 这样五个按钮可以共用同一个回调函数。
 *
 * @param e LVGL 事件对象，user_data 里带目标 Hz 值
 */
static void menu_rate_clicked_cb(lv_event_t *e)
{
    /* LVGL：取注册事件时传入的 user_data，强转回目标档位值 */
    uint8_t hz = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    ui_set_display_rate_hz(hz);
    menu_rate_refresh();
    (void)buzzer_play(BUZZER_TONE_CLICK);
}

/**
 * @brief 构建快速菜单滑板（含把手、标题、清除行、刷新率档位行）
 *
 * 滑板初始放在屏幕下沿之外（y=g.h）并隐藏，打开动画把它升上来。
 * 结构自上而下：拖拽把手 → 标题+按键提示 → 清除按钮 → 刷新率说明+档位行。
 * 触摸目标尺寸都 ≥34px 高、按钮整行可点，手指不踩空。
 *
 * @param tile 测量页容器（菜单是它的最后一个子对象，浮在数据行之上）
 */
static void create_quick_menu(lv_obj_t *tile)
{
    measure_geom_t g = measure_geom();

    /* 重建场景（旋转后 ui_nav 重建本页）：菜单随之新建，先复位开关态，
     * 避免 s_menu_open 残留为 true 导致按键被投递给一个隐藏的新菜单。 */
    s_menu_open = false;
    s_focus = 0;
    s_clear_btn = NULL;
    s_rate_row = NULL;
    for (int i = 0; i < 5; i++) {
        s_rate_btns[i] = NULL;
    }

    s_menu = lv_obj_create(tile);
    lv_obj_remove_style_all(s_menu); /* 剥掉主题样式，完全自绘 */
    lv_obj_set_size(s_menu, g.w, MENU_H); /* 宽随屏、高 164（恰好装下全部内容） */
    lv_obj_set_pos(s_menu, 0, g.h); /* 屏外（关闭态，横 240 / 竖 284） */
    lv_obj_set_style_bg_color(s_menu, COL_CARD, 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu, 12, 0); /* 顶部圆角：视觉上像"抽屉" */
    lv_obj_set_style_border_color(s_menu, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_menu, 1, 0);
    lv_obj_set_style_pad_all(s_menu, 8, 0);  /* LVGL：四边内边距 8px */
    lv_obj_set_style_pad_row(s_menu, 4, 0);  /* LVGL：flex 纵排时子项行间距 4px */
    lv_obj_set_scrollable(s_menu, false);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN); /* LVGL：flex 纵排，自上而下铺内容 */
    lv_obj_set_hidden(s_menu, true); /* 初始隐藏，等 open 动画前再显示 */

    /* 顶部拖拽把手（视觉可滑提示，居中 36x4 圆条） */
    lv_obj_t *handle_row = lv_obj_create(s_menu);
    lv_obj_remove_style_all(handle_row);
    lv_obj_set_size(handle_row, LV_PCT(100), 8); /* LVGL：LV_PCT(100)=父容器内容宽的 100% */
    lv_obj_set_scrollable(handle_row, false);
    lv_obj_set_flex_flow(handle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(handle_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER); /* 把手居中 */
    lv_obj_t *handle = lv_obj_create(handle_row);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 36, 4);
    lv_obj_set_style_bg_color(handle, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0); /* LVGL：圆角取半高，成胶囊形 */
    lv_obj_set_clickable(handle, false); /* LVGL：不接收点击（纯装饰，别抢按钮事件） */

    /* 标题区：标题与按键提示分两行，避免窄屏（竖屏 240 宽）同行溢出重叠 */
    lv_obj_t *hdr = lv_obj_create(s_menu);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT); /* 高度随两行文字内容自适应 */
    lv_obj_set_scrollable(hdr, false);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, "快速菜单");
    lv_obj_set_style_text_font(t, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    lv_obj_set_style_text_color(t, COL_INK, 0);

    lv_obj_t *hint = lv_label_create(hdr);
    lv_label_set_text(hint, "K1/K2 上下 · 双击确定 · 长按退出"); /* 纯按键操作说明 */
    lv_obj_set_style_text_font(hint, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(hint, COL_ENV, 0);

    /* ① 清除电能统计（焦点行 0） */
    s_clear_btn = menu_btn(s_menu, "清除电能统计数据", LV_PCT(100), 38, LVGL_PORT_FONT_16);
    /* LVGL：注册事件回调——按钮被点击(LV_EVENT_CLICKED)时调用 menu_clear_clicked_cb */
    lv_obj_add_event_cb(s_clear_btn, menu_clear_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* ② 数据刷新率（焦点行 1；按钮行自身描边作为焦点指示） */
    lv_obj_t *cap = lv_label_create(s_menu);
    lv_label_set_text(cap, "数据刷新率（低档位做滑动平均，读数更稳）"); /* 说明为什么低档更稳 */
    lv_obj_set_style_text_font(cap, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(cap, COL_ENV, 0);

    s_rate_row = lv_obj_create(s_menu);
    lv_obj_remove_style_all(s_rate_row);
    lv_obj_set_size(s_rate_row, LV_PCT(100), 34);
    lv_obj_set_scrollable(s_rate_row, false);
    lv_obj_set_flex_flow(s_rate_row, LV_FLEX_FLOW_ROW);
    /* LVGL：均分空隙（SPACE_EVENLY），五档按钮等距排开 */
    lv_obj_set_flex_align(s_rate_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(s_rate_row, 8, 0);
    lv_obj_set_style_border_color(s_rate_row, COL_PRIMARY, 0); /* 焦点态时靠 border_width 现形 */

    for (int i = 0; i < 5; i++) {
        /* 按钮宽随分辨率：横屏 48（5 个+间隙在 264 内宽内），
         * 竖屏收窄到 38（5×38=190，在 220 内宽内仍均布可点）。 */
        s_rate_btns[i] =
            menu_btn(s_rate_row, s_rate_texts[i], g.rate_btn_w, 34, LVGL_PORT_FONT_12);
        /* 五个按钮共用一个回调，把各自档位值塞进 user_data 区分 */
        lv_obj_add_event_cb(s_rate_btns[i], menu_rate_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_rate_vals[i]);
    }
    menu_rate_refresh();  /* 按当前档位点亮对应按钮 */
    menu_focus_refresh(); /* 焦点回到清除行 */
}

/**
 * @brief 打开快速菜单（滑板自底部升起 220ms）
 *
 * 幂等：菜单已开或正在动画时直接返回，K2 长按连触不会叠出两层动画。
 * 打开时焦点固定回到第一行（清除行），从"确定会做什么"开始，
 * 避免上次停留的焦点行导致误操作。
 */
void screen_measure_open_menu(void)
{
    if (s_menu == NULL || s_menu_open || s_menu_animating) {
        return;
    }
    measure_geom_t g = measure_geom();
    s_focus = 0; /* 每次打开焦点回到第一行 */
    menu_rate_refresh();
    menu_focus_refresh();
    lv_obj_set_hidden(s_menu, false); /* 先显示再动画，否则升起来看不见 */
    s_menu_animating = true;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_menu);
    lv_anim_set_values(&anim, g.h, g.h - MENU_H); /* 自底部升起（屏外→屏内） */
    lv_anim_set_duration(&anim, 220); /* 升起比落下稍慢（220 vs 180ms），出场更从容 */
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out); /* LVGL：缓出曲线（先快后慢，像"刹车"） */
    lv_anim_set_exec_cb(&anim, anim_menu_y);
    lv_anim_set_completed_cb(&anim, menu_open_done);
    lv_anim_start(&anim);
    /* 按键反馈音由 ui_nav_handle_key 统一播放，触屏路径无需本处再响。 */
}

/**
 * @brief 查询快速菜单是否处于打开状态
 * @return true=已完全展开（动画中不算），供 ui_nav 决定按键投递给谁
 */
bool screen_measure_menu_is_open(void)
{
    return s_menu_open;
}

/**
 * @brief 快速菜单按键处理（ui_nav 每次按键先问菜单，菜单开着就由菜单消费）
 *
 * 键位设计（开关式 + 两键分工）：
 *   长按 K1/K2 → 关闭菜单（打开也是长按，同一手势开/关好记）；
 *   单击 K1/K2 → 焦点上/下移（循环）；
 *   双击       → 激活焦点行（K1/K2 双击都认，减少记忆负担）。
 *
 * @param id 按键 ID（BUTTON_ID_BOOT=K1，其他=K2）
 * @param ev 按键事件（单击/双击/长按起始等）
 * @return true=事件已被菜单消费，ui_nav 不再翻页；false=菜单未开或事件不归菜单管
 */
bool screen_measure_menu_key(button_id_t id, button_event_t ev)
{
    if (!s_menu_open) {
        return false;
    }

    if (ev == BUTTON_EVENT_LONG_PRESS_START) {
        /* K1/K2 长按均关闭（打开也是 K2 长按，开关式）；按键音由外层统一响 */
        menu_close();
        return true;
    }

    if (ev == BUTTON_EVENT_SINGLE_CLICK) {
        /* K1 = 焦点上移，K2 = 焦点下移（循环）；按键音由外层统一响 */
        if (id == BUTTON_ID_BOOT) {
            s_focus = (s_focus - 1 + MENU_FOCUS_CNT) % MENU_FOCUS_CNT; /* 负数取模防溢出 */
        } else {
            s_focus = (s_focus + 1) % MENU_FOCUS_CNT;
        }
        menu_focus_refresh();
        return true;
    }

    if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
        /* K2 双击 = 激活当前焦点行（K1 双击同样有效，减少记忆负担） */
        if (s_focus == 0) {
            menu_do_clear(); /* 内部会自动关闭菜单 */
        } else {
            menu_rate_cycle();
        }
        return true;
    }

    return false;
}
