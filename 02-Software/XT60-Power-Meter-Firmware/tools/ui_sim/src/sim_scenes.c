/**
 * @file sim_scenes.c
 * @brief 场景脚本：构建界面 → 模拟真实触摸交互 → 截图（设置页全状态覆盖）
 *
 * 交互一律走**触摸路径**（而不是直接调用内部函数），因此场景本身就是
 * 一份"可执行的验收流程"：哪个按钮点不中、哪页打开后布局崩了，
 * 都会直接体现在截图（及控制台"未找到文本"告警）里。
 *
 * 点击目标用**文本语义查找**（find_text），避免硬编码坐标：
 * 先在屏幕与顶层图层里按文本找 label，再向上找可点击祖先、
 * 滚动到可见后取中心点点击 —— 与真人手指操作等价且更稳。
 */
#include "sim.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "button.h"
#include "lvgl_port.h"
#include "screens/splash.h"
#include "screen_measure.h"
#include "screen_wave.h"
#include "ui.h"
#include "ui_nav.h"

/* ============================================================
 * 场景辅助
 * ============================================================ */

static void shot(const char *name)
{
    sim_step(60);
    sim_shot(name);
}

/** @brief 对象是否落在当前屏幕可见范围内（过滤掉 tileview 中其他页的对象） */
static bool on_screen(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int w = 0;
    int h = 0;
    sim_get_resolution(&w, &h);
    if (a.x2 < 0 || a.y2 < 0 || a.x1 >= w || a.y1 >= h) {
        return false;
    }
    return true;
}

/** @brief 深度优先查找文本匹配的 label（跳过隐藏对象——如已关闭的对话框） */
static lv_obj_t *find_text_rec(lv_obj_t *root, const char *text)
{
    if (root == NULL || lv_obj_is_hidden(root)) {
        /* 关键：msgbox 关闭走 close_async（先 HIDDEN 再延迟删除），
         * 不过滤隐藏对象会点到"看不见的旧对话框按钮"上。 */
        return NULL;
    }
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *t = lv_label_get_text(root);
        if (t != NULL && strcmp(t, text) == 0 && on_screen(root)) {
            return root;
        }
    }
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *r = find_text_rec(lv_obj_get_child(root, i), text);
        if (r != NULL) {
            return r;
        }
    }
    return NULL;
}

/** @brief 在屏幕与顶层图层中查找文本（后者用于 msgbox） */
static lv_obj_t *find_text(const char *text)
{
    lv_obj_t *r = find_text_rec(lv_screen_active(), text);
    if (r == NULL) {
        r = find_text_rec(lv_layer_top(), text);
    }
    return r;
}

static lv_obj_t *clickable_ancestor(lv_obj_t *o)
{
    while (o != NULL && !lv_obj_is_clickable(o)) {
        o = lv_obj_get_parent(o);
    }
    return o;
}

static int tileview_scroll_x(void); /* 前向声明（定义在翻页辅助区） */

