/**
 * @file ui_msgbox.c
 * @brief 全局共享模态对话框实现（v0.6.8 自 screen_settings.c 原样上移）
 *
 * 实现要点（历史踩坑都在注释里，改前先读）：
 * ① 顶层图层可点击技巧：对话框挂在 lv_layer_top() 上，但顶层图层默认
 *    不参与命中测试，里面的按钮点不动——弹框时置 clickable，框全关后
 *    下一帧异步恢复（layer_restore_check_cb，避开 DELETE 事件的时序陷阱）。
 * ② 压暗遮罩（scrim）：黑底 40% 不透明度铺满屏幕，模态感；不可点击，
 *    点遮罩空白处的事件落到顶层图层本体，由 layer_clicked_cb 关闭对话框。
 * ③ 显式关闭双保险：LVGL 9.6 的 footer 按钮**没有**内建关闭行为，
 *    取消/确定都显式挂回调调 lv_msgbox_close，不依赖组件内建行为。
 * ④ 叠窗防御：任意时刻至多一个对话框——弹新框前先同步关旧框。
 *    历史卡死 bug：清零流程先弹"正在清零"再弹"已清零"，两框叠放时
 *    单例 scrim/active 指针被新框覆盖，旧框成孤儿永远关不掉
 *    （点确定后仍压暗、界面卡死）。close_active_msgbox 从入口根治。
 * ⑤ 同步关闭统一出口：点遮罩（本文件）与按键（ui_msgbox_key）都走
 *    同步 lv_msgbox_close——一同步一异步会让排队的异步回调摸到悬空指针。
 */
#include "ui_msgbox.h"

#include <string.h>

#include "lvgl_port.h"

/* ============================================================
 * 模块状态（同一时刻至多一个对话框，单例指针即可）
 * ============================================================ */
static bool s_layer_hooked = false;     /**< 顶层图层关闭回调是否已安装（只装一次） */
static lv_obj_t *s_msgbox_scrim = NULL; /**< 当前对话框的压暗遮罩（随框删除） */
static lv_obj_t *s_active_msgbox = NULL; /**< 当前打开的对话框（NULL=无；按键路由据此判模态） */

static void close_active_msgbox(void); /* 前向声明：弹新框前关旧框，防叠窗 */
static void notice_ok_cb(lv_event_t *e); /* 前向声明："确定/取消"共用的关框回调 */

/* ============================================================
 * 顶层图层生命周期
 * ============================================================ */

/**
 * @brief 点击顶层图层空白处 → 关闭当前对话框（点遮罩关闭）
 *
 * 用**同步** lv_msgbox_close 而非 close_async：按键路径（ui_msgbox_key →
 * msgbox_key_action）也是同步关闭，两条路径若一同步一异步，异步排队的
 * 回调可能在对象已被另一路径删除后才执行（悬空指针）。统一同步关闭后
 * 无论点遮罩、点按钮还是按 K1/K2，都走同一个立即关闭出口。
 *
 * @param e LVGL 点击事件（目标是顶层图层 lv_layer_top()）
 */
static void layer_clicked_cb(lv_event_t *e)
{
    lv_obj_t *layer = lv_event_get_current_target(e);
    /* lv_obj_get_child_count()：数一数容器里现有几个子对象，用来判断
     * "还有没有对话框开着"（对话框就是顶层图层的最后一个子对象）。 */
    uint32_t n = lv_obj_get_child_count(layer);
    if (n > 0) {
        /* lv_obj_get_child(layer, n-1) 取最后创建的子对象 = 当前对话框 */
        lv_msgbox_close(lv_obj_get_child(layer, n - 1));
    }
}

/**
 * @brief 延迟一帧复查：图层已无子对象则恢复"不可点击"
 *
 * 时序说明：LV_EVENT_DELETE 触发时，被删对象**可能仍留在父对象的子列表中**，
 * 此刻直接查 child_count 会拿到非 0 值，导致顶层图层永远保持 clickable、
 * 把后续所有触摸全部吞掉（表现为"对话框关掉后返回按钮点不动"）。
 * 因此把复查放到下一帧（lv_async_call）执行。
 *
 * @param layer_ptr 顶层图层指针（void* 形参是为了匹配 lv_async_call 的回调签名）
 */
