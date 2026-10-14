/**
 * @file sim_main.c
 * @brief 模拟器主程序：headless 渲染引擎（确定性虚拟时钟）+ BMP 截图导出
 *
 * 渲染一致性设计（与真机逐字节对齐的路径）：
 *   · 显示分辨率 284×240、PARTIAL 渲染模式、缓冲 40 行
 *     —— 与真机 lvgl_port 的 LVGL_PORT_BUF_ROWS=40 一致；
 *   · 颜色格式 RGB565_SWAPPED —— 与真机面板线上格式一致，
 *     flush 回调收到的像素缓冲即真机 DMA 缓冲的同格式数据；
 *   · 虚拟时钟步进 5ms —— 与真机 LVGL_PORT_TICK_MS=5 一致，
 *     时间推进可控 ⇒ 动画/定时器行为可复现，截图完全确定性。
 *
 * 截图：直接从帧缓冲导出 24bpp BMP（RGB888），由 capture.py 转 PNG。
 */
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl_port.h"

#define SIM_W 284 /**< 默认横屏宽（与 LCD_NV3030B_H_RES 一致） */
#define SIM_H 240
/** 帧缓冲行距：按横竖屏最大边（284）固定，避免旋转后重分配 */
#define FB_STRIDE 284
#define FB_MAX_H 284
/** flush 缓冲行数与心跳（与真机 lvgl_port 保持一致） */
#define DISP_BUF_ROWS 40
#define TICK_MS 5

#ifndef SIM_OUT_DIR
#define SIM_OUT_DIR "."
#endif

static uint8_t s_disp_buf[SIM_W * DISP_BUF_ROWS * 2];
static uint8_t s_fb[FB_STRIDE * FB_MAX_H * 2];

static data_store_snapshot_t s_snap = {
    .voltage_v = 19.97670f,
    .current_a = 0.018860f,
    .power_w = 0.37683f,
    .energy_wh = 0.11800f,
    .temperature_c = 32.5f,
    .humidity_pct = 30.0f,
    .battery_mv = 3920,
    .runtime_s = 12345,
};

static int s_touch_x = 0;
static int s_touch_y = 0;
static bool s_touch_pressed = false;

/* ============================================================
 * 显示与输入
 * ============================================================ */

/** @brief flush：把渲染块写入帧缓冲（数据格式与真机 DMA 缓冲一致） */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    for (int y = 0; y < h; y++) {
        memcpy(&s_fb[((size_t)(area->y1 + y) * FB_STRIDE + (size_t)area->x1) * 2],
               &px_map[(size_t)y * (size_t)w * 2], (size_t)w * 2);
    }
    lv_display_flush_ready(disp);
    sim_flush_count_inc();
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = (lv_coord_t)s_touch_x;
    data->point.y = (lv_coord_t)s_touch_y;
    data->state = s_touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void sim_init(void)
{
    lv_init();

    lv_display_t *d = lv_display_create(SIM_W, SIM_H);
    lv_display_set_flush_cb(d, flush_cb);
    lv_display_set_buffers(d, s_disp_buf, NULL, sizeof(s_disp_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565_SWAPPED);

    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, indev_read_cb);

    /* 字体 + 主题（与真机 lvgl_port_app_init 对应） */
    lvgl_port_app_init();

    printf("[sim] 引擎就绪：%dx%d RGB565_SWAPPED，PARTIAL %d 行，tick %dms\n", SIM_W, SIM_H,
           DISP_BUF_ROWS, TICK_MS);
}

void sim_step(uint32_t ms)
{
    uint32_t steps = ms / TICK_MS;
    if (steps == 0) {
        steps = 1;
    }
    for (uint32_t i = 0; i < steps; i++) {
        lv_tick_inc(TICK_MS);
        lv_timer_handler();
    }
}

void sim_touch_down(int x, int y)
{
    s_touch_x = x;
    s_touch_y = y;
    s_touch_pressed = true;
}

void sim_touch_move(int x, int y)
{
    s_touch_x = x;
    s_touch_y = y;
    s_touch_pressed = true;
}

void sim_touch_up(void)
{
    s_touch_pressed = false;
}

void sim_click(int x, int y)
{
    sim_touch_down(x, y);
    sim_step(40);
    sim_touch_up();
    sim_step(140);
}

