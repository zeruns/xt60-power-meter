/**
 * @file ads131m02_engine.c
 * @brief ADS131M02 数据面引擎（M2）实现：DRDY ISR 直启 GDMA 的最小 SPI3 帧引擎
 *
 * 一帧的完整通路（20MHz、96bit ≈ 4.8µs 总线时间）：
 *
 *   DRDY# 下降沿
 *     └─ DRDY ISR（IRAM）
 *          ① 记时间戳 + 唤醒消费者
 *          ② 若流式开启且槽环有余量 → kick_frame()：
 *             TX/RX 描述符重臂（内存写）→ gdma_ll 启动 → spi_ll_user_start
 *          ③ 否则丢帧计数（绝不覆盖未消费槽）
 *
 *   ~5µs 后 DMA 收满 12 字节 → 槽内数据就绪
 *   消费者 read_frame()：取信号量 → 忙等 SPI 完成位 → 从槽环取帧
 *
 * 关键实现取舍：
 *   · **不用 spi_master 驱动**：它的事务启动是任务上下文语义（详见头文件）。
 *     本引擎用 spi_hal/LL 装载一次性帧格式，把「启动」压成 ISR 内的寄存器写。
 *   · **不用 GDMA 完成中断做消费同步**：GDMA 驱动 ISR 在 flash 里，cache
 *     关闭窗口（NVS 擦写）会被挂起；改为消费者忙等 spi_ll_usr_is_done
 *     （inline、帧仅 5µs）。维护路径同样不用完成中断：DRDY ISR 直启维护帧后
 *     给一个「已启动」信号量，任务被唤醒后仍靠忙等确认传输真正结束。
 *   · **描述符每次 kick 重臂**（单描述符复用）：比链式环简单，且 owner 位
 *     检查天然防止未就绪启动。
 *
 * IRAM 安全：drdy_isr 与 engine_kick 只含内存写 + inline LL 调用；
 * 任务上下文初始化才用 HAL/驱动 API。
 */
#include "ads131m02_engine.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_private/gpio.h"
#include "esp_private/gdma.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/spi_share_hw_ctrl.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/dma_types.h"
#include "hal/gdma_ll.h"
#include "hal/spi_hal.h"
#include "hal/spi_ll.h"
#include "soc/gpio_periph.h"
#include "soc/spi_periph.h"

static const char *TAG = "ads_engine";

/** 帧长度：4 字 × 3 字节 = 12 字节（96 bit），整帧 = 槽 = 描述符长度 */
#define ENG_FRAME_BYTES (ADS_ENGINE_FRAME_WORDS * 3)
/** 槽环容量（kicked 与 read 的最大在途距离；4kHz 下 = 2ms 缓冲） */
#define ENG_RING 8

/* ============================================================
 * 硬件句柄与一次性配置（任务上下文写，之后只读）
 * ============================================================ */
static spi_hal_context_t s_hal;      /**< SPI HAL 上下文（含 hw 寄存器基址） */
static spi_hal_dev_config_t s_hal_dev; /**< SPI 设备配置（mode/时序） */
static spi_dev_t *s_hw = NULL;       /**< SPI3 寄存器基址（ISR 内联调用用） */
static gdma_channel_handle_t s_tx_chan = NULL; /**< GDMA TX 通道（驱动句柄，仅初始化用） */
static gdma_channel_handle_t s_rx_chan = NULL; /**< GDMA RX 通道 */
static gdma_dev_t *s_gdma = NULL;    /**< GDMA 寄存器基址（LL 调用用） */
static uint32_t s_tx_ll_chan = 0;    /**< GDMA TX 逻辑通道号（LL 调用用） */
static uint32_t s_rx_ll_chan = 0;    /**< GDMA RX 逻辑通道号 */

/* ============================================================
 * DMA 描述符与缓冲（DRAM + 对齐；描述符每次 kick 重臂）
 * ============================================================ */
/** TX 帧缓冲：流式时全 0（NULL 帧），维护时写入命令字（同一批描述符复用） */
static DRAM_ATTR uint8_t s_tx_buf[ENG_FRAME_BYTES] __attribute__((aligned(4)));
/** RX 槽环：每帧落一槽，kick 写、消费读，序号单调 */
static DRAM_ATTR uint8_t s_slot[ENG_RING][ENG_FRAME_BYTES] __attribute__((aligned(4)));
/** TX/RX 描述符（12 字节结构，DMA 需读，必须在 DMA 可达内存） */
static DRAM_ATTR dma_descriptor_t s_tx_desc __attribute__((aligned(4)));
static DRAM_ATTR dma_descriptor_t s_rx_desc __attribute__((aligned(4)));

