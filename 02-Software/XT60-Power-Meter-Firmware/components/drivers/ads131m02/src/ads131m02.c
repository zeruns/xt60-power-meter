/**
 * @file ads131m02.c
 * @brief ADS131M02 双通道 24bit 同步采样 ΔΣ ADC 驱动实现（SPI + DRDY 中断）
 *
 * 器件：TI ADS131M02，2 通道同步采样 24bit ΔΣ ADC。
 *   AIN0 —— **电流**，PGA 增益 16x（1mΩ 四端开尔文分流器 ISENSE±）
 *   AIN1 —— **电压**，PGA 增益 1x（U7 电压跟随器 + 101:1 精密分压 R33 100kΩ / R39 1kΩ）
 * ⚠ 通道与直觉相反，依据上板原理图 V2 的实际接法；GAIN1 = 0x04，详见 ADS_GAIN1_VALUE 注释。
 * 引脚：见 board_pins.h 的 BOARD_PIN_ADC_*。
 *
 * 阅读顺序建议：
 *   ① 命令编码宏（下方 ADS_CMD_* / ADS_CMD_RREG / ADS_CMD_WREG）；
 *   ② ads_xfer_raw()    —— 本驱动唯一的 SPI 原语：一次事务交换 N 个 24bit 字；
 *   ③ ads131m02_init()  —— 建 SPI、硬复位、RESET 验证、UNLOCK、写 GAIN1、装 DRDY 中断；
 *   ④ ads131m02_read()  —— 等 DRDY# 下降沿 -> 交换 3 个 NULL 字 -> 取 CH0 / CH1。
 *
 * ============================================================
 * SPI 时序要点（TI 数据手册）
 * ============================================================
 *   · SPI mode 1（CPOL = 0, CPHA = 1）：SCLK 空闲低；数据在 SCLK 上升沿改变、
 *     下降沿锁存（主机与从机一致），故主机侧使用 mode 1。
 *   · 命令与数据都是 24bit 字、MSB 先出；16bit 命令码 MSB 对齐（cmd << 8）。
 *   · 命令响应出现在「命令帧之后的那一帧」的首字（TI 手册原文：the device outputs
 *     the register contents in the command response word of the following frame）。
 *     由于一帧 = STATUS + CH0 + CH1 = 3 个字，读寄存器必须时钟**两整帧**，
 *     响应在 rx[3]。见 ADS_XFER_MAX_WORDS / ads_read_reg() 的说明。
 *   · 转换数据帧 = [STATUS 24bit][CH0 24bit][CH1 24bit] = 9 字节（本例未启用 CRC；
 *     若后续在 MODE 寄存器里启用 CRC，必须同步修改 ADS_FRAME_WORDS）。
 *
 * ============================================================
 * 为什么用 SPI3_HOST，而不是文档里写的「独占 SPI2」
 * ============================================================
 * docs/hardware_pinout.md §5 要求 ADS131M02「独占 SPI2」，但其本意是
 * 「测量链路独占一条不被其他设备事务插入打断的总线」，不是字面上的外设编号
 * ——该结论写在 LCD 分配之前。当前 LCD（QSPI 4bit）已占用 SPI2_HOST，
 * 而 ESP32-S3 的一个 SPI 外设同一时刻只能映射一组引脚（LCD 在 IO15~18，
 * ADC 在 IO9~12，无法共存），因此这里改用 SPI3_HOST：同样满足「独占、不被打断」，
 * 且不改动已经跑通的 LCD 驱动。
 *
 * ============================================================
 * 硬件注意：本板 ADS131M02 没有片选（CS）引脚
 * ============================================================
 * hardware_pinout.md 的连接表只有 SCLK / MOSI / MISO / DRDY# / RESET# / CLK 六项，
 * 没有 CS —— 即片选在硬件上常接地、器件永远处于选中状态。因此：
 *   · spi_device 配置 spics_io_num = -1（不让 SPI 主机去驱动不存在的片选）；
 *   · 帧边界只能靠「24bit 字的整数倍」保证，故一次采样必须放在同一个 SPI 事务里
 *     连续交换 3 个字（ads_xfer_raw 就是这么做的），绝不可拆成多次事务，否则帧错位。
 *   · 若上板后读数恒为乱码或固定值，先确认硬件 CS 是否真的接地。
 *
 * ============================================================
 * 上板调试要点（读不到 / 读数不对时按顺序排查）
 * ============================================================
 *   · 【时钟来源】本板 ADC 的 MCLK 由 R27 / R29 两个 0Ω 电阻二选一：
 *       R27 = 由 ESP32 在 IO14(BOARD_PIN_ADC_CLK) 输出 8.192MHz；
 *       R29 = 板载 8.192MHz 有源晶振直接供给。
 *     本驱动默认按「板载晶振」实现——IO14 只配成高阻输入、不输出时钟。
 *     若实焊的是 R27，ADC 就没有时钟：DRDY# 永远不来，ads131m02_read() 会恒返回
 *     ESP_ERR_TIMEOUT。此时需要另行实现 IO14 的 8.192MHz 输出
 *     （LEDC 无法精确产生该频率，建议用 MCPWM 或 RMT 分频）。
 *   · 【通信锚点】复位后读 ID 寄存器，其高字节固定为 0x22（手册复位值 22xxh，
 *     低 8 位“可能变化”）。ID 读不到/高字节不对，说明 SPI 通路或字序有问题。
 *   · 【响应槽位】寄存器访问的响应在「命令帧之后那一帧」的首字（rx[3]）。
 *     若读回值随每次上电变化（像转换数据），说明时钟的字数没凑够整帧、
 *     没走到响应帧——这是本驱动曾踩过的坑，见 ADS_XFER_MAX_WORDS 注释。
 *     需要看原始字时把本文件日志级别开到 DEBUG，ads_read_reg() 会打印 6 个字。
 *   · 【帧错位】没有 CS 时若帧错位，读出的电压/电流会呈现规律的错位关系。
 *     排查点：MODE 寄存器里的字长设置、是否启用了 CRC。
 *   · 【中断与内存】DRDY ISR 只做 xSemaphoreGiveFromISR，且 ISR 与
 *     ads131m02_read() 均不访问 PSRAM（见 hardware_pinout.md §6 的硬约束）。
 * ============================================================
 */
