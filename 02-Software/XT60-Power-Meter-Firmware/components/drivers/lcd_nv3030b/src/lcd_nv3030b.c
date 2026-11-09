/**
 * @file lcd_nv3030b.c
 * @brief NV3030B / SPD2010 显示驱动实现：QSPI 总线 + 面板 + LEDC 背光
 *
 * 器件：TK018F3716 模组，NV3030B 控制器（240RGB×320，玻璃用 240×284）。
 * 引脚见 board_pins.h 的 BOARD_PIN_LCD_* / BOARD_PIN_LCD_BL。
 *
 * 本驱动只做四件事：
 *   ① QSPI 总线 + panel IO + 背光；
 *   ② 通过官方组件 espressif/esp_lcd_spd2010 创建面板；
 *   ③ 注入厂商在本模组上验证过的初始化序列 s_lcd_init_cmds；
 *   ④ 刷确定底色（**不点背光**，留给上层在第一帧上屏后打开）；
 *      LVGL 刷屏走 lcd_nv3030b_draw_bitmap()。
 *
 * ============================================================
 * 为什么用官方组件而不是自写命令序列
 * ------------------------------------------------------------
 * QSPI 命令必须带 opcode 封装，否则面板收不到有效命令：
 *   线上 32bit 命令字 = (opcode << 24) | (cmd << 8)
 *     写命令 opcode = 0x02，写显存 opcode = 0x32
 * esp_lcd 对 32bit 命令只做字节序反转、不补 opcode；opcode 封装由
 * spd2010 组件在 QSPI 模式下内部完成（见其 tx_param/tx_color）。
 *
 * ============================================================
 * 为什么必须覆盖组件默认初始化序列（关键）
 * ------------------------------------------------------------
 * 注册表 espressif/esp_lcd_spd2010 1.0.2 自带的默认表（约 200 条分页序列）
 * 对本模组不适用 → 满屏花屏。厂商随模组提供的同组件版本已把默认表整段
 * 注释，替换为 s_lcd_init_cmds 这套短序列（与 STM32 例程 Lcd_Initialize
 * 逐字节一致）。通过 spd2010_vendor_config_t.init_cmds 注入。
 *
 * ============================================================
 * 显示方向：序列末尾 MADCTL=0x68 = MX|MV|BGR（标准 MIPI rotation 1）
 *   NV3030B 手册 7.1.25：MY=0x80 MX=0x40 MV=0x20 BGR=0x08；
 *   TFT_eSPI 官方 NV3030B 驱动横屏即用 0x68。只写 0x28(无 MX) 会镜像错位。
 *   240×284 玻璃在 240×320 RAM 内无需 CGRAM 偏移（gap=0）。
 *
 * ============================================================
 * 上板调试要点
 * ------------------------------------------------------------
 *   · 整屏黑但背光正常：多为命令帧不被面板接收，先确认 quad_mode 与
 *     use_qspi_interface=1；
 *   · 颜色偏色/列错位：像素字节序问题。面板线上要求高字节在前
 *     （big-endian），LVGL 侧以 RGB565 渲染、flush 时 swap_bytes 交换成大端；
 *   · 横条变竖条：MADCTL 方向位错，确认是 0x68 不是 0x28。
 * ============================================================
 */
#include "lcd_nv3030b.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board_pins.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_spd2010.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** 日志标签 */
static const char *TAG = "lcd_nv3030b";

/* ============================================================
 * QSPI 总线 / panel IO 参数
 * 取值与厂商宏 SPD2010_PANEL_BUS_QSPI_CONFIG / SPD2010_PANEL_IO_QSPI_CONFIG
 * 逐项一致。
 * ============================================================ */
#define LCD_QSPI_HOST        SPI2_HOST
/* QSPI 写显存时钟。
 * 实测结论（本模组 + 本板走线）：40MHz 花屏；30MHz 表面正常但持续刷屏数秒后
 * 面板失锁（软件全链路 ESP_OK，但 GRAM 不再更新，屏幕冻结），根因是该面板/
 * 走线的信号完整性裕量不足。曾在排查中降到 10MHz 临时稳定；后续做了「台阶1」
 * 走线/时序优化后，现状 23MHz 实测长时间稳定，为当前量产取值。
 * ⚠ 该值与显示稳定性强相关：若换批次/改板出现刷屏冻结，优先降档排查；
 *   反向调高必须重新长时间刷屏实测验证，不可只看短时正常。
 * 注意：只影响写显存速率，不影响命令时序。 */