/* ============================================================
 * 流式状态（ISR 与消费者共享；原子/单调序号）
 * ============================================================ */
static atomic_bool s_stream_on = false;  /**< 流式 kick 开关（维护模式关） */
static atomic_uint s_kick_seq;           /**< 已 kick 帧序号（ISR 写） */
static atomic_uint s_read_seq;           /**< 已消费帧序号（消费者写） */
static atomic_uint s_drdy_count;         /**< DRDY 边沿计数 */
static atomic_uint s_kick_count;         /**< kick 总数（含维护） */
static atomic_uint s_drop_count;         /**< 槽满丢帧数 */

/** DRDY 同步信号量（ISR 给、任务取；流式与维护共用「帧起点」语义） */
static SemaphoreHandle_t s_drdy_sem = NULL;

/* ---- 维护帧直启（M3 修复）----
 * 为什么维护帧也要在 DRDY ISR 里 kick：
 *   「任务取信号量 → 再 kick」在 DRDY 边沿到 kick 之间插入了**不确定的任务
 *   调度延迟**。4kHz 档帧周期仅 250µs，延迟抖动会让命令帧落到错误的帧窗，
 *   芯片认不出 RREG/WREG 命令（实测寄存器读回全是 STATUS=0x0503、命令回显
 *   0x0655 串到错误帧）。流式帧之所以正常，正因它在 ISR 内直启、确定性对齐。
 *   故维护帧同样改为「任务置请求 → DRDY ISR 直启 → ISR 通知完成」，
 *   使每次 xfer 精确落在一个 DRDY 边沿、精确推进一帧。 */
static const uint32_t *volatile s_maint_tx = NULL; /**< 维护帧待发 4 字（非 NULL=请求 ISR kick） */
static SemaphoreHandle_t s_maint_done_sem = NULL;  /**< 维护帧 kick 完成（ISR give、任务 take） */

/* ---- δ 统计（DRDY 边沿 → SPI 启动；ISR 写、任务只读快照）---- */
static volatile uint32_t s_lat_min_us = UINT32_MAX;
static volatile uint32_t s_lat_max_us = 0;
static volatile uint64_t s_lat_sum_us = 0;
static volatile uint32_t s_lat_n = 0;

/**
 * @brief 24bit 字数组 → 线上字节（MSB 先出）
 *
 * IRAM_ATTR：被 IRAM 的 engine_kick 调用（DRDY ISR 路径），cache 关闭
 * 窗口内必须可执行——不能依赖编译器把它内联进 engine_kick。
 */
static void IRAM_ATTR pack_words(const uint32_t *words, uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const uint32_t v = (words != NULL) ? (words[i] & 0xFFFFFFu) : 0u;
        buf[i * 3 + 0] = (uint8_t)(v >> 16);
        buf[i * 3 + 1] = (uint8_t)(v >> 8);
        buf[i * 3 + 2] = (uint8_t)v;
    }
}

/**
 * @brief 线上字节 → 24bit 字数组（pack_words 的逆）
 */
static void unpack_words(const uint8_t *buf, uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        words[i] = ((uint32_t)buf[i * 3 + 0] << 16) | ((uint32_t)buf[i * 3 + 1] << 8) |
                   (uint32_t)buf[i * 3 + 2];
    }
}

/**
 * @brief 启动一帧 96bit 全双工传输（IRAM 安全，ISR 与维护路径共用）
 *
 * 顺序参照 spi_master 的 s_spi_dma_prepare_data：先臂 DMA、后启动 SPI。
 * 描述符每次重臂（owner=1），DMA 搬完自动置 0，天然防重入。
 *
 * @param tx_words 待发送 4 字（NULL = 全 0 的 NULL 帧）
 * @param rx_buf   接收缓冲（≥12 字节，DMA 对齐）
 */