static void layer_restore_check_cb(void *layer_ptr)
{
    lv_obj_t *layer = (lv_obj_t *)layer_ptr;
    if (layer != NULL && lv_obj_get_child_count(layer) == 0) {
        /* lv_obj_set_clickable(false)：让对象不再参与触摸命中测试，
         * 触摸会"穿过"它落到下层页面控件上（否则整个页面被图层吞掉点击）。 */
        lv_obj_set_clickable(layer, false);
    }
}

/**
 * @brief 对话框被删除 → 删除压暗遮罩 + 安排一次延迟复查（恢复图层不可点击）
 *
 * 同时清 s_active_msgbox：弹窗句柄随对象一起失效，按键路由（ui_nav 的
 * ui_msgbox_is_open 门）靠它判断"此刻有没有弹窗要接管按键"，不清理会
 * 一直误判为模态。
 *
 * @param e LVGL 删除事件；user_data 携带顶层图层指针
 */
static void box_deleted_cb(lv_event_t *e)
{
    lv_obj_t *box = lv_event_get_target(e); /* 被删的对话框本体 */
    lv_obj_t *layer = (lv_obj_t *)lv_event_get_user_data(e);
    if (s_active_msgbox == box) {
        s_active_msgbox = NULL;
    }
    if (s_msgbox_scrim != NULL) {
        /* lv_obj_delete()：立即删除遮罩对象（连同其样式一起释放） */
        lv_obj_delete(s_msgbox_scrim);
        s_msgbox_scrim = NULL;
    }
    if (layer != NULL) {
        /* lv_async_call()：把复查函数挂到下一帧的异步队列执行，
         * 避开"删除事件触发时对象还在子列表里"的时序陷阱（见上方注释） */
        lv_async_call(layer_restore_check_cb, layer);
    }
}

/* ============================================================
 * 对话框构建
 * ============================================================ */

/**
 * @brief 把对话框里的文字全部换成中文字体（主题字体已是 Noto，这里是双保险）
 *
 * @param box lv_msgbox_create 创建的对话框对象
 */
static void style_msgbox(lv_obj_t *box)
{
    /* lv_msgbox_get_title / get_content / get_footer：分别取对话框的
     * 标题栏、正文区、底部按钮区三段的子对象（LVGL 9 的 msgbox 由这三块组成）。 */
    lv_obj_t *title = lv_msgbox_get_title(box);
    if (title != NULL) {
        lv_obj_set_style_text_font(title, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    }
    lv_obj_t *content = lv_msgbox_get_content(box);
    if (content != NULL) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++) {
            lv_obj_set_style_text_font(lv_obj_get_child(content, i),
                                       lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(content, i), lv_color_hex(0x374151),
                                        0);
        }
    }
    lv_obj_t *footer = lv_msgbox_get_footer(box);
    if (footer != NULL) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(footer); i++) {
            lv_obj_t *btn = lv_obj_get_child(footer, i);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl != NULL) {
                lv_obj_set_style_text_font(lbl, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
            }
            lv_obj_set_style_min_width(btn, 84, 0);
        }
    }
}

/**
 * @brief 创建挂到顶层图层上的模态框（公共部分）
 *
 * @param title 标题文本
 * @param text  正文文本
 * @return 创建好的 lv_msgbox 对象（已加标题/正文并居中，按钮由调用方补）
 */