#define LCD_QSPI_PCLK_HZ     (23 * 1000 * 1000)
#define LCD_QSPI_CMD_BITS    32
#define LCD_QSPI_PARAM_BITS  8
#define LCD_QSPI_TRANS_DEPTH 10

/** 一次最大传输：整屏 RGB565 字节数（供 DMA 分配参考） */
#define LCD_MAX_TRANSFER_SZ  (LCD_NV3030B_H_RES * LCD_NV3030B_V_RES * 2)

/* ============================================================
 * 背光 LEDC 资源
 *
 * 按 board_pins.h 统一规划：LCD 背光用 LEDC 通道 1 / 定时器 1，
 * 蜂鸣器用通道 0 / 定时器 0，两者互不冲突。
 * 背光 IO45 经 Q3 AO3400A 低边驱动 LED-：IO45 高 → Q3 导通 → 亮。
 * ============================================================ */
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER    LEDC_TIMER_1
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_1
#define BL_LEDC_RES      LEDC_TIMER_10_BIT
#define BL_DUTY_MAX      ((1 << 10) - 1)
#define BL_PWM_FREQ_HZ   5000

/** 背光 PWM 是否已配置（配置后方可调占空比） */
static bool s_bl_ready = false;

/** QSPI panel IO 句柄；非空表示总线与 IO 已就绪 */
static esp_lcd_panel_io_handle_t s_io = NULL;

/** 面板句柄（官方 esp_lcd_spd2010 实例） */
static esp_lcd_panel_handle_t s_panel = NULL;

/** 是否已初始化 */
static bool s_initialized = false;

/** 上层注册的「显存写入完成」回调及其上下文（见头文件） */
static lcd_nv3030b_flush_done_cb_t s_flush_done_cb = NULL;
static void *s_flush_done_ctx = NULL;

/* ---------------- 句柄导出（供官方 esp_lvgl_port 注册显示） ----------------
 * 改用 ESP 官方 espressif/esp_lvgl_port 后，显示设备由它调用
 * lvgl_port_add_disp() 注册（它需要 panel / panel IO 句柄自行刷屏），
 * 故把内部持有的两个句柄暴露出去；未初始化时返回 NULL。 */