void sim_swipe(int x1, int y1, int x2, int y2, int steps)
{
    if (steps < 2) {
        steps = 2;
    }
    sim_touch_down(x1, y1);
    sim_step(30);
    for (int i = 1; i <= steps; i++) {
        sim_touch_move(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
        sim_step(25);
    }
    sim_touch_up();
    sim_step(150);
}

void sim_get_resolution(int *w, int *h)
{
    lv_display_t *d = lv_display_get_default();
    if (w != NULL) {
        *w = lv_display_get_horizontal_resolution(d);
    }
    if (h != NULL) {
        *h = lv_display_get_vertical_resolution(d);
    }
}

int sim_center_x(void)
{
    int w = 0;
    sim_get_resolution(&w, NULL);
    return w / 2;
}

int sim_center_y(void)
{
    int h = 0;
    sim_get_resolution(NULL, &h);
    return h / 2;
}

/* ============================================================
 * 测量快照
 * ============================================================ */

void sim_set_measurement(float voltage_v, float current_a, float power_w, float energy_wh,
                         float temperature_c, float humidity_pct, uint32_t battery_mv,
                         uint32_t runtime_s)
{
    s_snap.voltage_v = voltage_v;
    s_snap.current_a = current_a;
    s_snap.power_w = power_w;
    s_snap.energy_wh = energy_wh;
    s_snap.temperature_c = temperature_c;
    s_snap.humidity_pct = humidity_pct;
    s_snap.battery_mv = battery_mv;
    s_snap.runtime_s = runtime_s;
}

const data_store_snapshot_t *sim_get_measurement(void)
{
    return &s_snap;
}

void sim_zero_energy(void)
{
    s_snap.energy_wh = 0.0f;
    s_snap.runtime_s = 0;
}

/* ============================================================
 * 截图（BMP 24bpp → capture.py 转 PNG）
 * ============================================================ */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void sim_shot(const char *name)
{
    /* 截图前强制把挂起的刷新落地，保证画面为最新状态 */
    lv_refr_now(NULL);

    int w = 0;
    int h = 0;
    sim_get_resolution(&w, &h);
    if (w <= 0 || h <= 0 || w > FB_STRIDE || h > FB_MAX_H) {
        printf("[sim] 分辨率异常，跳过截图 %s\n", name);
        return;
    }

    char path[600];
    snprintf(path, sizeof(path), "%s/%s.bmp", SIM_OUT_DIR, name);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("[sim] 无法写文件: %s\n", path);
        return;
    }

    int row_bytes = (w * 3 + 3) & ~3;
    uint32_t data_size = (uint32_t)row_bytes * (uint32_t)h;
    uint32_t file_size = 54u + data_size;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_u32(&hdr[2], file_size);
    put_u32(&hdr[10], 54);
    put_u32(&hdr[14], 40);
    put_u32(&hdr[18], (uint32_t)w);
    put_u32(&hdr[22], (uint32_t)h); /* 正值：自下而上 */
    put_u16(&hdr[26], 1);
    put_u16(&hdr[28], 24);
    put_u32(&hdr[34], data_size);
    put_u32(&hdr[38], 2835);
    put_u32(&hdr[42], 2835);
    fwrite(hdr, 1, sizeof(hdr), f);

    uint8_t *row = (uint8_t *)malloc((size_t)row_bytes);
    if (row == NULL) {
        fclose(f);
        return;
    }
    for (int y = h - 1; y >= 0; y--) {
        memset(row, 0, (size_t)row_bytes);
        for (int x = 0; x < w; x++) {
            const uint8_t *p = &s_fb[((size_t)y * FB_STRIDE + (size_t)x) * 2];
            /* RGB565_SWAPPED：高字节在前 */
            uint16_t v = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
            uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
            uint8_t g6 = (uint8_t)((v >> 5) & 0x3F);
            uint8_t b5 = (uint8_t)(v & 0x1F);
            /* 真机面板 MADCTL 的 BGR=1（0x08）会把帧缓冲码值的 R/B 通道
             * 交换后映射到物理子像素——模拟器没有面板这一环，导出时做
             * 同样的交换，截图颜色才与真机观感一致（否则蓝显橙、红显蓝）。 */
            uint8_t r8 = (uint8_t)((b5 << 3) | (b5 >> 2));
            uint8_t b8 = (uint8_t)((r5 << 3) | (r5 >> 2));
            row[x * 3 + 0] = r8;
            row[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            row[x * 3 + 2] = b8;
        }
        fwrite(row, 1, (size_t)row_bytes, f);
    }
    free(row);
    fclose(f);

    printf("[sim] 截图 %s（%dx%d）\n", path, w, h);
}

/* ============================================================
 * 入口
 * ============================================================ */

int main(void)
{
    sim_init();
    sim_run_scenes();
    printf("[sim] 完成\n");
    return 0;
}