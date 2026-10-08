/**
 * @file lvgl_port.c
 * @brief LVGL 适配层实现：显示 flush、触摸输入、心跳节拍、线程安全锁
 *
 * 设计要点：
 * - 显示：横屏逻辑 284x240（面板物理 240x284，由 MADCTL=0x68 硬旋转），
 *   颜色格式 **RGB565 big-endian（LV_COLOR_FORMAT_RGB565_SWAPPED）**，
 *   LVGL 直接渲染成面板线上所需字节序，单缓冲 PARTIAL。
 *   ⚠ esp_lcd 对颜色数据的 SPI 传输是**异步**的（内部 spi_device_queue_trans），
 *   esp_lcd_panel_io_tx_color() 返回时 DMA 并未结束。因此**不能**在
 *   lcd_nv3030b_draw_bitmap() 返回后立刻调用 lv_display_flush_ready()——
 *   那会让 LVGL 马上复用同一块绘图缓冲，与仍在跑的 DMA 抢内存，表现为
 *   画面撕裂、花屏甚至整屏黑。正确做法是注册
 *   lcd_nv3030b_register_flush_done_cb()，在传输真正完成后（SPI 中断里）
 *   再 flush_ready。单缓冲 PARTIAL 模式必须依赖这一点才安全。
 * - 触摸：CST816D 输出竖屏物理坐标，需变换到横屏逻辑坐标（见 read_cb 注释）。
 * - 心跳：esp_timer 周期调用 lv_tick_inc()。
 * - 锁：任何非 UI 任务访问 LVGL API 前必须 lvgl_port_lock()。
 *
 * 本组件不创建 LVGL 任务；UI 任务由 services/ui 创建与调度。
 */
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"

#include "lcd_nv3030b.h"
#include "lvgl_port.h"
#include "touch_cst816d.h"

/* 嵌入的子集化中文字体（由 EMBED_FILES 自动生成符号） */
extern const uint8_t notosanssc_subset_ttf_start[] asm("_binary_notosanssc_subset_ttf_start");
extern const uint8_t notosanssc_subset_ttf_end[]   asm("_binary_notosanssc_subset_ttf_end");

static const char *TAG = "lvgl_port";

/** LVGL 心跳周期（毫秒） */
#define LVGL_PORT_TICK_MS 5
/** flush 缓冲行数（PARTIAL 模式按行刷新，40 行兼顾内存占用与刷新效率） */
#define LVGL_PORT_BUF_ROWS 40

static SemaphoreHandle_t s_lvgl_mutex = NULL; /**< LVGL 访问互斥锁 */
static esp_timer_handle_t s_tick_timer = NULL; /**< 心跳定时器 */
static lv_display_t *s_display = NULL;         /**< 显示设备 */
static lv_indev_t *s_indev = NULL;             /**< 输入设备 */
static void *s_draw_buf = NULL;                /**< 渲染缓冲（DMA 内部 RAM） */
static bool s_inited = false;                  /**< 防重复初始化标志 */

/** 当前显示旋转档位（与面板 MADCTL 保持同步；触摸映射按它查表） */
static lcd_nv3030b_rotation_t s_rotation = LCD_NV3030B_ROT_90;

/* ---- 中文字体（tiny_ttf） ---- */
/** 各字号对应的像素高度（与枚举 lvgl_port_font_size_t 一一对应） */
static const int s_font_px[LVGL_PORT_FONT_MAX] = { 12, 14, 16, 20, 24, 32, 40 };
/** 已创建的字体指针（NULL 表示该字号未创建） */
static lv_font_t *s_fonts[LVGL_PORT_FONT_MAX] = { 0 };

/**
 * @brief 显存写入完成回调：通知 LVGL「绘图缓冲已可复用」
 *
 * 这是唯一正确调用 lv_display_flush_ready() 的位置，理由见文件头说明：
 * esp_lcd 的颜色传输是异步的，只有此回调触发时才代表 DMA 真正结束。
 * 该回调在 SPI 中断上下文执行，故只做一个标志置位操作，不申请锁、
 * 不阻塞（LVGL 的 flush 状态位是原子的，符合 ISR 使用要求）。
 *
 * @param user_ctx 注册时传入的显示设备指针
 */
static void flush_done_cb(void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
}

/**
 * @brief LVGL 显示 flush 回调：把渲染好的区域写入面板
 *
 * 字节序由显示格式统一解决：显示设备配的是 LV_COLOR_FORMAT_RGB565_SWAPPED，
 * LVGL 渲染出的 px_map 已经是面板线上要求的「高字节在前」big-endian，
 * 因此这里**直接发送、不做任何软件字节交换**（不要再加 swap_color_bytes，
 * 否则等于交换两次、颜色反而错乱）。
 */
