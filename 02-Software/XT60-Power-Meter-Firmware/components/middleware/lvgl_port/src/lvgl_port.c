/**
 * @file lvgl_port.c
 * @brief 应用侧 LVGL 适配层实现（基于 ESP 官方 espressif/esp_lvgl_port）
 *
 * 职责划分（与官方组件的边界）：
 *   │ 关注点          │ 由谁负责                                  │
 *   │ LVGL 内核初始化  │ 官方 lvgl_port_init()（内部 lv_init）      │
 *   │ 心跳/时基        │ 官方（esp_timer 周期 lv_tick_inc）         │
 *   │ LVGL 任务        │ 官方（周期调用 lv_timer_handler）          │
 *   │ 访问互斥锁       │ 官方 lvgl_port_lock/unlock                 │
 *   │ 显示缓冲+刷屏    │ 官方 lvgl_port_add_disp()                  │
 *   │ 传输完成回调     │ 官方（注册到 panel IO 的 on_color_trans_done）│
 *   │ 中文字体         │ 本组件（SPIFFS→PSRAM→tiny_ttf）            │
 *   │ 触摸输入         │ 本组件（自研 CST816D 驱动 + 旋转映射）      │
 *   │ 首帧计数/背光门控│ 本组件（LVGL 事件 LV_EVENT_FLUSH_FINISH）   │
 *   │ 旋转档位         │ 本组件（MADCTL + LVGL 分辨率 + 触摸映射）   │
 *
 * ────────────────────────────────────────────────────────────
 * 关于硬旋转与 sw_rotate（重要，改动前必读）
 * ────────────────────────────────────────────────────────────
 * 本面板的方向由 MADCTL=0x68（MX|MV|BGR）在面板侧硬旋转为 284x240 横屏，
 * LVGL 逻辑分辨率随之设为 284x240。官方的 lvgl_port_add_disp() 在
 * sw_rotate=false 时会根据 disp_cfg.rotation 调用 esp_lcd_panel_swap_xy()/
 * mirror() 去“设置面板方向”。但本面板（spd2010 组件）**不支持 swap_xy**
 * （调用会报错），且该调用会改写 MADCTL，破坏我们已标定好的硬旋转。
 * 因此这里置 disp_cfg.flags.sw_rotate = 1：官方会跳过其 HW 旋转分支，
 * 只做“软件旋转”（而我们始终让 LVGL 旋转档位保持 ROTATION_0，软件旋转
 * 分支不产生任何变换），从而完全保留面板侧的 MADCTL 硬旋转与触摸映射。
 *
 * ────────────────────────────────────────────────────────────
 * 中文字体（tiny_ttf + SPIFFS 文件）
 * ────────────────────────────────────────────────────────────
 * 字体文件放 storage（SPIFFS）分区不占 app 程序空间；init 时一次性读入
 * PSRAM（2.2MB，8MB PSRAM 余量充足），之后渲染全走内存，运行期对 flash
 * 零访问——避免渲染中读 SPIFFS 与 flash 擦除/LCD DMA 交错触发 cache 故障。
 * 文件缺失时回退内置 montserrat14（仅 ASCII），界面不崩。
 */
#include "lvgl_port.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lcd_nv3030b.h"
#include "touch_cst816d.h"

static const char *TAG = "lvgl_port";

/** LVGL 心跳周期（毫秒）：交给官方 esp_lvgl_port 的 timer_period_ms */
#define LVGL_PORT_TICK_MS 5
/** flush 缓冲行数（PARTIAL 模式按行刷新，40 行兼顾内存占用与刷新效率） */
#define LVGL_PORT_BUF_ROWS 40
/** LVGL 任务栈大小（字节）与优先级、绑定核（0=交互核） */
#define LVGL_PORT_TASK_STACK 7168
#define LVGL_PORT_TASK_PRIO  4
#define LVGL_PORT_TASK_CORE  0

/* ---- 中文字体路径（SPIFFS） ---- */
#define LVGL_PORT_FONT_REG_PATH  "/spiffs/fonts/notosc_reg.ttf"
#define LVGL_PORT_FONT_BOLD_PATH "/spiffs/fonts/notosc_bold.ttf"

static bool s_inited = false;                  /**< 防重复初始化标志 */
static lv_display_t *s_display = NULL;         /**< 官方 add_disp 返回的显示设备 */
static lv_indev_t *s_indev = NULL;             /**< 触摸输入设备（本组件注册） */

/** 累计完成渲染的次数（LVGL 事件回调内自增）：UI 层据此判断“首帧已上屏”，
 *  然后才打开背光——避免开机先看到黑屏/局部条带再跳到启动画面。 */
static volatile uint32_t s_flush_count = 0;