#include "ads131m02.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "board_pins.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** 日志标签 */
static const char *TAG = "ads131m02";

/* ============================================================
 * SPI 参数
 * ============================================================ */
/** 独占 SPI3（SPI2 已被 LCD QSPI 占用，理由见文件头） */
#define ADS_SPI_HOST   SPI3_HOST
/** 供日志打印用：枚举值 SPI3_HOST=2 直接打印会显示成 "SPI2"，容易误导 */
#define ADS_SPI_HOST_NAME "SPI3"
/** 保守时钟：先保证无 CS 长线通信可靠，稳定后可上调 */
#define ADS_SPI_CLK_HZ (4 * 1000 * 1000)
/** SPI mode 1（CPOL = 0 / CPHA = 1） */
#define ADS_SPI_MODE   1

/** 24bit 字长（字节） */
#define ADS_WORD_BYTES 3
/** 一帧数据 = STATUS + CH0 + CH1，共 3 个 24bit 字；也是本驱动的最大事务字数 */
#define ADS_FRAME_WORDS 3

/**
 * 帧同步模型（本驱动所有 SPI 访问都遵守，出问题先看这里）
 *
 * 本器件的**帧边界由 DRDY# 界定**：主机在每个 DRDY 下降沿之后时钟**恰好一帧**
 * （3 个字 = 9 字节）。命令字放在帧的首字，其**响应出现在下一帧的首字**
 * （TI 手册：the device outputs the register contents in the command response
 *   word of the following frame；TI E2E 亦明确"next frame"）。
 *
 * 所以寄存器访问 = 「等 DRDY → 时钟一帧发命令」+「等 DRDY → 时钟一帧读响应」，
 * 见 ads_read_reg() / ads_write_reg() / ads_send_cmd()。
 *
 * ⚠ 曾踩的坑：不等 DRDY、一次性时钟两个帧就想一次读完响应。结果帧边界漂移，
 *   读到的"响应"其实是转换数据 —— 表现为读回值随每次上电都不同
 *   （GAIN1 读到 0x0013/0x40A6/0x00A6，ID 读到 0x2818/0x333E）。
 *   而 ads131m02_read()（等 DRDY 后读一帧）一直是对的，正说明帧同步是关键。
 */

/* ============================================================
 * 命令编码（手册 Table 8-11）
 * ============================================================ */
#define ADS_CMD_NULL   0x0000u /**< 空命令：响应为 STATUS 寄存器 */
#define ADS_CMD_UNLOCK 0x0655u /**< 解锁寄存器写保护（与 LOCK 0x0555 成对） */

/**
 * RREG：格式 101a aaaa annn nnnn
 *   7bit 寄存器地址在 [12:6]，7bit 字数（寄存器个数 - 1）在 [5:0]。
 * @param addr 起始寄存器地址
 * @param n 连续读取的寄存器个数（1 ~ 128）
 */
#define ADS_CMD_RREG(addr, n) \
    (0xA000u | (((uint32_t)(addr) & 0x7Fu) << 7) | (((uint32_t)(n) - 1u) & 0x7Fu))

/**
 * WREG：格式 011a aaaa annn nnnn（位域含义同 RREG）
 * @param addr 起始寄存器地址
 * @param n 连续写入的寄存器个数（1 ~ 128）
 */
#define ADS_CMD_WREG(addr, n) \
    (0x6000u | (((uint32_t)(addr) & 0x7Fu) << 7) | (((uint32_t)(n) - 1u) & 0x7Fu))

/* ============================================================
 * 寄存器
 * ============================================================ */
#define ADS_REG_ID    0x00u /**< 器件 ID（只读，手册典型值见 ADS_DEV_ID_TYPICAL） */
#define ADS_REG_CLOCK 0x03u /**< 时钟/采样率：CH_EN、TBM、OSR[2:0]、PWR[1:0] */
#define ADS_REG_GAIN1 0x04u /**< 增益：CH0 = PGAGAIN0[2:0]（低 3 位），CH1 = PGAGAIN1[2:0]（bit6:4） */
/**
 * CH0(AIN0) = 16x（电流）、CH1(AIN1) = 1x（电压）→ 0x04。
 *
 * ⚠ 两处曾出错，都在此注释固化（依据：手册 §8.6.5 Table 8-18 + 原理图 V2）：
 *
 * 1) 增益编码是**线性表**，不是反向表（手册 Table 8-18 原文）：
 *      PGAGAINn[2:0]: 000b=1, 001b=2, 010b=4, 011b=8, 100b=16, 101b=32, 110b=64, 111b=128
 *
 * 2) **通道接法与直觉相反**（见 01-Hardware/02-上板原理图_V2.pdf）：
 *      AIN0P/AIN0N（CH0）← ISENSE+ / ISENSE-，即**电流**（1mΩ 四端分流器，需 16x）
 *      AIN1P/AIN1N（CH1）← 电压跟随器 U7 输出 / VOUT-，即**电压**（101:1 分压，需 1x）
 *    原理图标注亦印证：电压量程 121V@1x、电流量程 ±75A@16x。
 *
 *    曾误写 0x40（把 CH0 当电压、CH1 当电流），导致电压/电流互换；
 *    还曾误按"反向增益表"写 0x37，把电流通道设成 8x、电压通道设成 128x。
 *
 * CH1 = 000b → 1x（bit6:4）；CH0 = 100b → 16x（bit2:0）=> 0x04。
 */
