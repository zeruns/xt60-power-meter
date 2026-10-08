/**
 * @file ui.c
 * @brief LVGL 界面层实现：启动画面 + 运行界面 + UI 任务
 *
 * 当前阶段：
 *   ① 启动画面：大字标题「XT60功率计」+ 副标题「作者博客：blog.zeruns.com」
 *      + 固件版本，停留若干秒；
 *   ② 随后进入 LVGL 官方 widgets demo（用于验证显示与触控、并显示 FPS/CPU）。
 *   正式界面就绪后，把 ② 替换为 src/screens/ 下的真实界面构建。
 *
 * 字体：中文用 lvgl_port 提供的 tiny_ttf 字体（子集化黑体，嵌入 Flash），
 *   可按需取任意字号（12/14/16/20/24/32/40），解决了内置 CJK 字体
 *   字形不全（率/博/客/软/件/版 等字显示为方框）和字号固定的问题。
 *
 * 任务约定：UI 任务绑定 Core 0（交互核），循环调用 lv_timer_handler()；
 * 所有 LVGL 访问（含本任务）都在 lvgl_port 的互斥锁保护下进行。
 * 数据只经 middleware/data_store 快照读取，不直接访问驱动层。
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_log.h"

#include "lv_demos.h"
#include "lvgl.h"

#include "lvgl_port.h"
#include "ui.h"

static const char *TAG = "ui";

/* 组件不得依赖 main 层的 APP_CORE_UI 宏，此处本地定义 UI 任务核绑定 */
#define UI_TASK_CORE 0
#define UI_TASK_NAME "ui"
#define UI_TASK_STACK_BYTES 8192 /**< xTaskCreate 栈单位为字节 */
#define UI_TASK_PRIORITY 12

#define UI_BUILD_LOCK_TIMEOUT_MS 5000 /**< 构建界面时获取 LVGL 锁的超时（构建较慢） */
#define UI_HANDLER_WAIT_MIN_MS 1     /**< lv_timer_handler 后最小让出时间 */
#define UI_HANDLER_WAIT_MAX_MS 500   /**< lv_timer_handler 后最大让出时间 */
#define UI_SPLASH_HOLD_MS 3000       /**< 启动画面停留时长 */

static bool s_ui_inited = false; /**< 防重复初始化标志 */

/**
 * @brief 构建启动画面（白底 + 大字标题 + 博客 + 固件版本，垂直居中）
 *
 * 设计要点：
 *   · 白底深色字，视觉干净、对比度高；
 *   · 标题用 40px 黑体，足够醒目；副标题 16px 中灰；版本 14px 浅灰，
 *     靠字号与颜色拉开层次；
 *   · 容器占满屏幕（LV_PCT 100%），flex 纵向居中，天然适配横竖屏——
 *     屏幕旋转后分辨率变化，内容始终居中；
 *   · 版本号取 ESP-IDF 应用描述（来自 version.txt / CMake 的 PROJECT_VER）。
 */
static void build_splash(void)
{
    lv_obj_t *scr = lv_screen_active();
    /* 纯白背景 */
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 14, 0);

    /* ① 大字标题（40px 黑体，近黑色） */
    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "XT60功率计");
    lv_obj_set_style_text_font(title, lvgl_port_get_font(LVGL_PORT_FONT_40), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A1A1A), 0);

    /* ② 副标题：作者博客（16px，中灰） */
    lv_obj_t *blog = lv_label_create(cont);
    lv_label_set_text(blog, "作者博客：blog.zeruns.com");
    lv_obj_set_style_text_font(blog, lvgl_port_get_font(LVGL_PORT_FONT_16), 0);
    lv_obj_set_style_text_color(blog, lv_color_hex(0x666666), 0);

    /* ③ 固件版本（14px，浅灰） */
    lv_obj_t *ver = lv_label_create(cont);
    lv_label_set_text_fmt(ver, "固件版本 %s", esp_app_get_description()->version);
    lv_obj_set_style_text_font(ver, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x999999), 0);

    ESP_LOGI(TAG, "启动画面已构建（版本 %s）", esp_app_get_description()->version);
}