/** 当前显示旋转档位（与面板 MADCTL 保持同步；触摸映射按它查表） */
static lcd_nv3030b_rotation_t s_rotation = LCD_NV3030B_ROT_90;

/** 各字号对应的像素高度（与枚举 lvgl_port_font_size_t 一一对应） */
static const int s_font_px[LVGL_PORT_FONT_MAX] = { 12, 14, 16, 20, 24, 32, 40 };
/** 字重下标 */
enum {
    FONT_WEIGHT_REG = 0,
    FONT_WEIGHT_BOLD,
    FONT_WEIGHT_MAX,
};
/** 已创建的字体指针 [字重][字号]；NULL 表示未创建（取用方需判空） */
static lv_font_t *s_fonts[FONT_WEIGHT_MAX][LVGL_PORT_FONT_MAX] = { 0 };
/** 从 SPIFFS 整读入 PSRAM 的字体数据（运行期常驻，单位字节） */
static uint8_t *s_font_data[FONT_WEIGHT_MAX] = { 0 };
static size_t s_font_data_len[FONT_WEIGHT_MAX] = { 0 };

/**
 * @brief 把 SPIFFS 上的 TTF 整个读入 PSRAM
 * @return ESP_OK 成功；文件不存在/读取失败/内存不足返回错误（不致命）
 */
static esp_err_t load_font_file(const char *path, uint8_t **out_data, size_t *out_len)
{
    *out_data = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "字体文件打不开: %s（将回退内置 ASCII 字体）", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return ESP_FAIL;
    }
    rewind(f);

    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配 %ld 字节存字体失败", sz);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        ESP_LOGE(TAG, "字体读取不完整: %s（%u/%ld）", path, (unsigned)got, sz);
        free(buf);
        return ESP_FAIL;
    }

    *out_data = buf;
    *out_len = (size_t)sz;
    return ESP_OK;
}

/**
 * @brief 加载中文字体（SPIFFS→PSRAM→tiny_ttf，常规/粗体 × 多字号）
 */
static void load_fonts(void)
{
    static const struct {
        uint8_t weight;
        const char *path;
    } faces[FONT_WEIGHT_MAX] = {
        [FONT_WEIGHT_REG] = { FONT_WEIGHT_REG, LVGL_PORT_FONT_REG_PATH },
        [FONT_WEIGHT_BOLD] = { FONT_WEIGHT_BOLD, LVGL_PORT_FONT_BOLD_PATH },
    };

    int ok_count = 0;
    for (int w = 0; w < FONT_WEIGHT_MAX; w++) {
        if (load_font_file(faces[w].path, &s_font_data[w], &s_font_data_len[w]) != ESP_OK) {
            continue;
        }
        for (int i = 0; i < LVGL_PORT_FONT_MAX; i++) {
            s_fonts[w][i] = lv_tiny_ttf_create_data(s_font_data[w], s_font_data_len[w], s_font_px[i]);
            if (s_fonts[w][i] != NULL) {
                ok_count++;
            } else {
                ESP_LOGW(TAG, "字重 %d 创建 %dpx 字体失败", w, s_font_px[i]);
            }
        }
    }

    /* 字体回退链：Noto 子集含汉字但不含 LVGL 的 FontAwesome 图标符号
     * （电池/WiFi/设置等在 U+F000 私用区）。把内置 montserrat14 挂为
     * fallback：中文走 Noto，图标符号走 montserrat，两者都不出方块。 */
    for (int w = 0; w < FONT_WEIGHT_MAX; w++) {
        for (int i = 0; i < LVGL_PORT_FONT_MAX; i++) {
            if (s_fonts[w][i] != NULL) {
                s_fonts[w][i]->fallback = &lv_font_montserrat_14;
            }
        }
    }

    ESP_LOGI(TAG, "中文字体加载完成：%d/%d 个（2 字重 × %d 字号，SPIFFS→PSRAM 内存渲染）",
             ok_count, FONT_WEIGHT_MAX * LVGL_PORT_FONT_MAX, LVGL_PORT_FONT_MAX);
}

/**
 * @brief 渲染完成事件回调：累加首帧计数（供背光门控）
 *
 * 官方移植层在刷屏回调返回后会触发 LV_EVENT_FLUSH_FINISH，这里只做计数，
 * 不阻塞、不加锁。
 */
static void disp_flush_finish_cb(lv_event_t *e)
{
    (void)e;
    s_flush_count++;
}