static void IRAM_ATTR engine_kick(const uint32_t *tx_words, uint8_t *rx_buf)
{
    pack_words(tx_words, s_tx_buf, ADS_ENGINE_FRAME_WORDS);

    /* TX 描述符：固定指向 s_tx_buf（内容已按需填充） */
    s_tx_desc.dw0.size = ENG_FRAME_BYTES;
    s_tx_desc.dw0.length = ENG_FRAME_BYTES;
    s_tx_desc.dw0.suc_eof = 1;
    s_tx_desc.dw0.owner = 1;
    s_tx_desc.buffer = s_tx_buf;
    s_tx_desc.next = NULL;

    /* RX 描述符：指向本帧的槽 */
    s_rx_desc.dw0.size = ENG_FRAME_BYTES;
    s_rx_desc.dw0.length = ENG_FRAME_BYTES;
    s_rx_desc.dw0.suc_eof = 1;
    s_rx_desc.dw0.owner = 1;
    s_rx_desc.buffer = rx_buf;
    s_rx_desc.next = NULL;

    /* 启动序列严格对齐 spi_master 的 s_spi_dma_prepare_data + spi_new_trans：
     *   RX: gdma_reset → FIFO 复位 + infifo_full_clr + dma_rx_enable → gdma_start
     *   TX: gdma_reset → FIFO 复位 + outfifo_empty_clr + dma_tx_enable → gdma_start
     *   最后 spi_ll_apply_config（推送影子寄存器，缺它 bitlen 不生效）→ user_start
     * ⚠ FIFO 复位不可省（2026-10 实测：只 enable 不复位，DMA 握手被残留
     *   full/empty 状态卡死，SPI 永不出时钟，wait_spi_done 超时）。 */
    gdma_ll_rx_reset_channel(s_gdma, s_rx_ll_chan);
    spi_hal_hw_prepare_rx(s_hw);
    gdma_ll_rx_set_desc_addr(s_gdma, s_rx_ll_chan, (uint32_t)&s_rx_desc);
    gdma_ll_rx_start(s_gdma, s_rx_ll_chan);
    gdma_ll_tx_reset_channel(s_gdma, s_tx_ll_chan);
    spi_hal_hw_prepare_tx(s_hw);
    gdma_ll_tx_set_desc_addr(s_gdma, s_tx_ll_chan, (uint32_t)&s_tx_desc);
    gdma_ll_tx_start(s_gdma, s_tx_ll_chan);
    spi_ll_apply_config(s_hw);
    spi_ll_user_start(s_hw);

    atomic_fetch_add_explicit(&s_kick_count, 1, memory_order_relaxed);
}

/**
 * @brief DRDY# 下降沿 ISR：唤醒消费者 + 流式直启（M2 核心）
 *
 * 全部操作 IRAM 安全（内存写 + inline LL + IRAM 函数），NVS 擦写的
 * cache 关闭窗口里照常工作；此时 flash 代码被挂起，消费者暂停，
 * 槽满后自动丢帧计数，恢复后按序号继续，不产生错位。
 */
static void IRAM_ATTR drdy_isr(void *arg)
{
    (void)arg;
    const uint32_t t0 = (uint32_t)esp_timer_get_time(); /* IRAM 安全（CONFIG_ESP_TIMER_IN_IRAM） */
    atomic_fetch_add_explicit(&s_drdy_count, 1, memory_order_relaxed);

    BaseType_t woken = pdFALSE;
    /* 二值信号量：ISR 里 give、任务里 take，用来把「DRDY 到了」这个事件
     * 从中断上下文传递给采样任务；give 后若唤醒了更高优先级任务，
     * woken 会被置 pdTRUE，ISR 末尾据此决定是否立即切任务。 */
    xSemaphoreGiveFromISR(s_drdy_sem, &woken);

    /* 维护帧直启（M3）：任务已置 s_maint_tx 请求，在 DRDY 边沿精确 kick 一帧。
     * 维护与流式互斥（维护前 set_stream(false)），故这里是 else-if 关系。 */
    const uint32_t *mtx = s_maint_tx;
    if (mtx != NULL) {
        s_maint_tx = NULL;
        engine_kick(mtx, s_slot[0]); /* 维护帧固定借用槽 0（流式已停，无并发） */
        BaseType_t mw = pdFALSE;
        xSemaphoreGiveFromISR(s_maint_done_sem, &mw);
        woken = (woken == pdTRUE) || (mw == pdTRUE);
    } else if (atomic_load_explicit(&s_stream_on, memory_order_relaxed)) {
        const uint32_t kick = atomic_load_explicit(&s_kick_seq, memory_order_relaxed);
        const uint32_t read = atomic_load_explicit(&s_read_seq, memory_order_relaxed);
        if (kick - read < ENG_RING) {
            engine_kick(NULL, s_slot[kick % ENG_RING]);
            atomic_store_explicit(&s_kick_seq, kick + 1, memory_order_relaxed);

            /* δ = DRDY 边沿 → SPI 启动（本引擎的核心验收指标） */
            const uint32_t lat = (uint32_t)esp_timer_get_time() - t0;
            if (lat < s_lat_min_us) {
                s_lat_min_us = lat;
            }
            if (lat > s_lat_max_us) {
                s_lat_max_us = lat;
            }
            s_lat_sum_us += lat;
            s_lat_n++;
        } else {
            atomic_fetch_add_explicit(&s_drop_count, 1, memory_order_relaxed);
        }
    }
    /* 中断退出前按需触发一次任务切换（woken 为 pdTRUE 说明刚才唤醒的任务
     * 优先级高于被中断的任务），保证采样任务尽快取走这一帧。 */
    portYIELD_FROM_ISR(woken);
}

