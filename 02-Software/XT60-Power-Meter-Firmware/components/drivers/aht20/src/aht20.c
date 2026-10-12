/**
 * @file aht20.c
 * @brief AHT20 温湿度传感器驱动实现（I2C，7 位地址 0x38）
 *
 * 器件：AHT20 数字温湿度传感器，I2C 7 位地址 0x38（BOARD_AHT20_I2C_ADDR）。
 * 引脚：SCL=IO21、SDA=IO47（与 SC7A20H 共享「传感器 I2C 总线」，见 board.h）。
 *
 * 阅读顺序建议：
 *   ① 命令与寄存器（下方 CMD_* / 状态位）；
 *   ② aht20_init()  —— 上电校准 + 首次测量触发；
 *   ③ aht20_read()  —— 触发测量 -> 等待就绪 -> 解析温湿度。
 *
 * ============================================================
 * 为什么自实现，而非托管组件 espressif/aht20
 * ============================================================
 * 托管组件 espressif/aht20 基于 esp-iot-solution 的 legacy i2c_bus
 * （旧 driver/i2c.h）。本工程 AHT20 与 SC7A20H 共享同一 I2C 外设（I2C1），
 * 由 bsp 的 board_i2c_sensor_bus_get() 以新版 i2c_master 统一持有总线；
 * 若 AHT20 走 legacy 总线、SC7A20H 走新 i2c_master，会对同一 I2C 外设形成
 * 双驱动冲突。且 IDF 5.5 对 legacy I2C 有 deprecation 告警。
 * 故统一用官方底层驱动 esp_driver_i2c（新 i2c_master）实现，共享同一总线。
 *
 * ============================================================
 * 协议要点（AHT20）
 * ============================================================
 *   上电：延时 ≥40ms -> 读状态，若未校准(bit3=0) 发初始化命令 0xBE 0x08 0x00 -> 延时 10ms。
 *   测量：发触发命令 0xAC 0x33 0x00 -> 延时 ≥80ms -> 读 7 字节。
 *   状态字节：bit7=忙(1=测量/校准中)，bit3=已校准。
 *   数据：湿度 20bit = byte1..3 高位，温度 20bit = byte3 低 4bit + byte4..5。
 *         湿度% = raw_h / 2^20 * 100；温度°C = raw_t / 2^20 * 200 - 50。
 *
 * ============================================================
 * 上板调试要点（读数异常/超时排查）
 * ============================================================
 *   · ESP_ERR_TIMEOUT（器件未响应）：核对 I2C 地址 0x38、上拉、SCL/SDA 引脚，
 *     以及是否与 SC7A20H 共用总线后时序冲突。
 *   · 读数恒为 0 / 不变：多为忙位未清就解析，增大 AHT20_MEASURE_WAIT_MS。
 *   · 温湿度明显偏差：对照标定，必要时按数据手册微调换算系数。
 * ============================================================
 */
#include "aht20.h"
#include "aht20_calc.h"

#include <string.h>

#include "board.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** 日志标签 */
static const char *TAG = "aht20";

/* I2C / 时序参数 */
#define AHT20_I2C_ADDR        BOARD_AHT20_I2C_ADDR /**< 0x38 */
#define AHT20_I2C_FREQ_HZ     100000
#define AHT20_I2C_TIMEOUT_MS  100

#define AHT20_POWERON_WAIT_MS  40 /**< 上电稳定延时 */
#define AHT20_INIT_WAIT_MS     10 /**< 初始化命令后延时 */
#define AHT20_MEASURE_WAIT_MS  80 /**< 触发测量后等待转换 */
#define AHT20_BUSY_POLL_MS     5  /**< 忙位轮询间隔 */
#define AHT20_BUSY_MAX_POLLS   20 /**< 忙位轮询上限（防死等） */

/* 命令序列 */
static const uint8_t AHT20_CMD_INIT[3]    = { 0xBE, 0x08, 0x00 }; /**< 校准使能 */
static const uint8_t AHT20_CMD_TRIGGER[3] = { 0xAC, 0x33, 0x00 }; /**< 触发测量 */

/* 状态字节位 */
#define AHT20_STATUS_BUSY       0x80 /**< bit7 忙 */
#define AHT20_STATUS_CALIBRATED 0x08 /**< bit3 已校准 */

/** 读回字节数：状态 + 湿度 3B + 温度 2B + CRC */
#define AHT20_READ_LEN 7

/** 是否已初始化 */
static bool s_initialized = false;

/* I2C 器件句柄（挂在 bsp 共享传感器总线上） */
static i2c_master_dev_handle_t s_dev = NULL;

/**
 * @brief 读状态字节
 */
