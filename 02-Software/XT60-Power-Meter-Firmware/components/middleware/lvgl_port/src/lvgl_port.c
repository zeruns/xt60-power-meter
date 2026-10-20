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
 * 本面板物理方向由初始化序列 MADCTL=0x68（MX|MV|BGR）固定为 284x240 横屏，
 * 运行期绝不再向面板写任何方向命令（命令与颜色 DMA 并发会冻结，见
 * lcd_nv3030b.c）。用户角度 0/90/180/270° 全部由 LVGL 软件旋转实现：
 * disp_cfg.flags.sw_rotate = 1 使官方跳过 HW 旋转分支，flush 时用
 * lv_draw_rotate() 把逻辑缓冲旋进物理缓冲（90/270° 换行、180° 倒序）。
 * ⚠ 该旋转函数只支持 LV_COLOR_FORMAT_RGB565，不支持 RGB565_SWAPPED
 * （否则旋转输出是垃圾内存=整屏花屏），因此渲染格式用 RGB565、由
 * flags.swap_bytes=1 在旋转完成后统一交换字节序给面板。
 * 90/270° 时 touch 映射按 s_touch_map 表把物理坐标换成逻辑坐标。
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

/** LVGL 心跳周期（毫秒）：交给官方 esp_lvgl_port 的 timer_period_ms
 *
 * 【v0.6.3 性能】5ms → 16ms。这个值决定"多久唤醒一次 LVGL 任务去跑
 * lv_timer_handler"。本工程最高刷新需求是：测量页 30Hz（33ms/帧）与
 * 波形页 25Hz（40ms/帧），另外还有滚动/切换动画——16ms（约 62 次/秒）
 * 对它们都留有余量，而 5ms（200 次/秒）意味着每秒 195 次"没事也醒来看看"
 * 的空唤醒，白耗 CPU 与唤醒延迟。实测日志显示应用层 flush 只有约 176 次/秒，
 * 且 UI 心跳龄 1~4ms（主循环本身 5ms 节拍），因此 16ms 不会拖慢交互。 */
#define LVGL_PORT_TICK_MS 16

/* 渲染缓冲块高度：PARTIAL 模式按行分块刷新（双缓冲，各 284×N×2 字节）。
 *
 * 【v0.6.3 性能】16 → 24 行。分块越小，一帧被切成的块数越多 → 每块的
 * lv_draw_sw_rotate（软件旋转）+ QSPI 提交开销 × 块数；实测整机 flush
 * 约 176 次/秒，多数是这种小块。24 行把块数减少约 1/3，旋转与提交的
 * 固定开销同比降下来，而全部字节数不变。
 * 内存代价（内部 RAM，DMA 只能读内部 RAM）：双缓冲 + 旋转缓冲共
 * 3 × 284×24×2 ≈ 40.9KB（原 16 行时约 27.3KB，净增约 13.6KB）。
 * 实测内部 RAM 空闲约 130KB（v0.6.1 起 ALWAYSINTERNAL=0 后），有余量；
 * ⚠ 若将来内部 RAM 又变紧张，**优先回调这个值**（它是内部 RAM 里最大的
 *   可压缩项），而不是把 draw buffer 挪去 PSRAM（DMA 读不到）。 */
#define LVGL_PORT_BUF_ROWS 24
/** 软件旋转输出缓冲行数：与主缓冲等大（90/270° 旋转前后像素总数不变） */
#define LVGL_PORT_SWROT_ROWS LVGL_PORT_BUF_ROWS
/** LVGL 任务栈大小（字节）与优先级、绑定核（0=交互核） */
/* 软件旋转（lv_draw_sw_rotate）+ 波形 chart 的渲染调用链较深，
 * 实测 7168 在竖屏渲染波形/设置页时栈溢出（剩 140B 后 CORRUPTED），
 * 12288 余量不足 200B；16384 留足安全余量。 */
#define LVGL_PORT_TASK_STACK 16384
/* 台阶1 优化：优先级 4 → 10。LVGL 是交互核的主要负载，低优先级下
 * 频繁被采样/日志等任务抢占，帧率受限；10 让渲染及时获得 CPU。
 * 仍低于测量核的关键任务，不会饿死其他任务。 */