esp_lcd_panel_handle_t lcd_nv3030b_get_panel_handle(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t lcd_nv3030b_get_io_handle(void)
{
    return s_io;
}

/* ============================================================
 * 旋转（MADCTL 0x36）
 *
 * 每个档位对应的 MADCTL 值（标准 MIPI，含 BGR 位），与 TFT_eSPI 官方
 * NV3030B 驱动的 rotation 表一致。init 的厂商序列末尾已写 ROT_90(0x68)，
 * 故初始档位为 ROT_90。运行期切换由 lcd_nv3030b_set_rotation() 完成。
 * ============================================================ */
static const uint8_t s_rot_madctl[4] = {
    [LCD_NV3030B_ROT_0] = 0x08u,   /* BGR            竖屏   */
    [LCD_NV3030B_ROT_90] = 0x68u,  /* MX|MV|BGR      横屏   */
    [LCD_NV3030B_ROT_180] = 0xC8u, /* MX|MY|BGR      竖屏倒 */
    [LCD_NV3030B_ROT_270] = 0xA8u, /* MV|MY|BGR      横屏倒 */
};

/** 当前旋转档位（init 序列末尾写的是 ROT_90） */
static lcd_nv3030b_rotation_t s_rotation = LCD_NV3030B_ROT_90;

/**
 * @brief 把 8bit 命令码封装成 QSPI 的 32bit 命令字
 *
 * 该面板 QSPI 命令字格式为 (opcode << 24) | (cmd << 8)，写命令 opcode = 0x02。
 * esp_lcd 只做字节序反转、不补 opcode，故运行期发命令要自己封。
 */
#define LCD_QSPI_CMD(cmd8) ((int)((0x02u << 24) | (((uint32_t)(cmd8) & 0xFFu) << 8)))
/** LCD_CMD_MADCTL */
#define LCD_CMD_MADCTL 0x36u

/* ============================================================
 * 开机清屏
 *
 * 面板控制器刚复位时显存内容未定义，若此时直接点亮背光，用户会看到
 * 一段不确定的白色闪烁。故在开背光前先刷一屏确定的颜色（黑），把这段
 * 不确定的窗口盖掉。缓冲放在 BSS（静态、内部 RAM），分块传输。
 * ⚠ 极性：若这块清屏色看起来是白的，说明面板反显极性相反——
 *   但本工程的反显已由初始化序列 0x21 处理，一般无需再改。
 * ============================================================ */
#define LCD_FILL_ROWS   8       /**< 每次清屏传输的行数（横屏逻辑行） */
#define LCD_CLEAR_COLOR 0x0000u /**< 开机清屏色：RGB565 黑 */

static uint8_t s_fill_buf[LCD_NV3030B_H_RES * LCD_FILL_ROWS * 2];

/**
 * 清屏 / 自检图案的「等待传输完成」同步信号量（**创建一次、常驻不删**）
 *
 * ⚠ 为什么不删（曾踩的竞态）：颜色完成回调 lcd_panel_io_color_done_cb() 在
 * SPI 中断上下文执行，会先判 s_clear_sem != NULL 再 give；若 init 任务在
 * 清屏完成后 vSemaphoreDelete() 并置 NULL，回调可能恰好在「判断非空」与
 * 「give」之间执行删除 → 对已释放内存 give，use-after-free 直接崩。
 * 「删除前注销回调」也堵不住：esp_lcd 没有注销 on_color_trans_done 的接口，
 * 且可能已有 DMA 传输在飞、回调已在中断里排队。
 * 故采用**常驻方案**：init 时创建一次、永不删除（一个二值信号量的内存开销
 * 可忽略），每次清屏块之间 take 掉可能残留的 give 即可；
 * 运行期刷屏由上层注册的 s_flush_done_cb 通知（见该回调实现）。
 */
static SemaphoreHandle_t s_clear_sem = NULL;

/**
 * 「清屏/自检进行中」标志（回调据此决定给信号量还是转发上层回调）
 *
 * 信号量常驻后不能再用「s_clear_sem 是否为 NULL」判断阶段（它永远非 NULL），
 * 故用本标志：清屏/自检期间为 true（回调 give 信号量），其余时间为 false
 * （回调转发 s_flush_done_cb 给 LVGL）。只在任务侧写、ISR 侧读，
 * 用 volatile 保证可见性；写入时机都在「提交 DMA 之前」与「DMA 已确认完成后」，
 * 不存在 ISR 读到中间态的问题。
 */
static volatile bool s_clear_waiting = false;

/* 开机显示自检图案（诊断用，0 = 关闭）。
 *
 * 曾经用它定位花屏：绕开 LVGL 直刷 5 条纯色横条，一眼区分字节序/方向/通路。
 * 显示链路已调通，按产品需求改为「关闭」，启动画面改由 services/ui 用
 * LVGL 渲染（见 ui.c 的启动画面）。需要再排查显示问题时把这里改回 10000。 */
#define LCD_BOOT_TEST_PATTERN_MS 0

#if (LCD_BOOT_TEST_PATTERN_MS > 0)
/**
 * @brief 画 5 条横向色条（红/绿/蓝/白/黑），用于上板目视定位
 *
 * 横屏逻辑坐标（284 宽 × 240 高），每条约"整屏宽 × 48 高"。
 * 像素按 big-endian（高字节在前）填入，与面板线上字节序一致。
 *
 * @return ESP_OK 成功；否则为绘制错误
 */
static esp_err_t panel_draw_test_pattern(void)
{
    static const uint16_t bar_rgb565[5] = {
        0xF800u, /* 红 */
        0x07E0u, /* 绿 */
        0x001Fu, /* 蓝 */
        0xFFFFu, /* 白 */
        0x0000u, /* 黑 */
    };
    static uint8_t row_buf[LCD_NV3030B_H_RES * 2];

    const int bar_rows = LCD_NV3030B_V_RES / 5; /* 240/5 = 48 行/条 */

    /* 本函数独立于 panel_clear 被调用，自行置「清屏等待」标志，
     * 让颜色完成回调把完成发给信号量而不是上层回调 */
    s_clear_waiting = true;

    for (int i = 0; i < 5; i++) {
        for (size_t k = 0; k < sizeof(row_buf); k += 2) {
            /* big-endian：先高字节、后低字节（与 RGB565_SWAPPED 一致） */
            row_buf[k] = (uint8_t)(bar_rgb565[i] >> 8);
            row_buf[k + 1] = (uint8_t)(bar_rgb565[i] & 0xFFu);
        }

        const int y0 = i * bar_rows;
        const int y1 = (i == 4) ? LCD_NV3030B_V_RES : (y0 + bar_rows);

        esp_err_t err = lcd_nv3030b_draw_bitmap(0, y0, LCD_NV3030B_H_RES, y1, row_buf);
        if (err != ESP_OK) {
            s_clear_waiting = false;
            return err;
        }
        /* 先清残留 give 再等本轮 DMA（信号量常驻，残留会给假完成，见 panel_clear） */
        (void)xSemaphoreTake(s_clear_sem, 0);
        if (xSemaphoreTake(s_clear_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            s_clear_waiting = false;
            return ESP_ERR_TIMEOUT;
        }
    }
    s_clear_waiting = false;
    return ESP_OK;
}
#endif /* LCD_BOOT_TEST_PATTERN_MS */

/**
 * @brief esp_lcd 颜色传输完成回调（SPI 中断上下文）
 *
 * esp_lcd 对颜色数据走 spi_device_queue_trans（异步），只有这个回调触发时
 * 才代表 DMA 真正结束、缓冲可安全复用。
 *
 * ⚠ 信号量常驻（见 s_clear_sem 注释）：这里不再判「s_clear_sem != NULL」，
 * 而是看 s_clear_waiting 标志分流「清屏等待」与「上层回调」两条路径；
 * 对已删除信号量 give 的 use-after-free 风险由此消除。
 *
 * @return 固定返回 false：不在回调里额外触发任务切换
 */
static bool lcd_panel_io_color_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    if (s_clear_waiting) {
        /* 清屏/自检阶段：发信号量让任务等传输完成（信号量常驻，无需判 NULL） */
        BaseType_t hp = pdFALSE;
        xSemaphoreGiveFromISR(s_clear_sem, &hp);
        portYIELD_FROM_ISR(hp);
    } else if (s_flush_done_cb != NULL) {
        /* 运行期：转发给上层（LVGL）的缓冲可复用通知 */
        s_flush_done_cb(s_flush_done_ctx);
    }
    return false;
}

esp_err_t lcd_nv3030b_register_flush_done_cb(lcd_nv3030b_flush_done_cb_t cb, void *user_ctx)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_flush_done_cb = cb;
    s_flush_done_ctx = user_ctx;
    return ESP_OK;
}