static void display_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_err_t err = lcd_nv3030b_draw_bitmap(area->x1, area->y1,
                                            area->x2 + 1, area->y2 + 1, px_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写显存失败: %s", esp_err_to_name(err));
        /* 提交失败时不会有传输完成回调，必须在此处补一次，
         * 否则 LVGL 会一直等这块区域、整个界面卡住不再刷新。 */
        lv_display_flush_ready(disp);
    }
    /* 成功路径由 flush_done_cb() 在 DMA 完成后通知，见其注释 */
}

/**
 * @brief 触摸坐标映射参数（按旋转档位）
 *
 * 触摸 IC（CST816D）输出的是**物理玻璃坐标** (px∈0..239, py∈0..283)，
 * 它不随显示方向变化；而显示方向由面板 MADCTL 决定。因此每个旋转档位都需要
 * 一组「交换/翻转」参数，把物理坐标换算成当前逻辑坐标：
 *
 *   x0 = swap_xy ? py : px
 *   y0 = swap_xy ? px : py
 *   lx = flip_x  ? (逻辑宽-1 - x0) : x0
 *   ly = flip_y  ? (逻辑高-1 - y0) : y0
 *
 * ⚠ ROT_90（当前默认）的三项参数是**上板实测**校正过的：
 *   初版用 swap=T / flip_x=F / flip_y=T 时表现为左右镜像（用户实测反馈），
 *   改为 flip_x=T 后左右方向正确。
 *   若换档位后方向不对，只改这张表即可，不要去动面板 MADCTL。
 *   （各档位的理论值按标准 MIPI 推导，非默认档位首次使用需上板确认。）
 */
typedef struct {
    bool swap_xy; /**< 是否交换 px/py（横屏为 true） */
    bool flip_x;  /**< 逻辑 X 是否反向 */
    bool flip_y;  /**< 逻辑 Y 是否反向 */
} touch_map_t;

static const touch_map_t s_touch_map[4] = {
    [LCD_NV3030B_ROT_0] = { false, false, false },  /* 竖屏 240x284 */
    [LCD_NV3030B_ROT_90] = { true, true, true },    /* 横屏 284x240（已上板校正） */
    [LCD_NV3030B_ROT_180] = { false, true, true },  /* 竖屏倒置 */
    [LCD_NV3030B_ROT_270] = { true, false, false }, /* 横屏倒置 */
};

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    touch_cst816d_point_t tp;
    esp_err_t err = touch_cst816d_read(&tp);
    if (err != ESP_OK || !tp.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* 按当前旋转档位把物理坐标换算成逻辑坐标 */
    const touch_map_t *m = &s_touch_map[s_rotation];
    int lw = 0;
    int lh = 0;
    lcd_nv3030b_get_resolution(s_rotation, &lw, &lh);

    const int x0 = m->swap_xy ? (int)tp.y : (int)tp.x;
    const int y0 = m->swap_xy ? (int)tp.x : (int)tp.y;
    int lx = m->flip_x ? (lw - 1 - x0) : x0;
    int ly = m->flip_y ? (lh - 1 - y0) : y0;

    /* 裁剪到当前逻辑分辨率 */
    if (lx < 0) {
        lx = 0;
    } else if (lx > lw - 1) {
        lx = lw - 1;
    }
    if (ly < 0) {
        ly = 0;
    } else if (ly > lh - 1) {
        ly = lh - 1;
    }

    data->point.x = (lv_coord_t)lx;
    data->point.y = (lv_coord_t)ly;
    data->state = LV_INDEV_STATE_PRESSED;
}

/**
 * @brief 心跳定时器回调：推进 LVGL 时基
 */
static void tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_PORT_TICK_MS);
}

/**
 * @brief 初始化失败时释放已申请的资源
 */
static void deinit_resources(void)
{
    if (s_tick_timer != NULL) {
        esp_timer_stop(s_tick_timer);
        esp_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
    }
    if (s_draw_buf != NULL) {
        free(s_draw_buf);
        s_draw_buf = NULL;
    }
    if (s_lvgl_mutex != NULL) {
        vSemaphoreDelete(s_lvgl_mutex);
        s_lvgl_mutex = NULL;
    }
    s_display = NULL;
    s_indev = NULL;
}