/**
 * @brief 忙等当前 SPI 事务完成（帧仅 ~5µs；带超时封顶防硬件挂死）
 * @param timeout_us 超时（µs）
 * @return true=完成；false=超时（SPI 未启动/异常）
 */
static bool IRAM_ATTR wait_spi_done(uint32_t timeout_us)
{
    const uint32_t t0 = (uint32_t)esp_timer_get_time();
    while (!spi_ll_usr_is_done(s_hw)) {
        if ((uint32_t)esp_timer_get_time() - t0 > timeout_us) {
            return false;
        }
    }
    return true;
}

/* ============================================================
 * 公开 API
 * ============================================================ */

esp_err_t ads_engine_init(int mosi, int miso, int sclk, int clk_hz, int input_delay_ns)
{
    if (s_hw != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- ① SPI3 外设上电 + 引脚矩阵接线（无 CS：本板 CS 常选中）----
     * ⚠ 必须走 spicommon_periph_claim（2026-10 实测根因）：它做三件事——
     *   占用登记 + spi_ll_enable_bus_clock（PCR 寄存器接口总线时钟，缺它
     *   SPI3 所有寄存器写入被静默丢弃，读回恒 0）+ spi_ll_reset_register
     *   （释放 PCR 复位）。只开外设核时钟（clk_gate.clk_en）远远不够。 */
    if (!spicommon_periph_claim(SPI3_HOST, "ads_engine")) {
        return ESP_ERR_INVALID_STATE;
    }
    PERIPH_RCC_ATOMIC() {
        spi_ll_enable_clock(SPI3_HOST, true); /* 外设核时钟（claim 不含此项） */
    }
    const int sig_clk = spi_periph_signal[2].spiclk_in; /* S3 的 IN/OUT 信号号相同 */
    const int sig_mosi = spi_periph_signal[2].spid_in;
    const int sig_miso = spi_periph_signal[2].spiq_in;
    gpio_func_sel(sclk, PIN_FUNC_GPIO);
    gpio_func_sel(mosi, PIN_FUNC_GPIO);
    gpio_func_sel(miso, PIN_FUNC_GPIO);
    gpio_matrix_output(sclk, sig_clk, false, false);
    gpio_matrix_output(mosi, sig_mosi, false, false);
    gpio_matrix_input(miso, sig_miso, false);

    /* ---- ② SPI HAL：时钟 + mode + 固定帧格式（一次性）---- */
    spi_hal_init(&s_hal, 2); /* host_id: 0=SPI1 1=SPI2 2=SPI3 */
    s_hal.dma_enabled = true;
    s_hw = s_hal.hw;

    spi_hal_timing_param_t tp = {
        .clk_src_hz = 80 * 1000 * 1000, /* SPI 时钟源 = APB 80MHz */
        .half_duplex = 0,
        .no_compensate = 0,
        .expected_freq = clk_hz,
        .duty_cycle = 50,
        .input_delay_ns = input_delay_ns,
        .use_gpio = true, /* 引脚走 GPIO 矩阵 */
    };
    s_hal_dev = (spi_hal_dev_config_t){ 0 };
    esp_err_t err = spi_hal_cal_clock_conf(&tp, &s_hal_dev.timing_conf);
    if (err != ESP_OK) {
        return err;
    }
    s_hal_dev.mode = 1;    /* SPI mode 1（与 ADS131M02 对齐） */
    s_hal_dev.cs_pin_id = 0; /* 无物理 CS（引脚未接），内部 CS 线悬空无害 */
    spi_hal_setup_device(&s_hal, &s_hal_dev);

    /* 帧格式恒定：96bit 收发、无 cmd/addr/dummy——装载一次，kick 只发时钟 */
    spi_hal_trans_config_t trans = {
        .cmd = 0,
        .cmd_bits = 0,
        .addr_bits = 0,
        .dummy_bits = 0,
        .tx_bitlen = ENG_FRAME_BYTES * 8,
        .rx_bitlen = ENG_FRAME_BYTES * 8,
        .send_buffer = NULL,
        .rcv_buffer = NULL,
        .line_mode = { .data_lines = 1, .addr_lines = 1, .cmd_lines = 1 },
        .cs_keep_active = 0,
    };
    spi_hal_setup_trans(&s_hal, &s_hal_dev, &trans);
    spi_hal_enable_data_line(s_hw, true, true);

    ESP_LOGI(TAG, "SPI3 就绪：mode1 %d Hz（实得 %d Hz），帧 %d bit",
             clk_hz, s_hal_dev.timing_conf.real_freq, ENG_FRAME_BYTES * 8);

    /* ---- ③ GDMA 双通道接线到 SPI3（与 spi_master 内部做法一致）----
     * 一次调用成对分配 TX/RX；isr_cache_safe=1 让 GDMA 中断落在 IRAM，
     * cache 禁用窗口（NVS 擦写）里仍可服务（设计要求，见文件头）。 */
    gdma_channel_alloc_config_t alloc = {
        .intr_priority = 1,
        .flags = { .isr_cache_safe = 1 },
    };
    err = gdma_new_ahb_channel(&alloc, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        return err;
    }
    /* GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 3)：SPI3 → SOC_GDMA_TRIG_PERIPH_SPI3 */
    err = gdma_connect(s_tx_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 3));
    if (err != ESP_OK) {
        return err;
    }
    err = gdma_connect(s_rx_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 3));
    if (err != ESP_OK) {
        return err;
    }
    /* 数据 burst=32：与 spi_master 默认一致（SPI_DMA_DEFAULT_BURST_SIZE=32；
     * AHB v1 的 set_burst_size 断言只接受 16/32/64，8 崩、0 疑似影响 TX 供数）。 */
    gdma_transfer_config_t tcfg = { .max_data_burst_size = 32 };
    err = gdma_config_transfer(s_tx_chan, &tcfg);
    if (err != ESP_OK) {
        return err;
    }
    err = gdma_config_transfer(s_rx_chan, &tcfg);
    if (err != ESP_OK) {
        return err;
    }

    /* LL 侧需要（寄存器基址, 通道号）：初始化时各取一次 */
    s_gdma = GDMA_LL_GET_HW(0);
    int gid = 0;
    gdma_get_group_channel_id(s_tx_chan, &gid, (int *)&s_tx_ll_chan);
    gdma_get_group_channel_id(s_rx_chan, &gid, (int *)&s_rx_ll_chan);

    /* xSemaphoreCreateBinary：创建二值信号量（只有「有 / 无」两态），
     * 用作 ISR → 任务的单事件通知；返回 NULL 表示堆内存不足。 */
    s_drdy_sem = xSemaphoreCreateBinary();
    if (s_drdy_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* 维护帧完成信号量（M3 直启）：ISR 直启维护帧后 give，xfer_frame take */
    s_maint_done_sem = xSemaphoreCreateBinary();
    if (s_maint_done_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ads_engine_attach_drdy(int drdy_gpio)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << drdy_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* DRDY# 低有效，上拉兜底 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* ESP_INTR_FLAG_IRAM：flash 擦写关 cache 窗口内 DRDY 中断照常服务；
     * 不带该标志时中断分配器会在擦写期间禁用本中断 → 4kHz 丢边沿，
     * NVS 落盘瞬间出现数据缺口（docs/ads131-4khz-implementation-plan.md §4.2）。
     * ISR 服务可能已被他人安装（返回 INVALID_STATE 属正常） */
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return gpio_isr_handler_add(drdy_gpio, drdy_isr, NULL);
}

bool ads_engine_set_stream(bool on)
{
    const bool was = atomic_exchange_explicit(&s_stream_on, on, memory_order_relaxed);
    if (!on) {
        /* 暂停流式（或维护前的对齐起拍）：无条件 drain 掉 ISR 累积的 DRDY
         * 同步信号，让随后的维护帧从「下一个 DRDY」干净起拍——这是 RREG/WREG
         * 两帧协议帧边界连续的前提。⚠ 不能只在 was==true 时 drain：init 阶段
         * 流式本就关闭（was=false），但 DRDY ISR 仍在每个边沿 give 信号量并
         * 饱和为 1，若不 drain，第一个命令帧会吃到陈旧信号、在非帧边界 kick，
         * 导致命令不被芯片识别（实测寄存器读回全 0x0503）。 */
        /* 超时参数给 0 = 不等待、只查询：把信号量里可能残留的「陈旧 DRDY」
         * 逐个取空（循环到 take 返回非 pdTRUE 为止）。 */
        while (xSemaphoreTake(s_drdy_sem, 0) == pdTRUE) {
        }
    }
    return was;
}

esp_err_t ads_engine_xfer_frame(const uint32_t *tx, uint32_t *rx, uint32_t timeout_ms)
{
    if (s_hw == NULL || rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 维护帧直启（M3 修复）：置请求 → DRDY ISR 在边沿精确 kick → 等完成 → 取数。
     * 关键收益：kick 从「任务被调度后」提前到「DRDY 边沿（ISR 内）」，消除任务
     * 调度延迟对帧对齐的影响——这是此前命令不被芯片识别的根因。 */
    s_maint_tx = tx; /* 触发下一个 DRDY 边沿由 ISR 直启本帧 */
    if (xSemaphoreTake(s_maint_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* 超时：清掉悬空请求，避免后续 DRDY 误启陈旧帧 */
        s_maint_tx = NULL;
        return ESP_ERR_TIMEOUT;
    }
    if (!wait_spi_done(1000)) { /* 1ms 封顶：正常 ~5µs，超时即硬件异常 */
        /* 超时 = SPI 事务未能完成，排查入口：描述符 owner 位、cmd.usr、
         * dma_int_raw.trans_done。已知成因两类（2026-10 实测）：
         * ① 外设未上电导致寄存器写入被丢弃（见 ads_engine_init 的
         *    spicommon_periph_claim 注释）；② DMA 未消费描述符。 */
        ESP_LOGE(TAG, "维护帧超时（tx.owner=%u rx.owner=%u usr=%u done=%u）",
                 (unsigned)s_tx_desc.dw0.owner, (unsigned)s_rx_desc.dw0.owner,
                 (unsigned)s_hw->cmd.usr, (unsigned)s_hw->dma_int_raw.trans_done);
        return ESP_ERR_TIMEOUT;
    }
    unpack_words(s_slot[0], rx, ADS_ENGINE_FRAME_WORDS);
    return ESP_OK;
}

esp_err_t ads_engine_read_frame(uint32_t *rx, uint32_t timeout_ms)
{
    if (s_hw == NULL || rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 信号量与 kick 在同一 ISR 内配对给出；消费落后时按序号取旧帧，不错位 */
    if (xSemaphoreTake(s_drdy_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const uint32_t seq = atomic_load_explicit(&s_read_seq, memory_order_relaxed);
    const uint32_t kick = atomic_load_explicit(&s_kick_seq, memory_order_relaxed);
    if (seq == kick) {
        /* 本 DRDY 因槽满没 kick（丢帧）：不产出数据。
         * 【B8 故障分流】丢帧返回 ESP_ERR_INVALID_STATE（区别于等 DRDY 超时的
         * ESP_ERR_TIMEOUT）——丢帧是"错过一帧、下一帧照常"的瞬态，不该与
         * "ADC 真死"混入同一条故障计数。 */
        return ESP_ERR_INVALID_STATE;
    }
    if (!wait_spi_done(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    unpack_words(s_slot[seq % ENG_RING], rx, ADS_ENGINE_FRAME_WORDS);
    atomic_store_explicit(&s_read_seq, seq + 1, memory_order_relaxed);
    return ESP_OK;
}

void ads_engine_get_stats(ads_engine_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    const uint32_t n = s_lat_n;
    out->drdy_count = atomic_load_explicit(&s_drdy_count, memory_order_relaxed);
    out->kick_count = atomic_load_explicit(&s_kick_count, memory_order_relaxed);
    out->drop_count = atomic_load_explicit(&s_drop_count, memory_order_relaxed);
    out->lat_min_us = (n != 0 && s_lat_min_us != UINT32_MAX) ? s_lat_min_us : 0;
    out->lat_avg_us = (n != 0) ? (uint32_t)(s_lat_sum_us / n) : 0;
    out->lat_max_us = s_lat_max_us;
}