/**
 * @brief 配置背光 PWM（LEDC 通道 1 / 定时器 1），初始占空比 0（背光灭）
 * @return ESP_OK 成功
 */
static esp_err_t backlight_pwm_init(void)
{
    /* LEDC 分「定时器」和「通道」两层：
     *   timer   决定 PWM 频率（freq_hz=5kHz）与占空比分辨率（10bit → 0~1023）；
     *   channel 决定用哪个定时器、输出到哪个 GPIO、初始占空比。
     * 4 个通道可共用同一 timer，本工程背光独用 timer1 / channel1。 */
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置背光 LEDC 定时器失败: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t ch_cfg = {
        .gpio_num = BOARD_PIN_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置背光 LEDC 通道失败: %s", esp_err_to_name(err));
        return err;
    }

    s_bl_ready = true;
    return ESP_OK;
}

/**
 * @brief 用指定颜色铺满整屏（横屏逻辑范围）
 *
 * 分块传输并在每块之间等 DMA 完成（初始化阶段 LVGL 尚未接管，必须阻塞）。
 *
 * @param rgb565 big-endian 高字节在前的填充色在缓冲中如何排列由本函数负责
 * @return ESP_OK 成功；否则为首个错误
 */
static esp_err_t panel_clear(uint16_t rgb565)
{
    for (size_t i = 0; i < sizeof(s_fill_buf); i += 2) {
        s_fill_buf[i] = (uint8_t)(rgb565 >> 8);      /* 高字节在前 */
        s_fill_buf[i + 1] = (uint8_t)(rgb565 & 0xFF);
    }

    /* 置「清屏等待」标志：本阶段颜色完成回调只给信号量，不转发上层回调 */
    s_clear_waiting = true;
    for (int y = 0; y < LCD_NV3030B_V_RES; y += LCD_FILL_ROWS) {
        int y_end = y + LCD_FILL_ROWS;
        if (y_end > LCD_NV3030B_V_RES) {
            y_end = LCD_NV3030B_V_RES;
        }
        esp_err_t err = lcd_nv3030b_draw_bitmap(0, y, LCD_NV3030B_H_RES, y_end, s_fill_buf);
        if (err != ESP_OK) {
            s_clear_waiting = false;
            return err;
        }
        /* 先清残留 give 再等本轮 DMA：信号量常驻，上一轮/上一块的残留 give
         * 可能还在，不清会把旧完成当成新完成（缓冲提前复用 -> 撕裂） */
        (void)xSemaphoreTake(s_clear_sem, 0);
        if (xSemaphoreTake(s_clear_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGE(TAG, "清屏等待传输完成超时（行块 y=%d）", y);
            s_clear_waiting = false;
            return ESP_ERR_TIMEOUT;
        }
    }
    s_clear_waiting = false;
    return ESP_OK;
}

/* ============================================================
 * 面板初始化序列（厂商随模组验证，勿随意改动；详见文件头说明）
 * 末尾 0x36=0x68(MX|MV|BGR) 决定横屏，0x3A=0x55 为 RGB565，
 * 0x21 为 INVON，0x11/0x29 为退出睡眠/开显示。
 * ============================================================ */
static const spd2010_lcd_init_cmd_t s_lcd_init_cmds[] = {
    { 0xFF, (uint8_t[]){ 0x20, 0x10, 0x10 }, 3, 0 },
    { 0x0C, (uint8_t[]){ 0x11 }, 1, 0 },
    { 0x10, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0x11, (uint8_t[]){ 0x11 }, 1, 0 },
    { 0x15, (uint8_t[]){ 0x42 }, 1, 0 },
    { 0x16, (uint8_t[]){ 0x11 }, 1, 0 },
    { 0x1A, (uint8_t[]){ 0x02 }, 1, 0 },
    { 0x11, NULL, 0, 10 },
    { 0xFD, (uint8_t[]){ 0x06, 0x08 }, 2, 0 },
    { 0x61, (uint8_t[]){ 0x07, 0x04 }, 2, 0 },
    { 0x62, (uint8_t[]){ 0x00, 0x44, 0x45 }, 3, 0 },
    { 0x63, (uint8_t[]){ 0x41, 0x07, 0x12, 0x12 }, 4, 0 },
    { 0x64, (uint8_t[]){ 0x37 }, 1, 0 },
    { 0x65, (uint8_t[]){ 0x09, 0x10, 0x21 }, 3, 0 },
    { 0x66, (uint8_t[]){ 0x09, 0x10, 0x21 }, 3, 0 },
    { 0x67, (uint8_t[]){ 0x20, 0x40 }, 2, 0 },
    { 0x68, (uint8_t[]){ 0x90, 0x4C, 0x7C, 0x66 }, 4, 0 },
    { 0xB1, (uint8_t[]){ 0x0F, 0x02, 0x01 }, 3, 0 },
    { 0xB4, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0xB5, (uint8_t[]){ 0x02, 0x02, 0x0A, 0x14 }, 4, 0 },
    { 0xB6, (uint8_t[]){ 0x04, 0x01, 0x9F, 0x00, 0x02 }, 5, 0 },
    { 0xDF, (uint8_t[]){ 0x11 }, 1, 0 },
    { 0xE2, (uint8_t[]){ 0x13, 0x00, 0x00, 0x30, 0x33, 0x3F }, 6, 0 },
    { 0xE5, (uint8_t[]){ 0x3F, 0x33, 0x30, 0x00, 0x00, 0x13 }, 6, 0 },
    { 0xE1, (uint8_t[]){ 0x00, 0x57 }, 2, 0 },
    { 0xE4, (uint8_t[]){ 0x58, 0x00 }, 2, 0 },
    { 0xE0, (uint8_t[]){ 0x01, 0x03, 0x0E, 0x0E, 0x0C, 0x15, 0x19 }, 7, 0 },
    { 0xE3, (uint8_t[]){ 0x1A, 0x16, 0x0C, 0x0F, 0x0E, 0x0D, 0x02, 0x01 }, 8, 0 },
    { 0xE6, (uint8_t[]){ 0x00, 0xFF }, 2, 0 },
    { 0xE7, (uint8_t[]){ 0x01, 0x04, 0x03, 0x03, 0x00, 0x12 }, 6, 0 },
    { 0xE8, (uint8_t[]){ 0x00, 0x70, 0x00 }, 3, 0 },
    { 0xEC, (uint8_t[]){ 0x52 }, 1, 0 },
    { 0xF1, (uint8_t[]){ 0x01, 0x01, 0x02 }, 3, 0 },
    { 0xF6, (uint8_t[]){ 0x09, 0x10, 0x00, 0x00 }, 4, 0 },
    { 0xFD, (uint8_t[]){ 0xFA, 0xFC }, 2, 0 },
    { 0x3A, (uint8_t[]){ 0x05 }, 1, 0 },
    { 0x36, (uint8_t[]){ 0x08 }, 1, 0 },
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x21, NULL, 0, 1 },
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },
    /* 横屏：MX|MV|BGR（NV3030B 标准 MIPI rotation 1），见文件头 */
    { 0x36, (uint8_t[]){ 0x68 }, 1, 0 },
    { 0x11, NULL, 0, 100 },
    { 0x29, NULL, 0, 0 },
};