static void click_obj(lv_obj_t *o)
{
    int sx0 = tileview_scroll_x();
    lv_obj_scroll_to_view(o, LV_ANIM_OFF);
    sim_step(40);
    int sx1 = tileview_scroll_x();
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    printf("[scene] 点击 @(%d,%d)-(%d,%d) scroll_x %d->%d\n", (int)a.x1, (int)a.y1, (int)a.x2,
           (int)a.y2, sx0, sx1);
    sim_click((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
}

/** @brief 查找"有垂直滚动余量"的容器（内容溢出屏幕时用于滚动回退） */
static lv_obj_t *find_scrollable_rec(lv_obj_t *root)
{
    if (root == NULL || lv_obj_is_hidden(root)) {
        return NULL;
    }
    if ((lv_obj_get_scroll_dir(root) & LV_DIR_VER) && lv_obj_get_scroll_bottom(root) > 4) {
        return root;
    }
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *r = find_scrollable_rec(lv_obj_get_child(root, i));
        if (r != NULL) {
            return r;
        }
    }
    return NULL;
}

/** @brief 按文本点击（返回是否命中，未命中会打印告警——即"验收失败"）
 *
 * 找不到时先做一次"滚动回退"：把当前页中垂直有滚动余量的容器滚到底，
 * 以覆盖"核心按钮被挤出首屏、需要滚动才能点"的页面（如校准编辑页）。
 */
static bool click_text(const char *text)
{
    lv_obj_t *o = find_text(text);
    if (o == NULL) {
        lv_obj_t *sc = find_scrollable_rec(lv_screen_active());
        if (sc != NULL) {
            lv_obj_scroll_to_y(sc, 10000, LV_ANIM_OFF);
            sim_step(60);
            o = find_text(text);
        }
    }
    if (o == NULL) {
        printf("[scene] !! 未找到可点击文本: %s\n", text);
        return false;
    }
    lv_obj_t *c = clickable_ancestor(o);
    if (c == NULL) {
        printf("[scene] !! 文本无可点击祖先: %s\n", text);
        return false;
    }
    click_obj(c);
    return true;
}

/** @brief 查找第一个可见滑条（跳过隐藏对象） */
static lv_obj_t *find_slider_rec(lv_obj_t *root)
{
    if (root == NULL || lv_obj_is_hidden(root)) {
        return NULL;
    }
    if (lv_obj_check_type(root, &lv_slider_class) && on_screen(root)) {
        return root;
    }
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *r = find_slider_rec(lv_obj_get_child(root, i));
        if (r != NULL) {
            return r;
        }
    }
    return NULL;
}

/** @brief 把第一个滑条拖到指定百分比（0~100，控件量程内线性映射） */
static void drag_slider_to_percent(int percent)
{
    lv_obj_t *s = find_slider_rec(lv_screen_active());
    if (s == NULL) {
        printf("[scene] !! 未找到滑条\n");
        return;
    }
    lv_obj_scroll_to_view(s, LV_ANIM_OFF);
    sim_step(40);
    lv_area_t a;
    lv_obj_get_coords(s, &a);
    int y = (a.y1 + a.y2) / 2;
    int x_left = a.x1 + 12;
    int x_right = a.x2 - 12;
    int target = x_left + (x_right - x_left) * percent / 100;

    sim_touch_down(x_left, y);
    sim_step(40);
    for (int i = 1; i <= 6; i++) {
        sim_touch_move(x_left + (target - x_left) * i / 6, y);
        sim_step(25);
    }
    sim_touch_up();
    sim_step(150);
}

static void close_subpage_if_open(void); /* 前向声明（定义在翻页辅助区） */

/** @brief 按住某个文本按钮截"按下态"，随后松开
 *
 * 注：LVGL 9 在"按下→移出→松开"时仍可能派发 CLICKED（不校验释放位置），
 * 因此松开后若真的打开了二级页，立即用 close_subpage_if_open() 兜底关闭。
 */
static void press_text_snapshot(const char *text, const char *shot_name)
{
    lv_obj_t *o = find_text(text);
    if (o == NULL) {
        printf("[scene] !! 未找到文本: %s\n", text);
        return;
    }
    lv_obj_t *c = clickable_ancestor(o);
    lv_area_t a;
    lv_obj_get_coords(c, &a);
    int cx = (a.x1 + a.x2) / 2;
    int cy = (a.y1 + a.y2) / 2;

    sim_touch_down(cx, cy);
    sim_step(80);
    sim_shot(shot_name);
    sim_touch_up();
    sim_step(200);
    /* 若松开触发了点击（打开了页面），把它关掉，恢复首页状态 */
    close_subpage_if_open();
}

