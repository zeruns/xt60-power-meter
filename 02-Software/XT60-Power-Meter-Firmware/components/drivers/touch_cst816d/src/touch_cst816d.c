/**
 * @file touch_cst816d.c
 * @brief CST816D 电容触摸驱动实现（I2C 读点，轮询方式）
 *
 * 器件：CST816D 电容触摸控制器，I2C 7 位地址 0x15（BOARD_TP_I2C_ADDR）。
 * 引脚：SCL=IO5、SDA=IO6、INT=IO46（见 board_pins.h）。
 *
 * 阅读顺序建议：
 *   ① 读点寄存器映射（下方 REG_* 与 parse_point()）；
 *   ② touch_cst816d_read() —— 读一组寄存器并解析出坐标与按下状态。
 *
 * 通信：轮询读寄存器（厂商例程 Touch_Test 即轮询，不依赖中断）。
 *   读时序 = 写寄存器地址 0x00 →（重复起始）连续读 N 字节，
 *   与 I2C 标准「写寄存器地址 + 重复起始读」一致，用
 *   i2c_master_transmit_receive() 一步完成。
 *
 * ============================================================
 * 关于 strapping 脚 IO46（TP_INT）
 * ============================================================
 * IO46 是 strapping 脚，复位期间电平影响启动，硬件已保证上电为高。
 * 软件侧须避免在启动早期把它拉低：本驱动把 INT 配成「输入 + 上拉」，
 * 不会主动驱动，故不会拉低，安全。
 *
 * ============================================================
 * 上板调试要点（触摸无响应/坐标不对时排查）
 * ============================================================
 *   · 读不到器件：核对 I2C 地址 0x15、上拉电阻、SCL/SDA 引脚。
 *   · 有按下但坐标乱：多为寄存器映射与批次不符。下方解析采用厂商
 *     例程实测映射（见 parse_point 注释）；若换批次，标准 CST816D 映射
 *     为 X=((buf[2]&0x0F)<<8)|buf[3]、Y=((buf[4]&0x0F)<<8)|buf[5]，
 *     按下判据为 buf[1](手指数)>0。二选一即可，见注释。
 *   · 横屏坐标与显示不匹配：坐标变换在 lvgl_port 的输入设备读取里做，
 *     本驱动只返回触摸 IC 的物理坐标（竖屏 240x284 域）。
 * ============================================================
 */
#include "touch_cst816d.h"

#include <string.h>

#include "board_pins.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** 日志标签 */
static const char *TAG = "touch_cst816d";

/* I2C 时序参数 */
#define TP_I2C_PORT        I2C_NUM_0
#define TP_I2C_FREQ_HZ     100000 /* 100kHz，触摸足够 */
#define TP_I2C_TIMEOUT_MS  20

/** 触摸面板物理分辨率（竖屏域，触摸 IC 出值范围） */
#define TP_PHYS_H_RES 240
#define TP_PHYS_V_RES 284

/* 读点：从寄存器 0x00 起连续读 8 字节（厂商例程同款） */
#define TP_REG_MAP_BASE 0x00
#define TP_READ_LEN     8

/* 是否已初始化 */
static bool s_initialized = false;

/* I2C 句柄 */
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/**
 * @brief 解析 8 字节寄存器原始数据为触摸点
 *
 * 采用厂商例程（STM32 Touch_Test）的实测映射（本模块批次已验证）：
 *   buf[3] 高 2bit 为手势/事件标志位（event flag）：
 *     10b = 按下（press down）、00b/01b/11b = 抬起/滑动/无事件等。
 *     → 有按下判据 **(buf[3] & 0xC0) == 0x80**（掩码判据）。
 *   X             =  buf[4]          → 横坐标（8bit，0~239，竖屏域）
 *   Y             = (buf[5]&0x0F)<<8 | buf[6] → 纵坐标（12bit，0~283，竖屏域）
 *
 * ⚠ 判据为什么用掩码而不用 buf[3] == 0x80 严格相等：
 *   buf[3] 低 6bit 会携带手势/序号等附加信息（滑动方向、事件序号等），
 *   严格等于 0x80 时只要低位带任何手势位就判成"未按下"，表现为拖动/滑动
 *   中丢点、按下识别过于敏感。按数据手册语义只认高 2bit 的事件标志位，
 *   低位无论携带什么都应视为"有按下"。
 *
 * 若换触摸批次后坐标不对，改用标准 CST816D 映射（见文件头注释）：
 *   pressed = buf[1] > 0;
 *   x = ((buf[2] & 0x0F) << 8) | buf[3];
 *   y = ((buf[4] & 0x0F) << 8) | buf[5];
 *
 * @param buf 8 字节寄存器原始值
 * @param out 输出触摸点（物理竖屏坐标）
 */
static void parse_point(const uint8_t buf[TP_READ_LEN], touch_cst816d_point_t *out)
{
    if ((buf[3] & 0xC0u) == 0x80u) {
        out->pressed = true;
        out->x = buf[4];
        out->y = (uint16_t)(((buf[5] & 0x0F) << 8) | buf[6]);

        /* 越界保护：换批次时坐标可能超出面板范围，截断避免上层算错 */
        if (out->x >= TP_PHYS_H_RES) {
            out->x = TP_PHYS_H_RES - 1;
        }
        if (out->y >= TP_PHYS_V_RES) {
            out->y = TP_PHYS_V_RES - 1;
        }
    } else {
        out->pressed = false;
        out->x = 0;
        out->y = 0;
    }
}

esp_err_t touch_cst816d_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- ① INT 脚：输入 + 上拉（不主动驱动，避免 strapping 脚被拉低） ---- */
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_TP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&int_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "INT 脚 IO%d 配置失败: %s", BOARD_PIN_TP_INT, esp_err_to_name(err));
        return err;
    }

    /* ---- ② 建 I2C 主机总线 ---- */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TP_I2C_PORT,
        .sda_io_num = BOARD_PIN_TP_SDA,
        .scl_io_num = BOARD_PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* 板上已有 2.2k 上拉，内部上拉兜底 */
    };
    err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸 I2C 总线创建失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ③ 添加触摸器件（7 位地址） ---- */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TP_I2C_ADDR,
        .scl_speed_hz = TP_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "添加触摸器件失败: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成（I2C 地址 0x%02X，SCL=IO%d SDA=IO%d INT=IO%d）", BOARD_TP_I2C_ADDR,
             BOARD_PIN_TP_SCL, BOARD_PIN_TP_SDA, BOARD_PIN_TP_INT);
    return ESP_OK;
}

esp_err_t touch_cst816d_read(touch_cst816d_point_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg = TP_REG_MAP_BASE;
    uint8_t buf[TP_READ_LEN] = { 0 };

    /* 写寄存器地址 0x00 → 重复起始 → 读 8 字节 */
    esp_err_t err =
        i2c_master_transmit_receive(s_dev, &reg, 1, buf, TP_READ_LEN, TP_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        /* 读失败（器件忙/NACK）不视为致命，返回错误由上层按"未按下"处理 */
        out->pressed = false;
        out->x = 0;
        out->y = 0;
        return err;
    }

    parse_point(buf, out);
    return ESP_OK;
}