#define ADS_GAIN1_VALUE 0x0004u
/** 器件 ID 手册典型值；作为本驱动最可靠的通信验证锚点（已上板实测一致） */
#define ADS_DEV_ID_TYPICAL 0x2205u

/* ============================================================
 * CLOCK 寄存器位定义（手册 §8.6.4 Table 8-17，复位值 0x030E）
 *   bit9    CH1_EN / bit8 CH0_EN：两通道均使能；
 *   bit5    TBM：0 = OSR 由 bit[4:2] 决定（1 = Turbo，OSR 固定 64）；
 *   bit[4:2] OSR[2:0]：011 = 1024（复位档）；
 *   bit[1:0] PWR[1:0]：10 = HR（高分辨率，11 同为 HR，保持复位值 10）。
 *
 * HR 模式 f_MOD = f_CLKIN/2 = 4.096MHz，ODR = f_MOD/OSR = 4000 Hz（复位档）。
 *
 * ⚠ 2026-10 实机排查（SBAS839 §8.5.1.5 原文）：
 *   "The DRDY pulse is blocked when new conversions complete while conversion
 *    data are read."——主机读取窗口跨越转换完成时刻时 DRDY 被抑制。
 *   4kHz 档（250µs/帧）下主机"等DRDY+读帧"周期 ~3ms 必然跨越多个转换完成，
 *   DRDY 塌到 ~310Hz。故 OSR 由 011(1024) 提到 110(8×)：
 *   · ODR 降为 500Hz（2ms/帧），读取周期有充足余量避开转换完成时刻；
 *   · 每样本过采样 8×，单样本噪声降 ~2.8 倍（√8），显示更稳。
 *   （若实测 ODR 需调整，改 OSR 档位即可：每 +1 档 ODR 减半。）
 * ============================================================ */
#define ADS_CLOCK_CH_EN_MASK  0x0300u /**< CH1_EN | CH0_EN */
#define ADS_CLOCK_OSR_SHIFT   2
#define ADS_CLOCK_OSR_8X      (0x06u << ADS_CLOCK_OSR_SHIFT) /**< OSR[2:0]=110（复位档 011 的 8 倍过采样） */
#define ADS_CLOCK_PWR_HR      (0x02u)                         /**< PWR[1:0]=10 HR */
#define ADS_CLOCK_VALUE       (ADS_CLOCK_CH_EN_MASK | ADS_CLOCK_OSR_8X | ADS_CLOCK_PWR_HR)
/** 配置后的输出数据率（Hz）：复位档 4000Hz ÷ 8 = 500Hz */
#define ADS_ODR_HZ            500u

/* ============================================================
 * 时序参数
 * ============================================================ */
#define ADS_RESET_LOW_MS    2   /**< RESET# 低电平保持（手册要求 ≥ 1ms） */
#define ADS_POWERUP_MS      50  /**< 复位释放后等待（上电 + 时钟起振） */
#define ADS_CMD_GAP_MS      2   /**< 命令之间留出的间隔 */
#define ADS_DRDY_TIMEOUT_MS 100 /**< 等 DRDY# 超时（采样周期 1ms，余量充足） */
#define ADS_MUTEX_TIMEOUT_MS 50 /**< 取总线互斥锁超时 */

/** 是否已初始化 */
static bool s_initialized = false;
/** SPI 设备句柄 */
static spi_device_handle_t s_spi = NULL;
/** DRDY# 下降沿信号量（ISR 给、任务取） */
static SemaphoreHandle_t s_drdy_sem = NULL;
/** SPI 事务互斥锁：测量核与诊断可能并发调用 ads131m02_read() */
static SemaphoreHandle_t s_bus_mutex = NULL;
/** DRDY# 边沿累计（诊断：区分 DRDY 真实速率与任务处理速率） */
static atomic_uint s_drdy_count;

/**
 * @brief 16bit 命令/寄存器值 -> 线上 24bit 字（MSB 对齐）
 */
#define ADS_W(value16) (((uint32_t)(value16)&0xFFFFu) << 8)

/**
 * @brief 把 24bit 码值按二进制补码符号扩展到 32bit
 * @param word24 24bit 原始码值
 * @return 符号扩展后的 32bit 有符号值
 */
static inline int32_t ads_sign_extend24(uint32_t word24)
{
    return (int32_t)((word24 & 0x800000u) ? (word24 | 0xFF000000u) : word24);
}

/**
 * @brief DRDY# 下降沿中断服务程序
 *
 * 只做一件事：唤醒等待采样的任务。
 * 约束（hardware_pinout.md §6）：ISR 及其调用的代码/数据必须位于 IRAM，
 * 且不得访问 PSRAM、不得忙等——故这里只调用 FreeRTOS 的信号量 API。
 */
static void IRAM_ATTR ads_drdy_isr(void *arg)
{
    (void)arg;
    atomic_fetch_add_explicit(&s_drdy_count, 1, memory_order_relaxed);
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_drdy_sem, &hp_task_woken);
    portYIELD_FROM_ISR(hp_task_woken);
}

/**
 * @brief 一次 SPI 事务内连续交换 N 个 24bit 字
 *
 * 全部字必须放在同一个事务里（本板无片选，帧边界只靠连续时钟 + 字数保证）。
 * tx24 / rx24 都是「24bit 原始值」域：命令用 ADS_W(cmd) 对齐，数据直接是码值。
 *
 * @param tx24 待发送的 24bit 字数组，不可为 NULL
 * @param rx24 输出：收到的 24bit 字数组，可为 NULL（只写不读时）
 * @param word_count 字数，1 ~ ADS_FRAME_WORDS（一帧 = 3 字）
 * @return ESP_OK 成功
 */
