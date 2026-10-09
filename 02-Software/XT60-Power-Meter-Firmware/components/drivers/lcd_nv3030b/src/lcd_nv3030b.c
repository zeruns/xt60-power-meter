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
 *   ④ 刷确定底色、开背光；LVGL 刷屏走 lcd_nv3030b_draw_bitmap()。
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
 *     （big-endian），LVGL 用 RGB565_SWAPPED；
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
 * 实测结论（本模组 + 本板走线）：**30MHz 稳定，40MHz 会花屏**，
 * 说明该面板/走线的时序余量不足以支撑 40MHz（厂商例程取 20MHz 偏保守）。
 * 因此定格在 30MHz。若换批次后仍出现花屏/丢像素，依次降 20/10MHz 定位。
 * 注意：改这个值只影响写显存速率，不影响命令时序。 */
#define LCD_QSPI_PCLK_HZ     (30 * 1000 * 1000)
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

/** 清屏 / 自检图案的「等待传输完成」同步信号量（运行期置 NULL，改用上层回调） */
static SemaphoreHandle_t s_clear_sem = NULL;

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
            return err;
        }
        if (xSemaphoreTake(s_clear_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}
#endif /* LCD_BOOT_TEST_PATTERN_MS */

/**
 * @brief esp_lcd 颜色传输完成回调（SPI 中断上下文）
 *
 * esp_lcd 对颜色数据走 spi_device_queue_trans（异步），只有这个回调触发时
 * 才代表 DMA 真正结束、缓冲可安全复用。
 *
 * @return 固定返回 false：不在回调里额外触发任务切换
 */
static bool lcd_panel_io_color_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    if (s_clear_sem != NULL) {
        /* 清屏/自检阶段：发信号量让任务等传输完成 */
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

    for (int y = 0; y < LCD_NV3030B_V_RES; y += LCD_FILL_ROWS) {
        int y_end = y + LCD_FILL_ROWS;
        if (y_end > LCD_NV3030B_V_RES) {
            y_end = LCD_NV3030B_V_RES;
        }
        esp_err_t err = lcd_nv3030b_draw_bitmap(0, y, LCD_NV3030B_H_RES, y_end, s_fill_buf);
        if (err != ESP_OK) {
            return err;
        }
        if (xSemaphoreTake(s_clear_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGE(TAG, "清屏等待传输完成超时（行块 y=%d）", y);
            return ESP_ERR_TIMEOUT;
        }
    }
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

    /* ---- ① 复位脚：输出 + 默认高（不复位） ---- */
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&rst_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "复位脚 IO%d 配置失败: %s", BOARD_PIN_LCD_RST, esp_err_to_name(err));
        return err;
    }
    gpio_set_level(BOARD_PIN_LCD_RST, 1);

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
    err = spi_bus_initialize(LCD_QSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QSPI 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ③ 建 panel IO（QSPI） ---- */
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
     * 就是完整的启动画面。上层可通过 lcd_nv3030b_set_backlight() 打开。 */
    s_clear_sem = xSemaphoreCreateBinary();
    if (s_clear_sem == NULL) {
        ESP_LOGE(TAG, "创建清屏同步信号量失败");
        goto fail_panel;
    }
    err = panel_clear(LCD_CLEAR_COLOR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "清屏失败: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_clear_sem);
        s_clear_sem = NULL;
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

    vSemaphoreDelete(s_clear_sem);
    s_clear_sem = NULL;

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

    const uint8_t madctl = s_rot_madctl[rot];
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, LCD_QSPI_CMD(LCD_CMD_MADCTL), &madctl, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写 MADCTL(0x%02X) 失败: %s", madctl, esp_err_to_name(err));
        return err;
    }

    s_rotation = rot;
    ESP_LOGI(TAG, "显示旋转切换为 ROT_%d（MADCTL=0x%02X）", rot * 90, madctl);
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