esp_err_t lcd_nv3030b_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;

    /* ---- ① 复位脚：由 esp_lcd 托管，这里不再手动配置 ----
     * 历史写法是先在此外 gpio_config() 把 RST 拉高，随后又在 panel_cfg 里
     * 把同一引脚以 reset_gpio_num 交给 esp_lcd —— 等于重复占用同一 GPIO。
     * V6.1 的 GPIO 驱动新增了冲突检测，会因此打印
     * “gpio: conflict found for GPIO[7]” 告警。esp_lcd 在
     * esp_lcd_panel_reset() 内部会自行把该引脚配为输出并输出复位脉冲，
     * 因此此处无需（也不应）再手动配置。 */

    /* ---- ② 建 QSPI 总线（4 根数据线） ----
     * 只写 sclk/data0~3：它们在 spi_bus_config_t 里与旧字段 mosi/miso/
     * quadwp/quadhd 共用匿名联合，同时写两边后者会覆盖前者。 */
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_PIN_LCD_SCK,
        .data0_io_num = BOARD_PIN_LCD_SDIO0,
        .data1_io_num = BOARD_PIN_LCD_SDIO1,
        .data2_io_num = BOARD_PIN_LCD_SDIO2,
        .data3_io_num = BOARD_PIN_LCD_SDIO3,
        .max_transfer_sz = LCD_MAX_TRANSFER_SZ,
    };
    /* spi_bus_initialize：初始化一条 SPI 主机总线；第三个参数 SPI_DMA_CH_AUTO
     * 表示让驱动自动挑一个空闲 DMA 通道（显存数据走 DMA 才不会占用 CPU）。 */
    err = spi_bus_initialize(LCD_QSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QSPI 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ③ 建 panel IO（QSPI） ----
     * 关键字段含义（初学者容易混淆）：
     *   cs_gpio_num        ：片选脚，esp_lcd 每次事务自动拉低/拉高；
     *   dc_gpio_num = -1   ：QSPI 没有独立的「命令/数据」选择脚，
     *                        命令与参数靠 32bit 命令字里的 opcode 区分；
     *   spi_mode = 3       ：SCLK 空闲高电平，第二个边沿采样（本面板要求）；
     *   pclk_hz            ：写显存时钟（见 LCD_QSPI_PCLK_HZ 的实测说明）；
     *   trans_queue_depth  ：异步事务队列深度，决定最多可排队几个刷屏请求；
     *   on_color_trans_done：颜色 DMA 真正结束时在 SPI 中断里回调（见其实现）；
     *   lcd_cmd_bits/param_bits：命令字 32bit、参数 8bit 的线上宽度；
     *   quad_mode          ：4 根数据线并行收发（QSPI）。 */
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_PIN_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 3,
        .pclk_hz = LCD_QSPI_PCLK_HZ,
        .trans_queue_depth = LCD_QSPI_TRANS_DEPTH,
        .on_color_trans_done = lcd_panel_io_color_done_cb,
        .user_ctx = NULL,
        .lcd_cmd_bits = LCD_QSPI_CMD_BITS,
        .lcd_param_bits = LCD_QSPI_PARAM_BITS,
        .flags = {
            .quad_mode = true,
        },
    };
    err = esp_lcd_new_panel_io_spi(LCD_QSPI_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 QSPI panel IO 失败: %s", esp_err_to_name(err));
        goto fail_bus;
    }

    /* ---- ④ 先配背光 PWM（占空比 0 = 灭），保证初始化过程中背光灭 ---- */
    err = backlight_pwm_init();
    if (err != ESP_OK) {
        goto fail_io;
    }

    /* ---- ④.1 创建清屏同步信号量（**常驻不删**，原因见 s_clear_sem 注释） ----
     * 一个二值信号量的内存开销可忽略；相比「清屏后删除」带来的
     * ISR 判空→删除→give 的 use-after-free 竞态，常驻是零风险方案。
     * 「只在为空时创建」：init 失败后重试时不重复创建、不泄漏旧句柄。 */
    if (s_clear_sem == NULL) {
        s_clear_sem = xSemaphoreCreateBinary();
        if (s_clear_sem == NULL) {
            ESP_LOGE(TAG, "创建清屏同步信号量失败");
            goto fail_bl;
        }
    }

    /* ---- ⑤ 创建面板：注入厂商验证初始化序列 ---- */
    const spd2010_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        /* 与厂商 ESP32 例程一致用 RGB；最终 BGR 由序列末尾 0x36=0x68 决定 */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };
    err = esp_lcd_new_panel_spd2010(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建面板驱动失败: %s", esp_err_to_name(err));
        goto fail_bl;
    }

    /* ---- ⑥ 复位 → 初始化（序列内已含方向/反显/SLPOUT/DISPON） ----
     * 不再调用组件的 swap_xy/invert_color：方向已由序列末尾 MADCTL=0x68
     * 决定，反显已由序列内 0x21 决定。 */
    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板复位失败: %s", esp_err_to_name(err));
        goto fail_panel;
    }
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板初始化失败: %s", esp_err_to_name(err));
        goto fail_panel;
    }

    s_initialized = true;

    /* ---- ⑦ 先刷清屏色（阻塞等 DMA），**暂不点背光** ----
     * 背光由上层（lvgl_port）在第一帧真正上屏后才打开：
     * 本驱动 init 到 LVGL 首帧之间还有一段时间（自检、字体加载等），
     * 这段窗口里面板只有清屏色/未定义内容，此时若已开背光，用户会先看到
     * 「黑底 + 局部条带」再跳到启动画面。保持背光关闭，用户看到的第一帧
     * 就是完整的启动画面。上层可通过 lcd_nv3030b_set_backlight() 打开。
     * 清屏同步信号量已在 ④.1 创建（常驻），此处直接用。 */
    err = panel_clear(LCD_CLEAR_COLOR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "清屏失败: %s", esp_err_to_name(err));
        s_initialized = false;
        goto fail_panel;
    }