/* ============================================================
 * 翻页（触摸滑动为主，按键兜底）
 *
 * 诊断背景：波形页上从 chart 区域起手拖动时不翻页（疑与图表区域
 * 的拖动吞噬有关，待真机复核）。因此：
 *   · 滑动统一从顶部标题区起手（避开 chart）；
 *   · 每次翻页打印 tileview 的 scroll_x，滑动未到位则用 K2/K1 单击兜底，
 *     保证整组场景可完整执行（并留下"滑动失效"的明确证据）。
 * ============================================================ */

static lv_obj_t *find_tileview_rec(lv_obj_t *root)
{
    if (root == NULL) {
        return NULL;
    }
    if (lv_obj_check_type(root, &lv_tileview_class)) {
        return root;
    }
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *r = find_tileview_rec(lv_obj_get_child(root, i));
        if (r != NULL) {
            return r;
        }
    }
    return NULL;
}

static int tileview_scroll_x(void)
{
    lv_obj_t *tv = find_tileview_rec(lv_screen_active());
    return (tv != NULL) ? (int)lv_obj_get_scroll_x(tv) : -1;
}

/** @brief 左滑一页（从顶部起手），打印前后 scroll_x */
static void swipe_next_page(void)
{
    int w = 0;
    sim_get_resolution(&w, NULL);
    int x0 = tileview_scroll_x();
    sim_swipe(w - 24, 32, 24, 32, 6);
    sim_step(400);
    printf("[scene] 滑动翻页: scroll_x %d -> %d\n", x0, tileview_scroll_x());
}

/** @brief 右滑一页（回上一页），打印前后 scroll_x */
static void swipe_prev_page(void)
{
    int w = 0;
    sim_get_resolution(&w, NULL);
    int x0 = tileview_scroll_x();
    sim_swipe(24, 32, w - 24, 32, 6);
    sim_step(400);
    printf("[scene] 滑动翻页(回): scroll_x %d -> %d\n", x0, tileview_scroll_x());
}

/** @brief 确保停留在指定页（0=测量 / 1=波形 / 2=设置）：滑动优先，按键兜底
 *
 * 容差取页宽 1/4：旋转恢复后 tileview 的 scroll 位置不会自动重算
 * （如横→竖→横 后会停在 480 而非 568），此时会被判定为"页内错位"，
 * 通过一次页内滑动触发 snap 吸附回正位。
 */
static void ensure_page(int page_idx)
{
    int w = 0;
    sim_get_resolution(&w, NULL);
    int target = page_idx * w;
    int tol = w / 4;

    for (int guard = 0; guard < 5; guard++) {
        int x = tileview_scroll_x();
        if (x > target - tol && x < target + tol) {
            return;
        }
        if (x < target) {
            swipe_next_page();
            if (tileview_scroll_x() < target - tol) {
                /* 滑动未到位 → K2 单击（下一页）兜底 */
                ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_SINGLE_CLICK);
                sim_step(320);
                printf("[scene] 按键兜底(→): scroll_x=%d\n", tileview_scroll_x());
            }
        } else {
            swipe_prev_page();
            if (tileview_scroll_x() > target + tol) {
                /* 滑动未到位 → K1 单击（上一页）兜底 */
                ui_nav_handle_key(BUTTON_ID_BOOT, BUTTON_EVENT_SINGLE_CLICK);
                sim_step(320);
                printf("[scene] 按键兜底(←): scroll_x=%d\n", tileview_scroll_x());
            }
        }
    }
    printf("[scene] !! ensure_page(%d) 未到位: scroll_x=%d\n", page_idx, tileview_scroll_x());
}

