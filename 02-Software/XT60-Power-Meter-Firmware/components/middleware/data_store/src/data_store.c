/**
 * @file data_store.c
 * @brief 数据仓库实现：用 seqlock（顺序锁）实现无锁的跨核数据交换
 *
 * 为什么这里用原子操作（<stdatomic.h>）而不是 FreeRTOS 的互斥锁？
 * ─────────────────────────────────────────────────────────────
 * ① 测量核以 1kHz 写入，绝不能因为等锁而错过采样节拍；
 * ② 读取方只是"看一眼"，重读一次的代价远小于加锁解锁的开销；
 * ③ ESP32-S3 是双核 Xtensa，编译器提供的原子读写会编译成
 *    「读取-独占」这类指令，天然保证多核之间的一致性，
 *    不需要额外用临界区关中断（关中断会影响采样中断响应）。
 *
 * 关于 memory_order 的说明（入门者可以这样理解）：
 *   编译器与 CPU 都可能为了性能「调整指令顺序」，这在单线程里没问题，
 *   但跨核共享数据时会导致别人看到"半成品"。
 *   memory_order_acquire / release 就是在告诉编译器：
 *   「这条语句前后的内存访问顺序不能乱动」，从而保证别的核
 *   要么看到全部新数据，要么全部看不到。
 *
 * seqlock 的读写协议（配合 data_store_update/data_store_get 阅读）：
 *   写端：序号+1（变奇数，宣告"写入中"）→ 拷贝数据 → 序号+1（变偶数，宣告"写完了"）；
 *   读端：读序号 → 拷贝数据 → 再读序号，两次相同且为偶数才算成功。
 *   序号检查要"做两次"，是为了抓住"拷贝到一半时写者插了进来"的情况——
 *   这种撕裂数据（新老字段混搭）正是 seqlock 要消灭的问题。
 */
#include "data_store.h"

#include <stdatomic.h> /* C11 原子操作：atomic_load/store、atomic_thread_fence 等 */
#include <string.h>

#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "data_store";

/**
 * 读取失败后的最大重试次数
 *
 * 每次重试只是重新拷贝 24 字节，代价极小。
 * 正常情况（1kHz 写、30Hz 读）几乎不会发生冲突，第一次就能成功；
 * 设置上限只是为了防止极端异常下陷入死循环。
 */
#define DATA_STORE_MAX_RETRY 16

/**
 * 数据仓库的内部存储
 *
 * seq 就是 seqlock 的序号：偶数表示数据稳定，奇数表示正在写入。
 * 它必须用 atomic_uint 而不是普通 uint32_t，否则编译器可能把它
 * 优化到寄存器里，导致另一个核永远看不到更新。
 */
static struct {
    atomic_uint seq;                /**< seqlock 序号：奇=写入中，偶=稳定 */
    data_store_snapshot_t snapshot; /**< 快照数据本体 */
    atomic_uint update_count;       /**< 累计写入次数 */
    atomic_bool has_data;           /**< 是否已有过至少一次有效写入 */
} s_store;

/**
 * @brief 初始化数据仓库
 *
 * 只做内部状态清零，不分配动态内存（整份数据是静态分配的）。
 *
 * @return ESP_OK 成功
 */
esp_err_t data_store_init(void)
{
    /* 全部清零即可。atomic 类型支持直接赋值 0。
     * memory_order_relaxed：纯初始化清零，此时还没有别的核在读，
     * 不需要额外的顺序保证，用最弱的约束换取最少的性能开销 */
    atomic_store_explicit(&s_store.seq, 0, memory_order_relaxed);
    atomic_store_explicit(&s_store.update_count, 0, memory_order_relaxed);
    atomic_store_explicit(&s_store.has_data, false, memory_order_relaxed);
    memset(&s_store.snapshot, 0, sizeof(s_store.snapshot));

    ESP_LOGI(TAG, "初始化完成（seqlock 无锁快照，快照大小 %u 字节）",
             (unsigned)sizeof(data_store_snapshot_t));
    return ESP_OK;
}

/**
 * @brief 写入最新快照（只能由测量核调用，单写者！）
 *
 * 完整的 seqlock 写协议共 6 步，见函数体内 ①~⑥ 注释。
 * 本函数不会阻塞，任何时刻调用都能立即返回。
 *
 * @param snapshot 要写入的快照，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为 NULL
 */