static esp_err_t ads_xfer_raw(const uint32_t *tx24, uint32_t *rx24, size_t word_count)
{
    if (tx24 == NULL || word_count == 0 || word_count > ADS_FRAME_WORDS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx_buf[ADS_FRAME_WORDS * ADS_WORD_BYTES] = { 0 };
    uint8_t rx_buf[ADS_FRAME_WORDS * ADS_WORD_BYTES] = { 0 };

    for (size_t i = 0; i < word_count; i++) {
        uint32_t v = tx24[i] & 0xFFFFFFu;
        tx_buf[i * ADS_WORD_BYTES + 0] = (uint8_t)(v >> 16);
        tx_buf[i * ADS_WORD_BYTES + 1] = (uint8_t)(v >> 8);
        tx_buf[i * ADS_WORD_BYTES + 2] = (uint8_t)v;
    }

    spi_transaction_t trans = {
        .length = word_count * ADS_WORD_BYTES * 8, /* 单位：位 */
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &trans);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI 事务失败: %s", esp_err_to_name(err));
        return err;
    }

    if (rx24 != NULL) {
        for (size_t i = 0; i < word_count; i++) {
            rx24[i] = ((uint32_t)rx_buf[i * ADS_WORD_BYTES + 0] << 16) |
                      ((uint32_t)rx_buf[i * ADS_WORD_BYTES + 1] << 8) |
                      (uint32_t)rx_buf[i * ADS_WORD_BYTES + 2];
        }
    }
    return ESP_OK;
}

/**
 * @brief 等一次 DRDY# 下降沿（= 一帧的起点）
 *
 * 先清掉可能残留的信号量，再阻塞等待，保证拿到的是"最新的一帧起点"。
 *
 * @return ESP_OK；ESP_ERR_TIMEOUT 超时（通常意味着 ADC 无时钟 / DRDY 未接）
 */
static esp_err_t ads_wait_drdy(void)
{
    (void)xSemaphoreTake(s_drdy_sem, 0); /* 清残留 */
    if (xSemaphoreTake(s_drdy_sem, pdMS_TO_TICKS(ADS_DRDY_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/**
 * @brief 读一个寄存器（DRDY 同步的「两帧」访问）
 *
 * ⚠ 关键：本器件的帧边界由 DRDY# 界定，主机必须「每个 DRDY 周期时钟恰好一帧」。
 *   事务结构：
 *     帧 N   ：[RREG 命令字][dummy][dummy]       ← 命令放在帧首字
 *     帧 N+1 ：[寄存器内容][dummy][dummy]        ← 响应出现在下一帧的首字
 *   因此分两次事务、各等一次 DRDY。
 *
 *   曾踩的坑：一次性时钟 2 帧（不等 DRDY）→ 帧边界漂移，读到的"响应"是转换数据，
 *   表现为读回值随每次上电变化、ID 读出 0x2818/0x333E 这类非法值。
 *   （TI 手册：the device outputs the register contents in the command
 *     response word of the following frame；TI E2E 亦明确"next frame"。）
 *
 * @param addr  寄存器地址
 * @param value 输出：寄存器值（16bit）
 * @return ESP_OK 成功；ESP_ERR_TIMEOUT 等 DRDY 超时；其他为 SPI 错误
 */
static esp_err_t ads_read_reg(uint8_t addr, uint16_t *value)
{
    uint32_t tx[ADS_FRAME_WORDS] = { 0 };
    uint32_t rx[ADS_FRAME_WORDS] = { 0 };

    /* 帧 N：发 RREG */
    esp_err_t err = ads_wait_drdy();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "等 DRDY 超时（RREG 0x%02X 命令帧）", addr);
        return err;
    }
    tx[0] = ADS_W(ADS_CMD_RREG(addr, 1));
    err = ads_xfer_raw(tx, rx, ADS_FRAME_WORDS);
    if (err != ESP_OK) {
        return err;
    }

    /* 帧 N+1：帧首字即寄存器内容 */
    err = ads_wait_drdy();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "等 DRDY 超时（RREG 0x%02X 响应帧）", addr);
        return err;
    }
    tx[0] = ADS_W(ADS_CMD_NULL);
    err = ads_xfer_raw(tx, rx, ADS_FRAME_WORDS);
    if (err != ESP_OK) {
        return err;
    }

    *value = (uint16_t)(rx[0] >> 8);

    /* 诊断：开 DEBUG 级日志可看到整帧原始字，便于核对 */
    ESP_LOGD(TAG, "RREG 0x%02X -> 0x%04X（响应帧: %06X %06X %06X）", addr, *value, (unsigned)rx[0],
             (unsigned)rx[1], (unsigned)rx[2]);
    return ESP_OK;
}

/**
 * @brief 写一个寄存器（DRDY 同步的「两帧」访问）
 *
 * 帧 N   ：[WREG 命令字][要写入的数据][dummy]   ← WREG 的数据字紧跟命令字
 * 帧 N+1 ：[WREG 应答字][dummy][dummy]
 * 应答字格式 010a aaaa ammm mmmm（mmm mmmm = 实际写入个数 - 1），
 * 借它确认写入真的生效（未解锁时 WREG 会被静默丢弃）。
 *
 * @param addr  寄存器地址
 * @param value 待写入的值（16bit）
 * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE 应答格式不符；其他为 DRDY/SPI 错误
 */