static lv_obj_t *msgbox_begin(const char *title, const char *text)
{
    /* 叠窗防御（一键清零卡死修复）：已有活动弹窗先同步关闭，保证任意时刻
     * 至多一个弹窗——否则单例 scrim/active 指针被新框覆盖，旧框成孤儿。 */
    close_active_msgbox();

    /* lv_layer_top()：LVGL 的"顶层图层"，永远画在所有页面之上。
     * 对话框挂在这里，翻页/滚动都不会把它带走，天然呈现模态效果。 */
    lv_obj_t *layer = lv_layer_top();

    /* 关键：让顶层图层参与命中测试，否则里面的按钮点不动（见原实现注释） */
    lv_obj_set_clickable(layer, true);
    if (!s_layer_hooked) {
        s_layer_hooked = true;
        lv_obj_add_event_cb(layer, layer_clicked_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 压暗遮罩：先于对话框创建（渲染在下层），模态感 + 突出对话框。
     * 不可点击 → 命中测试穿透到顶层图层本体，点遮罩空白处仍走
     * layer_clicked_cb 关闭对话框（与原交互一致）。
     * lv_obj_remove_style_all()：清掉主题默认样式，得到一块干净的"画布"；
     * lv_obj_set_style_bg_opa(..., LV_OPA_40, 0) 把黑色底调到 40% 不透明度，
     * 即"压暗 60%"，视觉上提示"背后页面暂时不可用"。 */
    lv_obj_t *scrim = lv_obj_create(layer);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100)); /* LV_PCT(100)：占父对象满宽/满高 */
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
    lv_obj_set_clickable(scrim, false);
    s_msgbox_scrim = scrim;

    /* lv_msgbox_create()：LVGL 9 内置对话框部件，自带标题/正文/按钮区三段结构 */
    lv_obj_t *box = lv_msgbox_create(layer);
    lv_obj_set_width(box, 250); /* 固定宽度，避免长文本撑到圆角外 */
    lv_msgbox_add_title(box, title); /* 往对话框里追加标题段 */
    lv_msgbox_add_text(box, text);   /* 往对话框里追加正文段 */
    style_msgbox(box);
    lv_obj_center(box); /* 相对父对象（顶层图层）居中 */

    /* 对话框删除后恢复图层可点击性 */
    lv_obj_add_event_cb(box, box_deleted_cb, LV_EVENT_DELETE, layer);
    s_active_msgbox = box; /* 记录当前弹窗：按键路由据此把 K1/K2 让给弹窗 */
    return box;
}

/**
 * @brief 从 footer 按钮向上找到所属 msgbox 并**显式关闭**（幂等）
 *
 * 防御式设计：对话框的关闭不依赖 LVGL footer 按钮的内建关闭行为——
 * 若该行为在某种情况下失效，对话框将一直留在顶层图层上，把后续所有触摸
 * 全部吞掉（表现为"对话框关不掉、返回按钮也点不动"）。显式关闭双保险。
 *
 * @param btn 被点击的 footer 按钮（沿父链向上找到所属 msgbox）
 */
static void msgbox_close_from_btn(lv_obj_t *btn)
{
    /* 从按钮一路向上找父对象，直到命中 lv_msgbox 的类对象为止。
     * lv_obj_check_type(o, &lv_msgbox_class)：判断对象是不是 msgbox 类型；
     * lv_obj_get_parent(o)：取直接父对象。 */
    lv_obj_t *o = btn;
    while (o != NULL && !lv_obj_check_type(o, &lv_msgbox_class)) {
        o = lv_obj_get_parent(o);
    }
    if (o != NULL) {
        /* lv_msgbox_close(o)：同步关闭（立即删除）该对话框 */
        lv_msgbox_close(o);
    }
}

/** @brief 单按钮提示框："确定"点击 → 显式关闭
 *  @param e LVGL 点击事件（目标是"确定"按钮） */
static void notice_ok_cb(lv_event_t *e)
{
    msgbox_close_from_btn(lv_event_get_target(e)); /* lv_event_get_target：取触发事件的对象 */
}

/**
 * @brief 同步关闭当前活动弹窗（无则空操作）
 *
 * 同步关闭：box_deleted_cb 随之清理压暗遮罩与 s_active_msgbox。
 * 【一键清零卡死修复 2026-10】清零流程先弹"正在清零"，1 秒后再弹"已清零"
 * ——两框叠放时 s_active_msgbox/s_msgbox_scrim（单例指针）被新框覆盖，
 * 旧框与其遮罩成孤儿永远关不掉（点确定后仍压暗、界面卡死）。
 * 弹新框前先经本函数关旧框，叠窗问题从入口根治。
 */
static void close_active_msgbox(void)
{
    if (s_active_msgbox != NULL) {
        lv_msgbox_close(s_active_msgbox); /* 同步删除，回调自动清 scrim/句柄 */
    }
}

/**
 * @brief 确认框"确定"点击 → 先关本框，再执行业务动作
 *
 * @param e LVGL 点击事件；user_data 携带业务回调 ui_confirm_cb_t
 */