esp_err_t data_store_update(const data_store_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 读入当前序号（应该是偶数），不做顺序约束，因为下面马上要改写它 */
    unsigned int seq = atomic_load_explicit(&s_store.seq, memory_order_relaxed);

    /* 第 ① 步：序号 +1 变成奇数 —— 向所有读者宣告「我要开始写了」 */
    atomic_store_explicit(&s_store.seq, seq + 1, memory_order_relaxed);

    /* 第 ② 步：这一句保证「序号变奇数」先于「数据拷贝」被其他核看到，
     *          否则读者可能先看到新数据再看到奇数序号，导致误判。
     * atomic_thread_fence(release)：内存栅栏，禁止其前后的内存访问
     * 被编译器/CPU 重排序（release 侧约束写入顺序） */
    atomic_thread_fence(memory_order_release);

    /* 第 ③ 步：拷贝数据本体（24 字节，几条指令即可完成） */
    s_store.snapshot = *snapshot;

    /* 第 ④ 步：保证「数据拷贝」先于「序号变偶数」被其他核看到，
     *          否则读者可能先看到偶数序号、却读到还没写完的数据 */
    atomic_thread_fence(memory_order_release);

    /* 第 ⑤ 步：序号 +2（seq+1 → seq+2），变回偶数 —— 宣告「数据已完整」 */
    atomic_store_explicit(&s_store.seq, seq + 2, memory_order_relaxed);

    /* 第 ⑥ 步：更新计数与「已有数据」标志，供读取方判断数据是否可用。
     * atomic_fetch_add：原子地"读出旧值并加一"，多核下也不会数错 */
    atomic_fetch_add_explicit(&s_store.update_count, 1, memory_order_relaxed);
    atomic_store_explicit(&s_store.has_data, true, memory_order_relaxed);

    return ESP_OK;
}

/**
 * @brief 读取最新快照（交互核 / 任意任务可调用）
 *
 * 内部采用 seqlock 重试机制，保证读到的是一份「自洽」的数据，
 * 不会出现新老字段混搭（数据撕裂）的情况。
 *
 * @param out 输出快照，不可为 NULL
 * @return ESP_OK 成功读到一致数据
 *         ESP_ERR_INVALID_ARG 参数为 NULL
 *         ESP_ERR_INVALID_STATE 尚无任何数据写入过
 *         ESP_ERR_TIMEOUT 连续重试仍未能读到一致数据（理论上极少发生，
 *                         通常意味着写入过于频繁或系统异常）
 */
esp_err_t data_store_get(data_store_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 还没有任何数据写入过时，明确区分「没有数据」与「数据是 0」，
     * 避免调用方把初始化时的全零误当成真实测量结果 */
    if (!atomic_load_explicit(&s_store.has_data, memory_order_relaxed)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 读端核心：最多重试 DATA_STORE_MAX_RETRY 次，直到拿到一致的数据 */
    for (int retry = 0; retry < DATA_STORE_MAX_RETRY; retry++) {
        /* 读序号。用 acquire 语义确保下面读到的数据不会"跑到"序号读取之前 */
        unsigned int seq_before = atomic_load_explicit(&s_store.seq, memory_order_acquire);

        /* 序号是奇数 → 写入正在进行，此刻的数据不可信，立刻重试 */
        if (seq_before & 1U) {
            continue;
        }

        /* 拷贝数据本体 */
        *out = s_store.snapshot;

        /* 再次读序号。这一句用 acquire，保证数据拷贝不会被重排到它之后 */
        atomic_thread_fence(memory_order_acquire);
        unsigned int seq_after = atomic_load_explicit(&s_store.seq, memory_order_relaxed);

        /* 前后序号一致 → 拷贝期间没有发生写入，这份数据一定自洽 */
        if (seq_before == seq_after) {
            return ESP_OK;
        }

        /* 序号变了 → 拷贝过程中被写入打断（读到了撕裂数据），重来一次。
         * 这就是"读端校验序号两次"的原因：只查一次抓不住中途插进来的写者 */
    }

    /* 重试若干次仍不稳定：可能是写入频率异常高，返回超时让调用方决定如何处理 */
    ESP_LOGW(TAG, "读取重试 %d 次仍未获得一致数据，请检查写入频率", DATA_STORE_MAX_RETRY);
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 查询累计写入次数
 *
 * 用途：交互核可以据此判断"数据是否有更新"，避免重复处理同一份数据；
 * 也便于在调试时确认测量核是否真的在跑。
 *
 * @return 自初始化以来 data_store_update() 被调用的总次数
 */
uint32_t data_store_update_count(void)
{
    /* memory_order_relaxed：只读个统计数字，不需要与其他数据保持顺序关系 */
    return atomic_load_explicit(&s_store.update_count, memory_order_relaxed);
}