static esp_err_t aht20_read_status(uint8_t *status)
{
    return i2c_master_receive(s_dev, status, 1, AHT20_I2C_TIMEOUT_MS);
}

/**
 * @brief 等待器件忙位清除
 * @return ESP_OK 就绪；ESP_ERR_TIMEOUT 超时仍忙
 */
static esp_err_t aht20_wait_ready(void)
{
    for (int i = 0; i < AHT20_BUSY_MAX_POLLS; i++) {
        uint8_t status = 0;
        esp_err_t err = aht20_read_status(&status);
        if (err != ESP_OK) {
            return err;
        }
        if ((status & AHT20_STATUS_BUSY) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(AHT20_BUSY_POLL_MS));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t aht20_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- ① 取共享传感器 I2C 总线（与 SC7A20H 共用同一总线控制器） ---- */
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = board_i2c_sensor_bus_get(&bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "获取传感器 I2C 总线失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ② 挂载 AHT20 器件（7 位地址） ---- */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT20_I2C_ADDR,
        .scl_speed_hz = AHT20_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "添加 AHT20 器件失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ③ 上电稳定 ---- */
    vTaskDelay(pdMS_TO_TICKS(AHT20_POWERON_WAIT_MS));

    /* ---- ④ 校准使能（若未校准则发初始化命令） ---- */
    uint8_t status = 0;
    err = aht20_read_status(&status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读 AHT20 状态失败: %s", esp_err_to_name(err));
        return err;
    }
    if ((status & AHT20_STATUS_CALIBRATED) == 0) {
        err = i2c_master_transmit(s_dev, AHT20_CMD_INIT, sizeof(AHT20_CMD_INIT),
                                  AHT20_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AHT20 初始化命令失败: %s", esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(AHT20_INIT_WAIT_MS));
    }

    /* ---- ⑤ 首次测量触发（让器件进入已就绪状态） ---- */
    err = i2c_master_transmit(s_dev, AHT20_CMD_TRIGGER, sizeof(AHT20_CMD_TRIGGER),
                              AHT20_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AHT20 首次触发失败: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(AHT20_MEASURE_WAIT_MS));

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成（I2C 地址 0x%02X）", AHT20_I2C_ADDR);
    return ESP_OK;
}

esp_err_t aht20_read(aht20_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- ① 触发一次测量 ---- */
    esp_err_t err =
        i2c_master_transmit(s_dev, AHT20_CMD_TRIGGER, sizeof(AHT20_CMD_TRIGGER),
                            AHT20_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* ---- ② 等待转换完成 ---- */
    vTaskDelay(pdMS_TO_TICKS(AHT20_MEASURE_WAIT_MS));
    err = aht20_wait_ready();
    if (err != ESP_OK) {
        /* 器件未响应/一直忙 */
        return (err == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : err;
    }

    /* ---- ③ 读回数据 ---- */
    uint8_t buf[AHT20_READ_LEN] = { 0 };
    err = i2c_master_receive(s_dev, buf, AHT20_READ_LEN, AHT20_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* 忙位应已清零；若仍置位说明数据无效 */
    if (buf[0] & AHT20_STATUS_BUSY) {
        return ESP_ERR_TIMEOUT;
    }

    /* ---- ④ CRC8 校验（手册 §5.2：对前 6 字节 CRC8，初值 0xFF/多项式 0x31） ----
     * 总线干扰或读时序错位会导致数据损坏；校验不过直接判失败，避免把错误温湿度
     * 当作有效数据上报（错误数据比“本次读不到”危害更大）。 */
    if (aht20_crc8(buf, AHT20_READ_LEN - 1) != buf[AHT20_READ_LEN - 1]) {
        ESP_LOGW(TAG, "AHT20 数据 CRC8 校验失败（计算=0x%02X 器件=0x%02X），丢弃本次读数",
                 aht20_crc8(buf, AHT20_READ_LEN - 1), buf[AHT20_READ_LEN - 1]);
        return ESP_ERR_INVALID_CRC;
    }

    /* ---- ⑤ 解析 20bit 湿度 / 温度 ----
     * 湿度 = (b1<<12 | b2<<4 | b3>>4)，温度 = ((b3&0x0F)<<16 | b4<<8 | b5) */
    uint32_t raw_h = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | ((uint32_t)buf[3] >> 4);
    uint32_t raw_t = (((uint32_t)buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];

    /* 湿度物理量程钳制 0~100%RH（温度为机内环境量，保留原值不钳制） */
    out->humidity_pct =
        aht20_clamp_rh((float)raw_h * 100.0f / 1048576.0f);       /* / 2^20 * 100 */
    out->temperature_c = (float)raw_t * 200.0f / 1048576.0f - 50.0f; /* / 2^20 * 200 - 50 */

    return ESP_OK;
}