static void confirm_ok_cb(lv_event_t *e)
{
    /* lv_event_get_user_data()：取出注册事件时塞进去的自定义指针（这里是业务回调） */
    ui_confirm_cb_t cb = (ui_confirm_cb_t)lv_event_get_user_data(e);
    /* 先关闭本框（同步删除，立即生效），再执行业务动作（动作可能弹新框） */
    msgbox_close_from_btn(lv_event_get_target(e));
    if (cb != NULL) {
        cb();
    }
}

/* ============================================================
 * 公共 API（见 ui_msgbox.h）
 * ============================================================ */

void ui_msgbox_notice(const char *title, const char *text)
{
    lv_obj_t *box = msgbox_begin(title, text);
    /* lv_msgbox_add_footer_button()：在对话框底部按钮区追加一个按钮 */
    lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确定");
    lv_obj_add_event_cb(ok, notice_ok_cb, LV_EVENT_CLICKED, NULL); /* 点击触发关闭 */
    style_msgbox(box); /* footer 按钮后补的子对象也要过一遍字体设置 */
}

void ui_msgbox_confirm(const char *title, const char *text, ui_confirm_cb_t ok_cb)
{
    lv_obj_t *box = msgbox_begin(title, text);
    /* ⚠ LVGL 9.6 的 footer 按钮**没有**内建关闭行为（只有 add_close_button 才挂
     * 关闭回调），"取消"必须显式挂关闭回调，否则点它/按 K1 都关不掉框。
     * notice_ok_cb 的语义就是"关框不动作"，与"取消"一致，直接复用。 */
    lv_obj_t *cancel = lv_msgbox_add_footer_button(box, "取消");
    lv_obj_add_event_cb(cancel, notice_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确定");
    lv_obj_add_event_cb(ok, confirm_ok_cb, LV_EVENT_CLICKED, (void *)ok_cb); /* 业务回调经 user_data 传入 */
    style_msgbox(box);
}

void ui_msgbox_close_active(void)
{
    close_active_msgbox();
}

bool ui_msgbox_is_open(void)
{
    return s_active_msgbox != NULL;
}

/**
 * @brief 弹窗按键动作（静态）：K1=取消 / K2=确定（固定键位，无需焦点记忆）
 *
 * footer 按钮即 msgbox 底部按钮区的子对象（"取消"在前、"确定"在后，
 * 见 ui_msgbox_confirm / ui_msgbox_notice 的创建顺序）。激活方式是合成一次
 * LV_EVENT_CLICKED，注册回调（含 user_data）照常执行，与手指点击完全等价。
 * 确定动作可能再弹结果提示框（如"已清零"），s_active_msgbox 会随之更新，
 * 调用方拿它当"当前弹窗"用是安全的。
 *
 * @param id K1=BUTTON_ID_BOOT → 取消；K2=BUTTON_ID_USER → 确定
 */
static void msgbox_key_action(button_id_t id)
{
    lv_obj_t *box = s_active_msgbox;
    if (box == NULL) {
        return;
    }
    lv_obj_t *footer = lv_msgbox_get_footer(box); /* 底部按钮区（可能为 NULL） */
    uint32_t n = (footer != NULL) ? lv_obj_get_child_count(footer) : 0;
    if (n == 0) {
        /* 无按钮的异常框：任一键直接关闭，绝不把用户困在弹窗里 */
        lv_msgbox_close(box);
        return;
    }
    if (id == BUTTON_ID_USER) {
        /* K2 = 确定 = footer 最后一个按钮 */
        lv_obj_send_event(lv_obj_get_child(footer, n - 1), LV_EVENT_CLICKED, NULL);
    } else {
        /* K1 = 取消 = footer 第一个按钮（单按钮提示框时它就是"确定"，
         * 点了同样关闭——提示框只求"看完能走"，两个键都认） */
        lv_obj_send_event(lv_obj_get_child(footer, 0), LV_EVENT_CLICKED, NULL);
    }
}

void ui_msgbox_key(button_id_t id, button_event_t ev)
{
    /* 只有"单击/长按起始"触发动作；长按松开/双击等事件吞掉不外漏
     * （防止长按结束的 RELEASE 落到翻页逻辑——原设置页同名逻辑照搬） */
    if (ev == BUTTON_EVENT_SINGLE_CLICK || ev == BUTTON_EVENT_LONG_PRESS_START) {
        msgbox_key_action(id);
    }
}