/**
 * @brief UI 任务：先显示启动画面，再进入 demo 并驱动 LVGL 定时器
 *
 * 为什么界面构建放在**本任务**而不是 ui_init() 里：
 *   ui_init() 由 app_tasks_start() 调用，运行在 app_main 的任务上，其栈只有
 *   CONFIG_ESP_MAIN_TASK_STACK_SIZE（本工程 3584 字节）。而 LVGL 界面构建会
 *   递归创建大量对象，很容易把主任务栈吃满导致溢出复位。
 *   本任务栈固定 8192 字节，且有明确的职责边界，放在这里最安全。
 *
 * 构建期间必须持 LVGL 锁：此后 UI 任务与其它访问 LVGL 的任务会并发。
 *
 * lv_timer_handler() 返回距下次需要调度的毫秒数，据此让出 CPU；
 * 限制在 [UI_HANDLER_WAIT_MIN_MS, UI_HANDLER_WAIT_MAX_MS] 内。
 */
static void task_ui(void *arg)
{
    (void)arg;

    /* ---- ① 启动画面：构建 → 持续渲染 UI_SPLASH_HOLD_MS ----
     * ⚠ 关键：LVGL 只在 lv_timer_handler() 里真正渲染，创建对象本身不上屏。
     *   所以「停留 N 秒」绝不能写成 vTaskDelay(N)——那样这段时间没有任何
     *   渲染，屏幕上只会出现首轮刷新的残块（曾表现为一条白色横带）。
     *   正确做法是边跑 lv_timer_handler 边计时。 */
    if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
        build_splash();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "构建启动画面时获取 LVGL 锁失败");
    }

    uint32_t elapsed_ms = 0;
    while (elapsed_ms < UI_SPLASH_HOLD_MS) {
        uint32_t wait_ms = UI_HANDLER_WAIT_MAX_MS;
        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            wait_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (wait_ms < UI_HANDLER_WAIT_MIN_MS) {
            wait_ms = UI_HANDLER_WAIT_MIN_MS;
        } else if (wait_ms > UI_HANDLER_WAIT_MAX_MS) {
            wait_ms = UI_HANDLER_WAIT_MAX_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        elapsed_ms += wait_ms;
    }

    /* ---- ② 进入 widgets demo（正式界面就绪后替换此处） ---- */
    if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
        lv_obj_clean(lv_screen_active()); /* 清掉启动画面对象 */
        lv_demo_widgets();
        lvgl_port_unlock();
        ESP_LOGI(TAG, "进入 LVGL widgets demo");
    } else {
        ESP_LOGE(TAG, "构建 demo 时获取 LVGL 锁失败，界面保持启动画面");
    }

    /* ---- ③ 循环驱动 LVGL ---- */
    for (;;) {
        uint32_t wait_ms = UI_HANDLER_WAIT_MAX_MS;

        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            wait_ms = lv_timer_handler();
            lvgl_port_unlock();
        }

        if (wait_ms < UI_HANDLER_WAIT_MIN_MS) {
            wait_ms = UI_HANDLER_WAIT_MIN_MS;
        } else if (wait_ms > UI_HANDLER_WAIT_MAX_MS) {
            wait_ms = UI_HANDLER_WAIT_MAX_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t ui_init(void)
{
    if (s_ui_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 只创建任务；界面构建在任务内部完成（原因见 task_ui 注释）。
     * 调用方（app_tasks_start）持有的门控条件是「显示链路初始化成功」，
     * 界面能否构建成功由本任务自行记录日志。 */
    BaseType_t ok = xTaskCreatePinnedToCore(task_ui, UI_TASK_NAME,
                                            UI_TASK_STACK_BYTES, NULL,
                                            UI_TASK_PRIORITY, NULL,
                                            UI_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 UI 任务失败（栈 %d 字节）", UI_TASK_STACK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    s_ui_inited = true;
    ESP_LOGI(TAG, "UI 任务已启动（Core %d，栈 %d 字节，界面在任务内构建）",
             UI_TASK_CORE, UI_TASK_STACK_BYTES);
    return ESP_OK;
}