#define LVGL_PORT_TASK_PRIO  10
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
static const int s_font_px[LVGL_PORT_FONT_MAX] = { 12, 14, 16, 20, 24, 32, 36, 40 };
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
 * @brief 触摸坐标映射（旋转交给 LVGL 内置逻辑，本层只做固定玻璃校准）
 *
 * 触摸 IC（CST816D）输出的是**触摸玻璃坐标** (px∈0..239, py∈0..283)，
 * 它不随显示方向变化。本层只做一次与旋转无关的固定校准（0° 实测锚点），
 * 把玻璃坐标换到**面板物理坐标系**（284×240）：
 *
 *   (Px, Py) = (283 - py, 239 - px)
 *
 * 各角度的物理→逻辑旋转**交给 LVGL 内置**：LVGL 9.6 的
 * indev_pointer_proc() 会对 read_cb 返回的点自动调用
 * lv_display_rotate_point()（默认 cw，恰好是 flush 软件旋转
 * lv_draw_rotate 的逆变换，三个角度均已逐一验证）。
 *
 * ⚠ read_cb 只能返回**物理坐标**。若在此按角度自行映射成逻辑坐标，
 *   会被 LVGL 二次旋转：0° 时 rotate_point 是恒等所以不暴露，
 *   180° 时两次旋转相互抵消（触摸表现为 0°，与显示差 180°），
 *   90°/270° 叠加出反向/错乱——2026-10 实机现象正是此根因，
 *   已废弃原 s_touch_map 逐档位映射表。
 */
static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    touch_cst816d_point_t tp;
    esp_err_t err = touch_cst816d_read(&tp);
    if (err != ESP_OK || !tp.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* 玻璃坐标 → 面板物理坐标（0° 实测锚点，与旋转无关）。
     * 283 = LCD_NV3030B_PHYS_H-1，239 = LCD_NV3030B_PHYS_W-1。 */
    data->point.x = (lv_coord_t)(LCD_NV3030B_PHYS_H - 1 - (int)tp.y);
    data->point.y = (lv_coord_t)(LCD_NV3030B_PHYS_W - 1 - (int)tp.x);
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
    /* 【v0.6.3 性能】LVGL 任务栈改回**内部 RAM**。
     * 旧注释（"WiFi 全开后内部 RAM 仅剩百字节级"）是 v0.6.0 当时的事实，
     * 而 v0.6.1 把 SPIRAM_MALLOC_ALWAYSINTERNAL 降到 0 之后，实机测量
     * WiFi 已连接时内部 RAM 空闲约 130KB（历史最低 119KB）——有余量了。
     * 为什么值得改回：栈是被高频访问的内存（函数调用/局部变量），PSRAM 的
     * 访问延迟明显高于内部 RAM（还要走 cache），渲染与 UI 逻辑都在这些栈上跑。
     * LVGL 渲染任务不做 ISR 操作（flush 的 DMA 完成回调在 ISR 自己的栈上），
     * 所以放内部 RAM 只是"更快"，不涉及 ISR 安全性。
     * ⚠ 若将来内部 RAM 重新吃紧（如新增 WiFi 特性），把这里改回 SPIRAM 即可，
     *   代价只是栈访问变慢，功能不受影响。 */
    cfg.task_stack_caps = MALLOC_CAP_INTERNAL;
    esp_err_t err = lvgl_port_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "官方 LVGL 移植层初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ② 中文字体（须在 lv_init 之后创建） ---- */
    load_fonts();

    /* ---- ③ 注册显示（软件旋转架构）----
     * 面板物理方向永久固定 284x240 横屏（MADCTL 0x68，init 写一次后不再动）。
     * sw_rotate=1：方向切换时 esp_lvgl_port 在 flush 里用 lv_draw_rotate 把
     * 逻辑缓冲旋进物理缓冲，运行期零面板命令，杜绝命令/颜色 DMA 并发冻结。
     * PARTIAL 模式脏区被 draw buffer 容量切成 ≤LVGL_PORT_BUF_ROWS 行的小块，
     * 284×24×2≈13.6KB 的旋转输出缓冲就够（与主缓冲同尺寸），放内部 RAM，DMA 快。
     * ⚠ 旋转缓冲必须与主缓冲等大：90°/270° 时旋转输出按目标行主序写，
     *   行数=源宽、行跨度=源高对应的字节数，像素总数不变。 */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_nv3030b_get_io_handle(),
        .panel_handle = lcd_nv3030b_get_panel_handle(),
        .buffer_size = (uint32_t)LCD_NV3030B_H_RES * LVGL_PORT_BUF_ROWS, /* 24 行块 */
        /* 台阶1 优化：双缓冲——LVGL 渲染 buf1 时 DMA 正在搬运 buf2，
         * 渲染与刷屏并行，消除 CPU 等 DMA 的空转（实测帧率提升明显）。
         * 内存代价：多一个 284×24×2≈13.6KB 内部 RAM 缓冲。 */
        .double_buffer = true,
        .hres = (uint32_t)LCD_NV3030B_H_RES, /* 物理 284，永不改变 */
        .vres = (uint32_t)LCD_NV3030B_V_RES, /* 物理 240，永不改变 */
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            .buff_dma = 1,   /* 主缓冲+旋转缓冲在内部 RAM（小块，渲染/DMA 快） */
            .buff_spiram = 0,
            .sw_rotate = 1,
            /* 必须在 flush 时交换字节序：LVGL 的 lv_draw_rotate()（9.6 中即
             * lv_draw_sw_rotate）只支持普通 RGB565，不支持 RGB565_SWAPPED
             * （落到 default 分支什么都不写，旋转输出是垃圾内存→90/180/270°
             * 整屏花屏）。因此渲染格式用 RGB565，旋转完成后由官方 flush 路径
             * 的 swap_bytes 把旋转输出逐像素交换成面板所需的大端字节序，
             * 0°（无旋转）路径同样正确。 */
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
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
    ESP_LOGI(TAG, "LVGL 适配层就绪（软件旋转；物理面板固定 284x240）");
    return ESP_OK;
}