/** @brief 命中诊断：打印 (x,y) 处的对象链与顶层图层状态（排查"点不到"用） */
static void debug_hit_at(int x, int y)
{
    lv_point_t p = { (lv_coord_t)x, (lv_coord_t)y };
    printf("[scene] --- 命中诊断 (%d,%d) ---\n", x, y);

    lv_obj_t *top = lv_layer_top();
    printf("[scene] layer_top: clickable=%d children=%d\n", (int)lv_obj_is_clickable(top),
           (int)lv_obj_get_child_count(top));

    lv_obj_t *roots[2] = { top, lv_screen_active() };
    const char *names[2] = { "layer_top", "screen" };
    for (int r = 0; r < 2; r++) {
        lv_obj_t *o = roots[r];
        if (o == NULL) {
            continue;
        }
        printf("[scene] %s 链: ", names[r]);
        while (o != NULL) {
            lv_obj_t *next = NULL;
            uint32_t n = lv_obj_get_child_count(o);
            for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
                lv_obj_t *ch = lv_obj_get_child(o, i);
                if (lv_obj_is_hidden(ch)) {
                    continue;
                }
                if (lv_obj_hit_test(ch, &p)) {
                    next = ch;
                    break;
                }
            }
            o = next;
            if (o == NULL) {
                break;
            }
            lv_area_t a;
            lv_obj_get_coords(o, &a);
            const char *cls = "obj";
            if (lv_obj_check_type(o, &lv_label_class)) {
                cls = "label";
            } else if (lv_obj_check_type(o, &lv_button_class)) {
                cls = "button";
            } else if (lv_obj_check_type(o, &lv_slider_class)) {
                cls = "slider";
            }
            printf("%s(%d,%d)-(%d,%d)c%d -> ", cls, (int)a.x1, (int)a.y1, (int)a.x2,
                   (int)a.y2, (int)lv_obj_is_clickable(o));
        }
        printf("END\n");
    }
}

/** @brief 关闭当前打开的二级页（若存在"< 返回"则点击它；幂等） */
static void close_subpage_if_open(void)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *o = find_text("< 返回");
        if (o == NULL) {
            return; /* 已无二级页 */
        }
        lv_obj_t *btn = clickable_ancestor(o);
        if (btn == NULL) {
            printf("[scene] !! 返回按钮无可点击祖先\n");
            return;
        }
        click_obj(btn);
        sim_step(200);
    }
    printf("[scene] !! 二级页未能关闭\n");
    debug_hit_at(37, 38); /* 返回按钮中心：打印实际命中链 */
}

/** @brief 打开二级页：先确保回到首页（关闭残留浮层），再点击入口 */
static void open_sub_page(const char *entry_text)
{
    close_subpage_if_open();
    click_text(entry_text);
    sim_step(250);
}

/**
 * @brief 经由"显示设置"页切换屏幕角度（0/90/180/270）
 *
 * 点角度按钮后 lvgl_port 改分辨率 → ui_nav 下一帧按新分辨率重建三页
 * （二级页随旧页关闭），step 450ms 足以跑完重建；随后翻到 page_after
 * （0=测量 / 1=波形 / 2=设置），ensure_page 按当前页宽校正滚动位置。
 */
static void rotate_to_angle(int deg, int page_after)
{
    char label[8];
    snprintf(label, sizeof(label), "%d°", deg);
    ensure_page(2);
    open_sub_page("显示设置");
    click_text(label);
    sim_step(450);
    ensure_page(page_after);
}

/* ============================================================
 * 场景序列
 * ============================================================ */