static esp_err_t ads_write_reg(uint8_t addr, uint16_t value)
{
    uint32_t tx[ADS_FRAME_WORDS] = { 0 };
    uint32_t rx[ADS_FRAME_WORDS] = { 0 };

    /* 帧 N：发 WREG + 数据 */
    esp_err_t err = ads_wait_drdy();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "等 DRDY 超时（WREG 0x%02X 命令帧）", addr);
        return err;
    }
    tx[0] = ADS_W(ADS_CMD_WREG(addr, 1));
    tx[1] = ADS_W(value);
    err = ads_xfer_raw(tx, rx, ADS_FRAME_WORDS);
    if (err != ESP_OK) {
        return err;
    }

    /* 帧 N+1：取应答字 */
    err = ads_wait_drdy();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "等 DRDY 超时（WREG 0x%02X 应答帧）", addr);
        return err;
    }
    tx[0] = ADS_W(ADS_CMD_NULL);
    err = ads_xfer_raw(tx, rx, ADS_FRAME_WORDS);
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t ack = (uint16_t)(rx[0] >> 8);
    /* 应答高 3 位应为 010b（0x4000 段），低 7 位为「写入个数 - 1」= 0 */
    if ((ack & 0xE000u) != 0x4000u || (ack & 0x7Fu) != 0u) {
        ESP_LOGE(TAG, "WREG 0x%02X=0x%04X 应答异常: 0x%04X（应答帧: %06X %06X %06X）", addr, value,
                 ack, (unsigned)rx[0], (unsigned)rx[1], (unsigned)rx[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "WREG 0x%02X=0x%04X 已确认（应答 0x%04X）", addr, value, ack);
    return ESP_OK;
}

/**
 * @brief 发送一条无数据命令，并可选取回它在下一帧的响应字
 *
 * @param cmd      16bit 命令码
 * @param response 输出：下一帧首字（高 16bit 对齐）；为 NULL 则不多读一帧
 * @return ESP_OK 成功；其他为 DRDY/SPI 错误
 */
static esp_err_t ads_send_cmd(uint16_t cmd, uint16_t *response)
{
    uint32_t tx[ADS_FRAME_WORDS] = { 0 };
    uint32_t rx[ADS_FRAME_WORDS] = { 0 };

    /* 帧 N：发命令 */
    esp_err_t err = ads_wait_drdy();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "等 DRDY 超时（命令 0x%04X 帧）", cmd);
        return err;
    }
    tx[0] = ADS_W(cmd);
    err = ads_xfer_raw(tx, rx, ADS_FRAME_WORDS);
    if (err != ESP_OK) {
        return err;
    }

    if (response == NULL) {
        return ESP_OK;
    }

    /* 帧 N+1：响应帧 */
    err = ads_wait_drdy();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "等 DRDY 超时（命令 0x%04X 响应帧）", cmd);
        return err;
    }
    tx[0] = ADS_W(ADS_CMD_NULL);
    err = ads_xfer_raw(tx, rx, ADS_FRAME_WORDS);
    if (err != ESP_OK) {
        return err;
    }
    *response = (uint16_t)(rx[0] >> 8);
    return ESP_OK;
}