uint32_t lvgl_port_flush_count(void)
{
    return s_flush_count;
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
 * ⚠ ROT_90（当前默认）的三项参数是**上板实测**校正过的，换档位方向不对时
 *   只改这张表，不要动面板 MADCTL。
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

esp_err_t lvgl_port_app_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- ① 官方 LVGL 移植层：初始化 LVGL + 创建 LVGL 任务与心跳定时器 ---- */
    lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.task_priority = LVGL_PORT_TASK_PRIO;
    cfg.task_stack = LVGL_PORT_TASK_STACK;
    cfg.task_affinity = LVGL_PORT_TASK_CORE;
    cfg.task_max_sleep_ms = 500;
    cfg.timer_period_ms = LVGL_PORT_TICK_MS;
    esp_err_t err = lvgl_port_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "官方 LVGL 移植层初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ② 中文字体（须在 lv_init 之后创建） ---- */
    load_fonts();

    /* ---- ③ 注册显示：由官方分配缓冲并接管刷屏与传输完成回调 ----
     * 关键：sw_rotate=1 —— 面板方向由 MADCTL 硬旋转决定，必须让官方跳过其
     * HW 旋转分支（否则会调 swap_xy（本面板不支持）并改写 MADCTL）。详见文件头。 */
    int lw = 0;
    int lh = 0;
    lcd_nv3030b_get_resolution(s_rotation, &lw, &lh);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_nv3030b_get_io_handle(),
        .panel_handle = lcd_nv3030b_get_panel_handle(),
        .buffer_size = (uint32_t)LCD_NV3030B_PHYS_H * LVGL_PORT_BUF_ROWS, /* 单位：像素 */
        .double_buffer = false,
        .hres = (uint32_t)lw,
        .vres = (uint32_t)lh,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            .buff_dma = 1,   /* 渲染缓冲放内部 RAM（QSPI DMA 要求） */
            .buff_spiram = 0,
            .sw_rotate = 1,  /* 见上：保护 MADCTL 硬旋转 */
            .swap_bytes = 0, /* 颜色格式已用 RGB565_SWAPPED，无需再交换 */
            .full_refresh = 0,
            .direct_mode = 0,
        },
        .color_format = LV_COLOR_FORMAT_RGB565_SWAPPED, /* 面板线上要求高字节在前 */
    };

    s_display = lvgl_port_add_disp(&disp_cfg);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "注册显示设备失败（io/panel 句柄是否有效？）");
        return ESP_FAIL;
    }

    /* 渲染完成计数：官方在刷屏回调返回后触发 FLUSH_FINISH 事件 */
    lv_display_add_event_cb(s_display, disp_flush_finish_cb, LV_EVENT_FLUSH_FINISH, NULL);

    /* ---- ④ 用中文字体重建默认主题 ----
     * msgbox/list/roller/键盘等官方控件默认字体原本是 montserrat14（不含中文，
     * 显示成方块）。重建后这些控件默认使用 16px Noto（其 fallback 已挂
     * montserrat14，图标仍正常）。主题重建会触发一次全树样式刷新，此时还没有
     * 任何业务界面。 */
    if (s_fonts[FONT_WEIGHT_REG][LVGL_PORT_FONT_16] != NULL) {
        lv_theme_default_init(s_display, lv_color_hex(0x0066CC), lv_color_hex(0x8A8F99),
                              false /*浅色主题*/, s_fonts[FONT_WEIGHT_REG][LVGL_PORT_FONT_16]);
    }

    /* ---- ⑤ 触摸输入设备（自研驱动 + 旋转映射） ----
     * 官方 lvgl_port_add_touch() 需要 esp_lcd_touch 句柄；本工程用自研
     * CST816D 驱动，故自行创建 indev（理由见 lvgl_port.h 文件头）。 */
    s_indev = lv_indev_create();
    if (s_indev == NULL) {
        ESP_LOGE(TAG, "创建触摸输入设备失败");
        return ESP_FAIL;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, indev_read_cb);

    s_inited = true;
    ESP_LOGI(TAG, "LVGL 适配层就绪（官方 esp_lvgl_port；显示 %dx%d ROT_%d，RGB565 big-endian）",
             lw, lh, s_rotation * 90);
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

/**
 * @brief 取字体的内部实现：文件字体失败时回退内置 montserrat14（仅 ASCII）
 */
static const lv_font_t *get_font(int weight, lvgl_port_font_size_t size)
{
    if (size < 0 || size >= LVGL_PORT_FONT_MAX || weight < 0 || weight >= FONT_WEIGHT_MAX) {
        return NULL;
    }
    lv_font_t *font = s_fonts[weight][size];
    if (font != NULL) {
        return font;
    }
    /* SPIFFS 未挂载 / 字体文件缺失：回退内置 ASCII 字体，保证界面不崩 */
    return &lv_font_montserrat_14;
}

const lv_font_t *lvgl_port_get_font(lvgl_port_font_size_t size)
{
    return get_font(FONT_WEIGHT_REG, size);
}

const lv_font_t *lvgl_port_get_font_bold(lvgl_port_font_size_t size)
{
    return get_font(FONT_WEIGHT_BOLD, size);
}