/* 用户角度（0/90/180/270）→ LVGL 软件旋转枚举。面板物理恒为 0° 横屏。 */
static const lv_display_rotation_t s_angle_to_lvrot[4] = {
    [LCD_NV3030B_ROT_90]  = LV_DISPLAY_ROTATION_0,
    [LCD_NV3030B_ROT_180] = LV_DISPLAY_ROTATION_90,
    [LCD_NV3030B_ROT_270] = LV_DISPLAY_ROTATION_180,
    [LCD_NV3030B_ROT_0]   = LV_DISPLAY_ROTATION_270,
};

esp_err_t lvgl_port_set_rotation(lcd_nv3030b_rotation_t rot)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rot > LCD_NV3030B_ROT_270) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 纯 LVGL 软件旋转：不写面板、不手动改分辨率。
     * lv_display_set_rotation 内部更新逻辑分辨率并广播 RESOLUTION_CHANGED
     * （三页据此重建）；esp_lvgl_port flush 时逐块 lv_draw_sw_rotate。 */
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "旋转：获取 LVGL 锁超时，本次切换放弃");
        return ESP_ERR_TIMEOUT;
    }
    s_rotation = rot;
    lv_display_set_rotation(s_display, s_angle_to_lvrot[rot]);
    lvgl_port_unlock();

    (void)lcd_nv3030b_set_rotation(rot); /* 仅记录档位，驱动内部无面板事务 */

    int lw = 0, lh = 0;
    lcd_nv3030b_get_resolution(rot, &lw, &lh);
    ESP_LOGI(TAG, "显示旋转（LVGL 软件层）ROT_%d（逻辑 %dx%d，物理面板固定 284x240）",
             rot * 90, lw, lh);
    return ESP_OK;
}

lcd_nv3030b_rotation_t lvgl_port_get_rotation(void)
{
    return s_rotation;
}

static lcd_nv3030b_rotation_t angle_to_rot(uint16_t deg)
{
    switch (deg) {
    case 90:  return LCD_NV3030B_ROT_180;
    case 180: return LCD_NV3030B_ROT_270;
    case 270: return LCD_NV3030B_ROT_0;
    case 0:
    default:  return LCD_NV3030B_ROT_90;
    }
}

static uint16_t rot_to_angle(lcd_nv3030b_rotation_t rot)
{
    switch (rot) {
    case LCD_NV3030B_ROT_180: return 90;
    case LCD_NV3030B_ROT_270: return 180;
    case LCD_NV3030B_ROT_0:   return 270;
    case LCD_NV3030B_ROT_90:
    default:                 return 0;
    }
}

esp_err_t lvgl_port_set_rotation_angle(uint16_t deg)
{
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
        return ESP_ERR_INVALID_ARG;
    }
    return lvgl_port_set_rotation(angle_to_rot(deg));
}

uint16_t lvgl_port_get_rotation_angle(void)
{
    return rot_to_angle(s_rotation);
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