#if (LCD_BOOT_TEST_PATTERN_MS > 0)
    err = panel_draw_test_pattern();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "显示自检图案失败: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "【诊断】已显示自检图案（横屏 红/绿/蓝/白/黑 5 条横色条），停留 %d ms",
             LCD_BOOT_TEST_PATTERN_MS);
    vTaskDelay(pdMS_TO_TICKS(LCD_BOOT_TEST_PATTERN_MS));
    (void)panel_clear(LCD_CLEAR_COLOR);
#endif

    /* ⚠ 不在此处删除 s_clear_sem（历史写法，曾引入 ISR use-after-free 竞态）：
     * 颜色完成回调在 SPI 中断上下文判 s_clear_waiting 后 give 该信号量，
     * 删除与回调并发会踩已释放内存。信号量常驻（见 s_clear_sem 注释）。 */

    ESP_LOGI(TAG, "初始化完成（%dx%d 横屏，QSPI %d Hz，esp_lcd_spd2010 + 厂商序列(MADCTL=0x68)，背光 IO%d）",
             LCD_NV3030B_H_RES, LCD_NV3030B_V_RES, LCD_QSPI_PCLK_HZ, BOARD_PIN_LCD_BL);
    return ESP_OK;

/* ---------------- 失败清理（按创建顺序逆序释放） ---------------- */
fail_panel:
    if (s_panel != NULL) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