void sim_run_scenes(void)
{
    printf("=== XT60 模拟器场景开始 ===\n");

    /* 与真机台架一致的初始数据 */
    sim_set_measurement(19.97670f, 0.018860f, 0.37683f, 0.11800f, 32.5f, 30.0f, 3920, 12345);

    /* ---- 01 启动页（横屏）---- */
    splash_build(lv_screen_active());
    splash_set_progress(45);
    splash_set_progress_text("正在初始化传感器…");
    shot("01_splash");

    /* ---- 01b 启动页（竖屏）：验证进度条居中、不与版本行冲突 ---- */
    lv_obj_clean(lv_screen_active());
    lvgl_port_set_rotation_angle(90);
    sim_step(100);
    splash_build(lv_screen_active());
    splash_set_progress(45);
    splash_set_progress_text("正在初始化传感器…");
    shot("01b_splash_90");
    lv_obj_clean(lv_screen_active());
    lvgl_port_set_rotation_angle(0);
    sim_step(100);

    /* ---- 三页主界面 ---- */
    ui_nav_build();
    sim_step(200);
    ui_nav_refresh(sim_get_measurement());
    shot("02_measure");

    /* 波形页（左滑切页） */
    swipe_next_page();
    ui_nav_refresh(sim_get_measurement());
    shot("03_wave");

    /* 波形设置滑板（K2 长按等价入口） */
    screen_wave_open_menu();
    sim_step(300);
    shot("04_wave_menu");
    if (screen_wave_menu_is_open()) {
        /* K1 长按 = 退出滑板（与真机按键语义一致） */
        screen_wave_menu_key(BUTTON_ID_BOOT, BUTTON_EVENT_LONG_PRESS_START);
        sim_step(300);
    }

    /* 设置页 */
    ensure_page(2);
    ui_nav_refresh(sim_get_measurement());
    shot("05_settings_home");

    /* ---- 按下态 ---- */
    press_text_snapshot("显示设置", "06_settings_pressed");

    /* ---- 显示设置 ---- */
    open_sub_page("显示设置");
    shot("07_display_settings");
    drag_slider_to_percent(25);
    shot("08_display_bright_low");
    /* 角度遍历：点角度按钮 → RESOLUTION_CHANGED → 三页按新分辨率重建，
     * 二级页随旧页关闭，故每次旋转后重新进入"显示设置"再截图/切下一个角度。 */
    click_text("90°");
    sim_step(400); /* 90° 竖屏 */
    open_sub_page("显示设置");
    shot("09_display_90");
    click_text("180°");
    sim_step(400); /* 180° 横屏倒 */
    open_sub_page("显示设置");
    shot("09b_display_180");
    click_text("270°");
    sim_step(400); /* 270° 竖屏倒 */
    open_sub_page("显示设置");
    shot("09c_display_270");
    click_text("0°");
    sim_step(500); /* 恢复 0° 横屏 */
    ensure_page(2);
    close_subpage_if_open();

    /* ---- 蜂鸣器 ---- */
    open_sub_page("蜂鸣器");
    shot("10_buzzer");
    drag_slider_to_percent(70);
    shot("11_buzzer_volume_70");
    click_text("试听");
    sim_step(200);
    close_subpage_if_open();

    /* ---- WiFi 路线图 ---- */
    open_sub_page("WiFi 与热点");
    shot("12_wifi_roadmap");
    close_subpage_if_open();

    /* ---- 电能统计与清零 ---- */
    open_sub_page("电能统计");
    ui_nav_refresh(sim_get_measurement());
    shot("13_energy");
    click_text("清零统计");
    sim_step(250);
    shot("14_energy_confirm");
    printf("[diag] 确认框弹出: 取消=%p 确定=%p\n", (void *)find_text("取消"),
           (void *)find_text("确定"));
    click_text("确定"); /* 确认清零 */
    sim_step(350);
    printf("[diag] 点确定后: 取消=%p 确定=%p\n", (void *)find_text("取消"),
           (void *)find_text("确定"));
    shot("15_energy_cleared");
    click_text("确定"); /* 关闭"已清零"提示 */
    sim_step(300);
    printf("[diag] 再点确定后: 确定=%p layer_children=%d clickable=%d\n",
           (void *)find_text("确定"), (int)lv_obj_get_child_count(lv_layer_top()),
           (int)lv_obj_is_clickable(lv_layer_top()));
    /* 清场回退：仍有对话框则用"点遮罩"路径关闭（产品实现的另一关闭方式） */
    for (int i = 0; i < 3 && lv_obj_get_child_count(lv_layer_top()) > 0; i++) {
        sim_click(275, 230);
        sim_step(250);
        printf("[diag] 遮罩关闭尝试 %d: layer_children=%d\n", i + 1,
               (int)lv_obj_get_child_count(lv_layer_top()));
    }
    ui_nav_refresh(sim_get_measurement());
    shot("16_energy_zeroed");
    close_subpage_if_open();

    /* ---- 关于本机 ---- */
    click_text("关于本机");
    sim_step(250);
    shot("17_about");
    click_text("确定");
    sim_step(250);

    /* ---- 标定向导（电压通道） ---- */
    open_sub_page("测量设置");
    shot("18_calib_menu");
    click_text("电压校准");
    sim_step(300);
    shot("19_calib_edit");
    click_text("＋");
    click_text("＋");
    sim_step(150);
    shot("20_calib_edited");
    click_text("应用校准");
    sim_step(300);
    shot("21_calib_done");
    sim_step(1800); /* 完成提示 1.4s 后自动返回首页 */

    /* ---- 四方向旋转重建走查（测量页 90/180/270 + 竖屏波形页） ---- */
    rotate_to_angle(90, 0);
    ui_nav_refresh(sim_get_measurement());
    shot("22_measure_90");

    rotate_to_angle(180, 0); /* 横屏倒：应与 0° 同布局，内容倒置 */
    ui_nav_refresh(sim_get_measurement());
    shot("28_measure_180");

    rotate_to_angle(270, 0); /* 竖屏倒：四行收窄布局 */
    ui_nav_refresh(sim_get_measurement());
    shot("29_measure_270");

    rotate_to_angle(90, 1); /* 竖屏波形页：图表加高到 170 */
    ui_nav_refresh(sim_get_measurement());
    shot("30_wave_90");

    /* 恢复 0° 横屏测量页（后续快速菜单场景在横屏执行） */
    rotate_to_angle(0, 0);
    ui_nav_refresh(sim_get_measurement());

    /* ---- 首页快速菜单（全程按键路径验证，不依赖触屏）---- */
    ensure_page(0); /* 回测量页 */
    ui_nav_refresh(sim_get_measurement());

    /* K2 长按打开（真机由 ui_nav 路由；场景直接投递等价事件） */
    ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_LONG_PRESS_START);
    sim_step(200);
    shot("24_measure_menu"); /* 初始焦点在「清除电能统计数据」行（橙边） */

    /* K2 单击：焦点下移到「数据刷新率」行 */
    ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_SINGLE_CLICK);
    sim_step(150);
    shot("25_measure_menu_focus_rate");

    /* K2 双击：激活焦点行 → 刷新率 5Hz → 10Hz（循环） */
    ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_DOUBLE_CLICK);
    sim_step(200);
    shot("26_measure_menu_10hz");
    if (ui_get_display_rate_hz() != 10) {
        printf("[scene] !! 按键循环刷新率失败：期望 10Hz，实际 %uHz\n",
               (unsigned)ui_get_display_rate_hz());
    }

    /* K2 单击：焦点回到清除行；K2 双击：执行清零，菜单应自动关闭 */
    ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_SINGLE_CLICK);
    sim_step(150);
    ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_DOUBLE_CLICK);
    sim_step(300);
    if (screen_measure_menu_is_open()) {
        printf("[scene] !! 清除电能后菜单未自动关闭\n");
    }
    ui_nav_refresh(sim_get_measurement());
    shot("27_measure_after_clear");

    /* 触屏路径回归：再次打开点档位 + K1 长按关闭 */
    screen_measure_open_menu();
    sim_step(150);
    click_text("30Hz");
    sim_step(200);
    if (ui_get_display_rate_hz() != 30) {
        printf("[scene] !! 触屏切 30Hz 失败\n");
    }
    ui_nav_handle_key(BUTTON_ID_BOOT, BUTTON_EVENT_LONG_PRESS_START); /* K1 长按关闭 */
    sim_step(250);
    ui_nav_refresh(sim_get_measurement());

    /* ---- 收尾：回到设置首页 ---- */
    ensure_page(2);
    ui_nav_refresh(sim_get_measurement());
    shot("23_settings_final");

    printf("=== 场景执行完成 ===\n");
}