esp_err_t lvgl_port_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 互斥锁（后续所有 LVGL 访问的线程安全边界） */
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (s_lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "创建互斥锁失败");
        deinit_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 2. 渲染缓冲：QSPI DMA 要求内部 RAM，PARTIAL 模式单缓冲。
     *    按面板最长边（284）分配整行，保证任意旋转档位下都是完整行。 */
    size_t buf_size = (size_t)LCD_NV3030B_PHYS_H * LVGL_PORT_BUF_ROWS * 2;
    s_draw_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_draw_buf == NULL) {
        ESP_LOGE(TAG, "分配渲染缓冲失败 (%u 字节)", (unsigned)buf_size);
        deinit_resources();
        return ESP_ERR_NO_MEM;
    }

    /* 3. LVGL 内核 */
    lv_init();

    /* 3.1 中文字体：从嵌入的子集化 TTF 创建各字号字体。
     *     tiny_ttf 会按需光栅化字形并缓存；字号在 init 阶段一次性建好，
     *     UI 层直接取指针，避免运行时反复创建。 */
    {
        const uint8_t *ttf_data = notosanssc_subset_ttf_start;
        size_t ttf_size = (size_t)(notosanssc_subset_ttf_end - notosanssc_subset_ttf_start);
        for (int i = 0; i < LVGL_PORT_FONT_MAX; i++) {
            s_fonts[i] = lv_tiny_ttf_create_data(ttf_data, ttf_size, s_font_px[i]);
            if (s_fonts[i] == NULL) {
                ESP_LOGW(TAG, "创建 %dpx 中文字体失败", s_font_px[i]);
            }
        }
        ESP_LOGI(TAG, "中文字体已加载（TTF %u 字节，%d 个字号）",
                 (unsigned)ttf_size, LVGL_PORT_FONT_MAX);
    }

    /* 4. 显示设备：按当前旋转档位的逻辑分辨率创建（默认 ROT_90 横屏 284x240）
     *
     * 颜色格式用 **RGB565_SWAPPED（big-endian）**：面板 QSPI 线上要求每像素
     * 高字节在前（厂商 STM32 例程先发 Color>>8）。让 LVGL 直接渲染成该字节序，
     * 无需软件交换；用普通小端 RGB565 会整体偏色/列错位。
     *
     * 面板方向由 MADCTL 决定（init 序列已写 ROT_90=0x68），此处不再重复设置。 */
    int lw = 0;
    int lh = 0;
    lcd_nv3030b_get_resolution(s_rotation, &lw, &lh);
    s_display = lv_display_create(lw, lh);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "创建显示设备失败");
        deinit_resources();
        return ESP_FAIL;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(s_display, display_flush_cb);
    lv_display_set_buffers(s_display, s_draw_buf, NULL, (uint32_t)buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* 4.1 注册显存写入完成回调（必须在第一次 flush 之前）：
     * 单缓冲 PARTIAL 模式下，只有它才能在 DMA 真正结束后放行 LVGL 复用缓冲，
     * 否则会与 DMA 抢内存导致画面错乱/黑屏，详见文件头与 flush_done_cb 注释。 */
    esp_err_t err = lcd_nv3030b_register_flush_done_cb(flush_done_cb, s_display);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册显存完成回调失败: %s", esp_err_to_name(err));
        deinit_resources();
        return err;
    }

    /* 5. 输入设备：电容触摸（指针类型） */
    s_indev = lv_indev_create();
    if (s_indev == NULL) {
        ESP_LOGE(TAG, "创建输入设备失败");
        deinit_resources();
        return ESP_FAIL;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, indev_read_cb);

    /* 6. 心跳定时器 */
    const esp_timer_create_args_t timer_args = {
        .callback = tick_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&timer_args, &s_tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建心跳定时器失败: %s", esp_err_to_name(err));
        deinit_resources();
        return err;
    }
    err = esp_timer_start_periodic(s_tick_timer, LVGL_PORT_TICK_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动心跳定时器失败: %s", esp_err_to_name(err));
        deinit_resources();
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "LVGL 适配层就绪（显示 %dx%d ROT_%d，RGB565 big-endian，缓冲 %u 字节）", lw, lh,
             s_rotation * 90, (unsigned)buf_size);
    return ESP_OK;
}

esp_err_t lvgl_port_set_rotation(lcd_nv3030b_rotation_t rot)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rot > LCD_NV3030B_ROT_270) {
        return ESP_ERR_INVALID_ARG;
    }

    int lw = 0;
    int lh = 0;
    lcd_nv3030b_get_resolution(rot, &lw, &lh);

    /* ① 先改面板方向（写 MADCTL） */
    esp_err_t err = lcd_nv3030b_set_rotation(rot);
    if (err != ESP_OK) {
        return err;
    }

    /* ② 记录档位：触摸映射立即按新档位生效 */
    s_rotation = rot;

    /* ③ 同步 LVGL 显示分辨率（LVGL 9 会广播 RESOLUTION_CHANGED，界面自动重排） */
    if (lvgl_port_lock(1000)) {
        lv_display_set_resolution(s_display, lw, lh);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "旋转后获取 LVGL 锁超时，显示分辨率未同步");
    }

    ESP_LOGI(TAG, "显示旋转已切换为 ROT_%d（%dx%d）", rot * 90, lw, lh);
    return ESP_OK;
}

lcd_nv3030b_rotation_t lvgl_port_get_rotation(void)
{
    return s_rotation;
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    if (s_lvgl_mutex == NULL) {
        return false;
    }
    if (timeout_ms == 0) {
        return xSemaphoreTake(s_lvgl_mutex, 0) == pdTRUE;
    }
    return xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    if (s_lvgl_mutex != NULL) {
        xSemaphoreGive(s_lvgl_mutex);
    }
}

const lv_font_t *lvgl_port_get_font(lvgl_port_font_size_t size)
{
    if (size < 0 || size >= LVGL_PORT_FONT_MAX) {
        return NULL;
    }
    return s_fonts[size];
}