esp_err_t ads131m02_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    bool isr_handler_added = false;

    /* ---- ① 同步对象：DRDY 信号量 + SPI 事务互斥锁 ---- */
    s_drdy_sem = xSemaphoreCreateBinary();
    s_bus_mutex = xSemaphoreCreateMutex();
    if (s_drdy_sem == NULL || s_bus_mutex == NULL) {
        ESP_LOGE(TAG, "创建同步对象失败（内存不足）");
        err = ESP_ERR_NO_MEM;
        goto fail_sync;
    }

    /* ---- ② RESET#(SYNC#) 脚：输出，先保持高（不复位） ---- */
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_ADC_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&rst_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RESET# 脚 IO%d 配置失败: %s", BOARD_PIN_ADC_RST, esp_err_to_name(err));
        goto fail_sync;
    }
    gpio_set_level(BOARD_PIN_ADC_RST, 1);

    /* ---- ③ CLKIN 脚：高阻输入（时钟默认由板载晶振供给，见文件头调试要点） ---- */
    const gpio_config_t clk_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_ADC_CLK,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "时钟脚 IO%d 配置失败: %s", BOARD_PIN_ADC_CLK, esp_err_to_name(err));
        goto fail_sync;
    }

    /* ---- ④ 建 SPI 总线与设备（数据量极小，无需 DMA） ---- */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_PIN_ADC_MOSI,
        .miso_io_num = BOARD_PIN_ADC_MISO,
        .sclk_io_num = BOARD_PIN_ADC_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ADS_FRAME_WORDS * ADS_WORD_BYTES,
    };
    err = spi_bus_initialize(ADS_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 总线初始化失败: %s", ADS_SPI_HOST_NAME, esp_err_to_name(err));
        goto fail_sync;
    }

    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = ADS_SPI_CLK_HZ,
        .mode = ADS_SPI_MODE,
        /* 本板无片选，器件常选中；帧边界靠整帧字数保证（见文件头） */
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_add_device(ADS_SPI_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "添加 SPI 设备失败: %s", esp_err_to_name(err));
        goto fail_bus;
    }

    /* ---- ⑤ DRDY# 下降沿中断 ----
     * 必须**早于**任何寄存器访问安装：本器件的帧由 DRDY 界定，
     * 寄存器访问要和 ads131m02_read() 一样「等 DRDY 再时钟一帧」，
     * 否则帧边界漂移、读到的"响应"其实是转换数据（详见 ads_read_reg 注释）。 */
    const gpio_config_t drdy_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_ADC_DRDY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* DRDY# 低有效，上拉兜底防悬空 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&drdy_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DRDY# 脚 IO%d 配置失败: %s", BOARD_PIN_ADC_DRDY, esp_err_to_name(err));
        goto fail_dev;
    }

    /* ISR 服务可能已被其他驱动安装过，此时返回 ESP_ERR_INVALID_STATE 属正常 */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "安装 GPIO ISR 服务失败: %s", esp_err_to_name(err));
        goto fail_dev;
    }
    err = gpio_isr_handler_add(BOARD_PIN_ADC_DRDY, ads_drdy_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 DRDY# 中断处理失败: %s", esp_err_to_name(err));
        goto fail_dev;
    }
    isr_handler_added = true;

    /* ---- ⑥ 硬复位：RESET# 低脉冲，随后等待上电与时钟起振 ---- */
    gpio_set_level(BOARD_PIN_ADC_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(ADS_RESET_LOW_MS));
    gpio_set_level(BOARD_PIN_ADC_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(ADS_POWERUP_MS));

    /* ---- ⑦ 解锁寄存器写保护（WREG 前必须，否则写入被静默丢弃） ----
     * 不在此处额外发软件 RESET 命令的原因（依据手册 §8.4.1.3 / §8.5.1.11）：
     *   软件 RESET(0011h) 只在“整帧时钟完整”时才被锁存，帧不完整时器件只会
     *   回显 0011h 且不复位；而本工程 CRC 关闭、每帧固定 3 字，早期实测也确实
     *   只收到回显。第⑥步的 SYNC/RESET# 引脚硬复位（低电平 2ms ≥ 手册要求的
     *   2048·tCLKIN≈250µs，随后等待 50ms ≫ tPOR/tREGACQ）已经完整完成复位，
     *   再发一个无法保证被锁存的软件 RESET 反而产生误导，故省略。 */
    err = ads_send_cmd(ADS_CMD_UNLOCK, NULL);
    if (err != ESP_OK) {
        goto fail_dev;
    }
    vTaskDelay(pdMS_TO_TICKS(ADS_CMD_GAP_MS));

    /* ---- ⑨ 写 GAIN1：CH0 = 16x（电流）/ CH1 = 1x（电压） ----
     * ads_write_reg() 会校验 WREG 应答字，写入未生效（如未解锁）会明确报错。 */
    err = ads_write_reg(ADS_REG_GAIN1, ADS_GAIN1_VALUE);
    if (err != ESP_OK) {
        /* 不在此处返回失败的理由见文件头「上板调试要点」：ads131m02 是启动表里的
         * 关键模块，一旦返回失败会把系统推到 FAULT。故只「响亮地报错」，
         * 最终判定交给运行期 read() 超时（由 power_measure 上报 SYSTEM_FAULT_ADC）。 */
        ESP_LOGE(TAG, "GAIN1 写入未确认: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(ADS_CMD_GAP_MS));

    /* ---- ⑩ 读回校验 GAIN1 ---- */
    uint16_t gain1_readback = 0;
    err = ads_read_reg(ADS_REG_GAIN1, &gain1_readback);
    if (err != ESP_OK) {
        goto fail_dev;
    }
    if (gain1_readback == ADS_GAIN1_VALUE) {
        ESP_LOGI(TAG, "GAIN1 写入并回读一致（0x%04X），寄存器访问正常", gain1_readback);
    } else {
        ESP_LOGE(TAG, "GAIN1 回读不一致（回读=0x%04X 写入=0x%04X）：寄存器访问可能有问题，"
                      "请按文件头调试要点排查",
                 gain1_readback, ADS_GAIN1_VALUE);
    }
    vTaskDelay(pdMS_TO_TICKS(ADS_CMD_GAP_MS));

    /* ---- ⑩.1 写 CLOCK：HR 模式 + OSR=8x 档 -> 输出数据率 500SPS ----
     * 与芯片复位默认（OSR=1024 档 4kSPS）不同：提高过采样降噪声，
     * 并给主机读取留出避开"转换完成时刻"的窗口（SBAS839 §8.5.1.5，
     * 详见 ADS_CLOCK_VALUE 注释）。仍显式写入并回读，保证采样率确定。 */
    err = ads_write_reg(ADS_REG_CLOCK, ADS_CLOCK_VALUE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CLOCK(采样率) 写入未确认: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(ADS_CMD_GAP_MS));

    uint16_t clock_readback = 0;
    err = ads_read_reg(ADS_REG_CLOCK, &clock_readback);
    if (err == ESP_OK) {
        if (clock_readback == ADS_CLOCK_VALUE) {
            ESP_LOGI(TAG, "CLOCK 回读 0x%04X：HR 模式 OSR=8x 档，ODR=%u SPS", clock_readback,
                     ADS_ODR_HZ);
        } else {
            ESP_LOGW(TAG, "CLOCK 回读 0x%04X 与目标 0x%04X 不符（实际 ODR 可能不是 %u SPS）",
                     clock_readback, ADS_CLOCK_VALUE, ADS_ODR_HZ);
        }
    }

    /* ---- ⑪ 通信判定：读器件 ID ----
     * 手册 ID 寄存器（00h）复位值标注为 22xxh：高字节（含 CHANCNT）固定为 0x22，
     * 低 8 位为保留位且“可能变化（subject to change）”。因此只校验高字节 0x22，
     * 不把低 8 位写死（早期代码精确比对 0x2205，换硅片版本会误报警告）。 */
    uint16_t dev_id = 0;
    err = ads_read_reg(ADS_REG_ID, &dev_id);
    if (err != ESP_OK) {
        goto fail_dev;
    }
    if ((dev_id & 0xFF00u) == (ADS_DEV_ID_TYPICAL & 0xFF00u)) {
        ESP_LOGI(TAG, "通信验证通过（器件 ID=0x%04X，高字节与手册 0x22 一致）", dev_id);
    } else {
        ESP_LOGW(TAG, "器件 ID 回读 0x%04X，高字节应为 0x22（手册复位值 22xxh），"
                      "排查见文件头调试要点", dev_id);
    }

    /* ---- ⑫ 寄存器体检：把决定"读数怎么解释"的位逐一打印出来 ----
     * 这一步是"核查驱动与硬件是否一致"的依据（对应手册 §8.6）：
     *   MODE(02h)   ：WLENGTH（字长，必须 24bit）、CRC 是否启用（启用则一帧变 4 字）、
     *                 DRDY_SEL / DRDY_FMT（DRDY 行为）；
     *   STATUS(01h) ：WLENGTH 回读、RESET/F_RESYNC（是否发生过复位/重同步）、
     *                 DRDY0/DRDY1（哪些通道有新数据）；
     *   CH0/CH1_CFG ：MUX（必须 00=外部 AIN 输入）、DCBLK（默认关闭）、PHASE；
     *   THRSHLD_LSB ：DCBLOCK[3:0]（0=直流阻断滤波器关闭，本工程读直流必须为 0）。
     * 任何一项与预期不符，都会直接导致"读数不对"，因此开机就查。 */
    {
        uint16_t mode = 0, status = 0, ch0cfg = 0, ch1cfg = 0, thr = 0, gain = 0;
        bool ok = true;
        ok &= (ads_read_reg(0x02u, &mode) == ESP_OK);
        ok &= (ads_read_reg(0x01u, &status) == ESP_OK);
        ok &= (ads_read_reg(0x09u, &ch0cfg) == ESP_OK);
        ok &= (ads_read_reg(0x0Eu, &ch1cfg) == ESP_OK); /* 0x0E = CH1_CFG（0x0A 是 CH0_OCAL_MSB） */
        ok &= (ads_read_reg(0x08u, &thr) == ESP_OK);
        ok &= (ads_read_reg(ADS_REG_GAIN1, &gain) == ESP_OK);
        if (ok) {
            const uint16_t wlen_mode = (uint16_t)((mode >> 8) & 0x03u);
            const uint16_t wlen_stat = (uint16_t)((status >> 8) & 0x03u);
            uint16_t dcblock = (uint16_t)(thr & 0x0Fu);
            uint16_t mux0 = (uint16_t)(ch0cfg & 0x03u);
            uint16_t mux1 = (uint16_t)(ch1cfg & 0x03u);
            /* GAIN1: PGAGAIN1[2:0] = bit6:4（CH1=电压）, PGAGAIN0[2:0] = bit2:0（CH0=电流）
             * 线性编码：000b=1x 001b=2x 010b=4x 011b=8x 100b=16x 101b=32x 110b=64x 111b=128x */
            static const uint16_t gain_lut[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
            const uint16_t g0 = (uint16_t)(gain & 0x07u);          /* CH0 = 电流 */
            const uint16_t g1 = (uint16_t)((gain >> 4) & 0x07u);   /* CH1 = 电压 */
            /* MODE bit13 = REG_CRC_EN，bit12 = RX_CRC_EN；bit4 = TIMEOUT */
            const bool crc_on = ((mode >> 12) & 0x03u) != 0u;
            ESP_LOGI(TAG, "寄存器体检: MODE=0x%04X(字长=%s CRC=%s 超时保护=%s) "
                          "STATUS=0x%04X(字长=%s DRDY=%u%u) "
                          "GAIN1=0x%04X(CH0电流=%ux CH1电压=%ux) "
                          "CH0_CFG=0x%04X(MUX0=%u) CH1_CFG=0x%04X(MUX1=%u) DCBLOCK=%u",
                     mode, (wlen_mode == 1) ? "24bit" : "非24bit(异常)",
                     crc_on ? "开(一帧4字!)" : "关",
                     ((mode >> 4) & 0x01u) ? "开" : "关",
                     status, (wlen_stat == 1) ? "24bit" : "异常",
                     (unsigned)((status >> 1) & 1u), (unsigned)(status & 1u),
                     gain, gain_lut[g0], gain_lut[g1],
                     ch0cfg, mux0, ch1cfg, mux1, dcblock);
            if (g0 != 4u) {
                ESP_LOGE(TAG, "CH0(电流通道) PGA 增益 = %ux，应为 16x（PGAGAIN0=100b）", gain_lut[g0]);
            }
            if (g1 != 0u) {
                ESP_LOGE(TAG, "CH1(电压通道) PGA 增益 = %ux，应为 1x（PGAGAIN1=000b）", gain_lut[g1]);
            }
            if (wlen_mode != 1 || wlen_stat != 1) {
                ESP_LOGE(TAG, "字长不是 24bit：一帧字数与驱动假设不符，采样数据会错位，"
                              "请把 WLENGTH 改为 01b");
            }
            if (mux0 != 0u) {
                ESP_LOGE(TAG, "CH0 MUX=%u 不是外部 AIN0 输入（应为 00b）", mux0);
            }
            if (mux1 != 0u) {
                ESP_LOGE(TAG, "CH1 MUX=%u 不是外部 AIN1 输入（应为 00b）", mux1);
            }
            if (dcblock != 0u) {
                ESP_LOGW(TAG, "DCBLOCK=%u 非 0：直流阻断滤波器已启用，直流电压/电流会被滤掉，"
                              "本工程读直流必须为 0", dcblock);
            }
            if (crc_on) {
                ESP_LOGE(TAG, "SPI CRC 已启用：一帧变为 STATUS+CH0+CH1+CRC 四个字，"
                              "驱动需同步改造，否则数据错位");
            }
        } else {
            ESP_LOGW(TAG, "寄存器体检失败（读寄存器出错），跳过");
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成（%s mode1 %d Hz，CH0=16x 电流 / CH1=1x 电压，ODR=%u SPS，DRDY#=IO%d）",
             ADS_SPI_HOST_NAME, ADS_SPI_CLK_HZ, ADS_ODR_HZ, BOARD_PIN_ADC_DRDY);
    return ESP_OK;
fail_dev:
    if (isr_handler_added) {
        gpio_isr_handler_remove(BOARD_PIN_ADC_DRDY);
    }
    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
fail_bus:
    spi_bus_free(ADS_SPI_HOST);
fail_sync:
    if (s_drdy_sem != NULL) {
        vSemaphoreDelete(s_drdy_sem);
        s_drdy_sem = NULL;
    }
    if (s_bus_mutex != NULL) {
        vSemaphoreDelete(s_bus_mutex);
        s_bus_mutex = NULL;
    }
    return err;
}

esp_err_t ads131m02_read(ads131m02_sample_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 串行化整段「等 DRDY + 读一帧」，避免并发调用把帧交错打乱 */
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(ADS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* 等 DRDY# 下降沿：代表一轮新的同步采样已就绪 */
    if (xSemaphoreTake(s_drdy_sem, pdMS_TO_TICKS(ADS_DRDY_TIMEOUT_MS)) != pdTRUE) {
        xSemaphoreGive(s_bus_mutex);
        return ESP_ERR_TIMEOUT;
    }

    /* 交换 3 个 NULL 字，一口气取回 [STATUS][CH0][CH1]（无 CS，必须同事务） */
    const uint32_t tx[ADS_FRAME_WORDS] = { ADS_W(ADS_CMD_NULL), ADS_W(ADS_CMD_NULL),
                                           ADS_W(ADS_CMD_NULL) };
    uint32_t rx[ADS_FRAME_WORDS] = { 0 };
    esp_err_t err = ads_xfer_raw(tx, rx, ADS_FRAME_WORDS);

    if (err == ESP_OK) {
        /* rx[0] 是 STATUS，rx[1]/rx[2] 为 24bit 补码。
         * ⚠ 通道接法与直觉相反（见原理图 V2）：
         *   rx[1] = CH0 = AIN0 = ISENSE± = **电流**
         *   rx[2] = CH1 = AIN1 = 分压/跟随器 = **电压** */
        out->status = (uint32_t)(rx[0] >> 8);
        out->current_raw = ads_sign_extend24(rx[1]); /* CH0 = AIN0 = 电流 */
        out->voltage_raw = ads_sign_extend24(rx[2]); /* CH1 = AIN1 = 电压 */
    }

    xSemaphoreGive(s_bus_mutex);
    return err;
}

uint32_t ads131m02_get_sample_rate_hz(void)
{
    return s_initialized ? ADS_ODR_HZ : 0;
}

/** 自检连续采样帧数：全部为 0 才判定"数据无意义"，单帧为 0 属正常 */
#define ADS_SELFTEST_FRAMES 8

esp_err_t ads131m02_selftest(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- ① 芯片存在性锚点：ID 寄存器（00h）高字节必须是 0x22 ----
     * 手册复位值 22xxh（高字节含 CHANCNT 固定，低 8 位可变）。
     * 芯片没焊时 MISO 悬空（ESP32 内部弱上下拉下读 0 或随机），
     * ID 读回 0x0000/乱码 → 此处判失败。
     * ⚠ init() 里 ID 不符只告警不失败（兼容硅片版本差异），
     *   但启动自检必须把它当硬失败——这正是"自检通过但测量全 0"的盲区。 */
    uint16_t dev_id = 0;
    esp_err_t err = ads_read_reg(ADS_REG_ID, &dev_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "自检：读 ID 寄存器失败: %s（SPI 通路断或无 DRDY）", esp_err_to_name(err));
        return err;
    }
    if ((dev_id & 0xFF00u) != (ADS_DEV_ID_TYPICAL & 0xFF00u)) {
        ESP_LOGE(TAG, "自检失败：ID=0x%04X 高字节不是 0x22——ADS131M02 缺失/虚焊或 SPI 通路异常",
                 dev_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ---- ② 寄存器通路可信：GAIN1 回读必须与初始化写入一致 ----
     * init() 写 GAIN1 时若未解锁会被静默丢弃（那里只告警），这里再验一道。 */
    uint16_t gain1 = 0;
    err = ads_read_reg(ADS_REG_GAIN1, &gain1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "自检：读 GAIN1 失败: %s", esp_err_to_name(err));
        return err;
    }
    if (gain1 != ADS_GAIN1_VALUE) {
        ESP_LOGE(TAG, "自检失败：GAIN1 回读 0x%04X ≠ 配置 0x%04X（寄存器写入未生效）", gain1,
                 ADS_GAIN1_VALUE);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ---- ③ 数据可信度：连续多帧码值不能全 0 ----
     * 芯片在但测量前端未接入（分压/分流器虚焊）时码值恒 0：
     * read() 不报错、旧自检照样通过，但测量数据全是 0——正是待排查现象。
     * 本设备由被测电源供电，正常运行时电压通道必有码值，全 0 即异常。 */
    bool any_nonzero = false;
    for (int i = 0; i < ADS_SELFTEST_FRAMES; i++) {
        ads131m02_sample_t s;
        err = ads131m02_read(&s);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "自检：第 %d/%d 帧读取失败: %s", i + 1, ADS_SELFTEST_FRAMES,
                     esp_err_to_name(err));
            return err;
        }
        if (s.voltage_raw != 0 || s.current_raw != 0) {
            any_nonzero = true;
            break;
        }
    }
    if (!any_nonzero) {
        ESP_LOGE(TAG, "自检失败：连续 %d 帧电压/电流码值全 0——芯片存在(ID=0x%04X)但测量数据无效，"
                      "排查前端分压 R33/R39、分流器 ISENSE± 是否接入",
                 ADS_SELFTEST_FRAMES, dev_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "自检通过（ID=0x%04X，GAIN1=0x%04X，采样数据非全 0）", dev_id, gain1);
    return ESP_OK;
}

uint32_t ads131m02_drdy_count(void)
{
    return atomic_load_explicit(&s_drdy_count, memory_order_relaxed);
}