fail_bl:
    if (s_bl_ready) {
        (void)ledc_stop(BL_LEDC_MODE, BL_LEDC_CHANNEL, 0);
        s_bl_ready = false;
    }
fail_io:
    if (s_io != NULL) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
fail_bus:
    spi_bus_free(LCD_QSPI_HOST);
    s_initialized = false;
    return err;
}

esp_err_t lcd_nv3030b_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bl_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t duty = (uint32_t)percent * BL_DUTY_MAX / 100U;
    /* ledc_set_duty 只把占空比写进影子寄存器，ledc_update_duty 才让它生效，
     * 两步必须成对调用（否则亮度不会变化）。 */
    esp_err_t err = ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

esp_err_t lcd_nv3030b_draw_bitmap(int x_start, int y_start, int x_end, int y_end,
                                  const void *color_data)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (color_data == NULL || x_end <= x_start || y_end <= y_start) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 坐标与 esp_lcd_panel_draw_bitmap 同为左闭右开，直接透传 */
    return esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t lcd_nv3030b_set_rotation(lcd_nv3030b_rotation_t rot)
{
    if (rot > LCD_NV3030B_ROT_270) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 2026-10 架构变更：面板物理方向永久固定为 ROT_90（MADCTL 0x68，已在
     * 厂商 init 序列里写过一次），运行期绝不再向面板写 MADCTL。
     * 原因：QSPI 面板的寄存器命令(tx_param)与颜色 DMA(tx_color)共用同一
     * panel IO，esp_lcd panel IO 非线程安全；运行期在刷屏流中插入 MADCTL
     * 会使面板控制器状态异常（实测快速刷新若干次后整屏冻结）。屏幕方向改由
     * LVGL 软件旋转完成（见 lvgl_port.c 的 sw_rotate + lv_display_set_rotation），
     * 面板始终只按 284x240 物理横屏收数据。这里只记录档位，不产生总线事务。 */
    (void)s_rot_madctl;
    s_rotation = rot;
    ESP_LOGI(TAG, "显示旋转档位=ROT_%d（面板 MADCTL 保持 0x68，旋转在 LVGL 软件层）", rot * 90);
    return ESP_OK;
}

lcd_nv3030b_rotation_t lcd_nv3030b_get_rotation(void)
{
    return s_rotation;
}

void lcd_nv3030b_get_resolution(lcd_nv3030b_rotation_t rot, int *width, int *height)
{
    /* ROT_90/270 为横屏：宽 = 物理高(284)，高 = 物理宽(240)；ROT_0/180 反之 */
    const bool landscape = (rot == LCD_NV3030B_ROT_90) || (rot == LCD_NV3030B_ROT_270);
    if (width != NULL) {
        *width = landscape ? LCD_NV3030B_PHYS_H : LCD_NV3030B_PHYS_W;
    }
    if (height != NULL) {
        *height = landscape ? LCD_NV3030B_PHYS_W : LCD_NV3030B_PHYS_H;
    }
}
