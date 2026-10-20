/**
 * @file buzzer.c
 * @brief 无源蜂鸣器驱动实现（LEDC 方波 + esp_timer 定时）
 *
 * 器件：贴片无源蜂鸣器 SMD-5025-0340-12Ω，3V / 4kHz 谐振 / 12Ω
 *       （"无源"意味着内部没有振荡电路，必须由外部输入方波才发声，
 *         因此本驱动用 LEDC 产生方波，而不是简单给高低电平）
 *
 * 阅读顺序建议：
 *   ① 下面的「参数来源说明」；
 *   ② sound_on() / sound_off() / sound_for()——最底层的发声原语；
 *   ③ buzzer_init() / buzzer_set_volume() / buzzer_play() / buzzer_set_alarm()
 *
 * ============================================================
 * ⚠⚠ 本文件不是配置点！修改参数请勿改这里 ⚠⚠
 * ============================================================
 * 本驱动的参数分两类，来源不同，务必区分清楚：
 *
 *   ┌─ ① 编译期固定项：方波频率、三种提示音时长、报警时长与间隔
 *   │     来源：Kconfig（CONFIG_BUZZER_* 宏），本文件直接使用，不设本地默认值。
 *   │     改法：菜单界面，或改 Kconfig 的 default 后删除 sdkconfig 重新生成：
 *   │         idf.py menuconfig → 组件配置 → 无源蜂鸣器（BUZZER）
 *   │     生效：修改后 idf.py build 即生效。
 *   │
 *   └─ ② 运行时可调项：响度
 *         来源：变量 s_volume_percent（初值取自 CONFIG_BUZZER_VOLUME_PERCENT，
 *               即"出厂默认响度"），运行期由 UI 经
 *               device_ctrl_set_buzzer_volume() 修改，并由 settings 服务
 *               存入 NVS、开机自动恢复。
 *         注意：**改 Kconfig 里的响度只影响"出厂默认值"**。如果用户已经在设备上
 *               调过音量，NVS 里存有用户值，它会覆盖出厂默认值——此时改 Kconfig
 *               并重新烧录，设备响度不会变。要让新默认值生效需先擦除 NVS
 *               （或后续实现"恢复出厂设置"功能）。
 *
 * 为什么不设本地默认值？因为一旦写下
 *     #ifdef CONFIG_BUZZER_FREQ_HZ
 *     #define BUZZER_FREQ_HZ CONFIG_BUZZER_FREQ_HZ
 *     #else
 *     #define BUZZER_FREQ_HZ 4000      ← 这行永远不会被编译！
 *     #endif
 * 这类"优先取 Kconfig、否则用回退值"的写法，由于 Kconfig.projbuild 中
 * 已经定义了该项，sdkconfig 里必然存在对应的 CONFIG_ 宏，
 * 于是 #else 分支成为**死代码**：改它不会有任何效果，
 * 而且编译不报错，非常容易误以为"参数改了但硬件没反应"。
 *
 * 上板核对实际生效值的方法：
 *   · 频率与出厂响度：看初始化日志
 *       buzzer: 初始化完成（IO1，方波 4000 Hz，出厂响度 100% → 占空比 511/1023）
 *   · 当前实际响度（可能已被用户改过）：调 device_ctrl_get_buzzer_volume()，
 *     或在修改响度时看日志 "buzzer: 响度已设为 XX%（占空比 Y/1023，下次发声生效）"
 * ============================================================
 */
#include "buzzer.h"

#include "board_pins.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/** 日志标签 */
static const char *TAG = "buzzer";

/** 响度的合法范围（与 settings 服务保持一致） */
#define BUZZER_VOLUME_MIN   1
#define BUZZER_VOLUME_MAX   100

/* ============================================================
 * LEDC 资源分配
 *
 * 使用低速模式（LEDC_LOW_SPEED_MODE）：低速通道在定时器更新时
 * 不会产生毛刺，适合蜂鸣器与背光这类连续输出。
 * 通道 / 定时器编号按板级统一规划，避免与 LCD 背光冲突，
 * 详见 board_pins.h 中的说明。
 * ============================================================ */
#define BUZZER_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER   LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZER_LEDC_RES     LEDC_TIMER_10_BIT

/** 10 位分辨率的占空比范围是 0 ~ 1023 */
#define BUZZER_DUTY_MAX     ((1 << 10) - 1)

/** 静音：占空比 0（输出恒低，MOS 不导通） */
#define BUZZER_DUTY_SILENT  0

/* ============================================================
 * 响度 → 占空比 换算
 *
 * 无源蜂鸣器由方波驱动，其发声强度主要由方波中**基波分量**决定，
 * 而基波幅度正比于 sin(π × 占空比)：
 *   占空比 50% 时 sin(π/2)=1 达到最大 → 最响；
 *   占空比偏离 50%（无论变大变小）基波都会下降 → 变轻。
 * 因此 50% 占空比是最响的驱动点，这一点与"占空比 100% 最响"的直觉不同
 * （占空比 100% 是纯直流，没有交变分量，蜂鸣器完全不发声）。
 *
 * 所以"响度百分比"与"占空比"不是同一个量，换算关系是：
 *   占空比 = 响度% × 满量程 / 2
 *   100% 响度 → 511/1023 ≈ 50% 占空比（最响）
 *    50% 响度 → 255/1023 ≈ 25% 占空比（基波约降到 71%，约 -3dB，差别较轻微）
 *    20% 响度 → 102/1023 ≈ 10% 占空比（基波约降到 31%，约 -10dB，明显）
 *    10% 响度 →  51/1023 ≈  5% 占空比（基波约降到 16%，约 -16dB，很轻）
 * 可见响度控制的效果在低值区更明显，调试时建议用 20 左右做对比。
 * ============================================================ */

/* ============================================================
 * 时长参数（编译期固定项，全部来自 Kconfig，见文件头的"参数来源说明"）
 * ============================================================ */

/** 提示音时长表，下标即 buzzer_tone_t */
static const uint32_t s_tone_duration_ms[] = {
    [BUZZER_TONE_CLICK] = CONFIG_BUZZER_TONE_CLICK_MS,
    [BUZZER_TONE_OK] = CONFIG_BUZZER_TONE_OK_MS,
    [BUZZER_TONE_ERROR] = CONFIG_BUZZER_TONE_ERROR_MS,
};

/** 提示音类型数量，用于参数合法性检查 */
#define TONE_COUNT (sizeof(s_tone_duration_ms) / sizeof(s_tone_duration_ms[0]))

/* ---------- 模块内部状态 ---------- */

/** 关闭发声的定时器（一次性）：到点后静音 */
static esp_timer_handle_t s_tone_timer = NULL;

/** 报警定时器（周期）：每隔一段时间触发一次长鸣 */
static esp_timer_handle_t s_alarm_timer = NULL;

/** 报警是否已开启（volatile：定时器回调与任务都会读） */
static volatile bool s_alarm_enabled = false;

/** 是否已初始化 */
static bool s_initialized = false;

/**
 * 当前响度（1~100）
 *
 * 这是**运行时变量**而不是编译期宏，因为用户后续可以在 UI 上调节音量。
 *   · 初值取 Kconfig 的 CONFIG_BUZZER_VOLUME_PERCENT —— 它只代表"出厂默认响度"；
 *   · 用户改过的值由 settings 服务存进 NVS，开机时经
 *     device_ctrl → buzzer_set_volume() 恢复。
 * 用 volatile 是因为它会被 UI 任务写入、被 esp_timer 回调读取。
 */
static volatile uint8_t s_volume_percent = CONFIG_BUZZER_VOLUME_PERCENT;

/**
 * 保护 s_volume_percent 的自旋锁
 *
 * 写入方是 UI 任务（buzzer_set_volume），读取方是 esp_timer 回调
 * （报警发声）或普通任务（buzzer_play），因此需要一段临界区来保证
 * 读到的不是"写了一半"的值。临界区里只做一次字节读取，开销可忽略。
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * 「发声状态 + 定时器操作」互斥锁
 *
 * sound_for()（任务上下文：UI 提示音 / 报警回调里调用）与 s_tone_timer_callback()
 * （esp_timer 任务）会并发操作发声状态。esp_timer_stop() 只能撤销**尚未触发**的
 * 定时，**不能撤销已在 esp_timer 任务里排队/执行中的旧回调**——旧回调的
 * sound_off() 可能晚于新一轮 sound_on() 执行，把刚打开的声音关掉（表现为
 * 提示音时断时续、偶发无声）。因此「查代际 → 开声/关声 → 启停定时器」整段
 * 都在本锁内完成，任意时刻只有一个执行者碰发声状态。
 * 锁内只做 LEDC 占空比设置与 esp_timer 启停（均为微秒级、不阻塞），
 * 用 mutex 而非临界区是因为 esp_timer API 内部可能睡眠，不可进临界区。
 */
static SemaphoreHandle_t s_sound_mutex = NULL;

/**
 * 发声代际计数（与 s_sound_mutex 配套的"迟到回调丢弃"判据）
 *
 * 每次开启新一轮发声就 +1，并把「本次发声应停止的代际」传给定时器回调
 * （经 s_tone_gen_expected 传递）。回调执行时若 s_tone_gen_expected 与当前
 * 代际不匹配，说明它是**旧一轮发声的迟到回调**（esp_timer_stop 没拦住），
 * 直接返回、不做 sound_off()——新的声音不受它影响。
 * 计数操作都在 s_sound_mutex 内，无需 volatile/原子。
 */
static uint32_t s_sound_gen = 0;
/** 一次性定时器回调应"生效"的代际（= 启动该定时器时的 s_sound_gen） */
static uint32_t s_tone_gen_expected = 0;

/**
 * @brief 根据当前响度计算 PWM 占空比
 *
 * 换算关系见上方"响度 → 占空比 换算"的说明。
 * 在临界区内取出响度快照后再做运算，避免把除法放进临界区。
 */
static uint32_t current_duty(void)
{
    portENTER_CRITICAL(&s_lock);
    uint8_t volume = s_volume_percent;
    portEXIT_CRITICAL(&s_lock);

    return (uint32_t)volume * BUZZER_DUTY_MAX / 200U;
}

/* ---------- 底层发声原语 ---------- */

/**
 * @brief 开始发声（按当前响度输出对应占空比的方波）
 *
 * 每次发声都重新读取响度，因此 UI 改完音量后，从下一声开始即为新音量。
 */
static void sound_on(void)
{
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, current_duty());
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

/**
 * @brief 停止发声（占空比设为 0，输出恒低）
 *
 * 这里不调用 ledc_stop()，而是把占空比设为 0：
 * 让定时器继续运行、只改占空比，切换速度更快，也不会产生输出毛刺。
 */
static void sound_off(void)
{
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY_SILENT);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

/**
 * @brief 发声指定时长后自动停止（非阻塞）
 *
 * 实现方式：立即发声，并启动一次性定时器；定时器到点后调用
 * s_tone_timer_callback() 静音。这样调用方无需阻塞等待，
 * 避免在事件回调里"睡"上几百毫秒。
 *
 * 并发保护：整段「推进代际 → 开声 → 重启定时器」都在 s_sound_mutex 内完成。
 * esp_timer_stop() 拦不住已在执行的旧回调，所以再用代际计数兜底——
 * 迟到的旧回调发现代际不匹配就放弃 sound_off()（见 s_sound_gen 注释）。
 *
 * @param on_ms 发声时长（毫秒）
 */
static void sound_for(uint32_t on_ms)
{
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);

    /* 推进代际：本轮发声从现在开始算新一代，此前任何迟到回调都失效 */
    s_sound_gen++;
    s_tone_gen_expected = s_sound_gen;

    sound_on();

    /* 若上一次提示音还没结束，先取消它的停止定时，改为本次的时长 */
    esp_timer_stop(s_tone_timer); /* 未启动时返回错误，属正常情况，忽略 */

    esp_err_t err = esp_timer_start_once(s_tone_timer, (uint64_t)on_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动发声定时器失败: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_sound_mutex);
}

/**
 * @brief 关闭发声的一次性定时器回调（esp_timer 任务上下文）
 *
 * 与 sound_for() 用 s_sound_mutex 串行化；再核对代际——本回调启动时所在的
 * 那轮发声若已被更新的 sound_for() 取代（s_sound_gen 已推进），说明自己是
 * esp_timer_stop 没拦住的迟到回调，必须放弃 sound_off()，否则会把新一轮
 * 刚打开的声音错误关掉。
 */
static void s_tone_timer_callback(void *arg)
{
    (void)arg;

    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    if (s_tone_gen_expected == s_sound_gen) {
        /* 代际未变：就是本轮发声到点，正常静音 */
        sound_off();
    }
    /* 代际不匹配 = 迟到的旧回调，直接忽略（不 sound_off） */
    xSemaphoreGive(s_sound_mutex);
}

/**
 * @brief 报警定时器回调：每到一个周期就长鸣一声
 *
 * 本回调与 s_tone_timer_callback 由同一个 esp_timer 任务依次执行（不会并发），
 * 但 sound_for() 还会被普通任务（buzzer_play）调用，发声状态的并发保护
 * 统一在 s_sound_mutex + 代际计数里做（见 sound_for）。
 */
static void s_alarm_timer_callback(void *arg)
{
    (void)arg;

    /* 报警可能在回调触发前的瞬间被关闭，这里再确认一次 */
    if (s_alarm_enabled) {
        sound_for(CONFIG_BUZZER_ALARM_ON_MS);
    }
}

/* ---------- 对外接口 ---------- */

esp_err_t buzzer_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 配置 LEDC 定时器：决定方波频率 ---- */
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_LEDC_RES,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = CONFIG_BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK, /* 由驱动自动选择时钟源 */
    };

    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 LEDC 定时器失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 配置 LEDC 通道：绑定到 IO1，初始占空比 0（静音） ---- */
    const ledc_channel_config_t ch_cfg = {
        .gpio_num = BOARD_PIN_BUZZER,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE, /* 不需要 LEDC 中断 */
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = BUZZER_DUTY_SILENT,
        .hpoint = 0,
    };

    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 LEDC 通道失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 创建发声状态互斥锁（sound_for / 定时器回调共用，见 s_sound_mutex 注释） ---- */
    s_sound_mutex = xSemaphoreCreateMutex();
    if (s_sound_mutex == NULL) {
        ESP_LOGE(TAG, "创建发声状态互斥锁失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    /* ---- 创建两个定时器 ---- */
    const esp_timer_create_args_t tone_timer_cfg = {
        .callback = s_tone_timer_callback,
        .arg = NULL,
        .name = "buzzer_tone",
    };
    err = esp_timer_create(&tone_timer_cfg, &s_tone_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建发声定时器失败: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_sound_mutex); /* 回滚已建资源，保证重试 init 不泄漏 */
        s_sound_mutex = NULL;
        return err;
    }

    const esp_timer_create_args_t alarm_timer_cfg = {
        .callback = s_alarm_timer_callback,
        .arg = NULL,
        .name = "buzzer_alarm",
    };
    err = esp_timer_create(&alarm_timer_cfg, &s_alarm_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建报警定时器失败: %s", esp_err_to_name(err));
        esp_timer_delete(s_tone_timer);
        s_tone_timer = NULL;
        vSemaphoreDelete(s_sound_mutex); /* 回滚已建资源，保证重试 init 不泄漏 */
        s_sound_mutex = NULL;
        return err;
    }

    /* 确保初始状态是静音的 */
    sound_off();

    s_alarm_enabled = false;
    s_initialized = true;

    /* 这条日志打印的是"实际编译进去的参数"，上板后可据此核对配置是否生效：
     * 若这里显示的频率/占空比与预期不符，说明 sdkconfig 里的值没更新
     * （改 Kconfig 的 default 后必须删掉 sdkconfig 重新生成）。 */
    ESP_LOGI(TAG, "初始化完成（IO%d，方波 %d Hz，出厂响度 %d%% → 占空比 %u/%d）", BOARD_PIN_BUZZER,
             CONFIG_BUZZER_FREQ_HZ, CONFIG_BUZZER_VOLUME_PERCENT, (unsigned)current_duty(),
             BUZZER_DUTY_MAX);
    return ESP_OK;
}

esp_err_t buzzer_set_volume(uint8_t percent)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < BUZZER_VOLUME_MIN || percent > BUZZER_VOLUME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 只更新响度变量即可：sound_on() 每次发声都会重新读取它。
     * 不在此处改写正在播放的提示音（原因见 buzzer.h 的说明）。 */
    portENTER_CRITICAL(&s_lock);
    s_volume_percent = percent;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "响度已设为 %u%%（占空比 %u/%d，下次发声生效）", percent,
             (unsigned)current_duty(), BUZZER_DUTY_MAX);
    return ESP_OK;
}

esp_err_t buzzer_get_volume(uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    *percent = s_volume_percent;
    portEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}

esp_err_t buzzer_play(buzzer_tone_t tone)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((size_t)tone >= TONE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 报警优先：报警期间忽略普通提示音，避免声音互相打断、
     * 听起来像"报警时断时续" */
    if (s_alarm_enabled) {
        ESP_LOGD(TAG, "报警进行中，忽略提示音 %d", (int)tone);
        return ESP_OK;
    }

    sound_for(s_tone_duration_ms[tone]);
    return ESP_OK;
}

esp_err_t buzzer_set_alarm(bool enable)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 状态未变化时直接返回，避免重复启动定时器（会返回错误） */
    if (enable == s_alarm_enabled) {
        return ESP_OK;
    }

    s_alarm_enabled = enable;

    if (enable) {
        ESP_LOGW(TAG, "报警开启（每 %d ms 长鸣 %d ms）", CONFIG_BUZZER_ALARM_PERIOD_MS,
                 CONFIG_BUZZER_ALARM_ON_MS);

        /* 先停止再启动：esp_timer 要求启动前定时器处于停止状态 */
        esp_timer_stop(s_alarm_timer);
        esp_err_t err = esp_timer_start_periodic(s_alarm_timer,
                                                 (uint64_t)CONFIG_BUZZER_ALARM_PERIOD_MS * 1000ULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "启动报警定时器失败: %s", esp_err_to_name(err));
            s_alarm_enabled = false;
            return err;
        }

        /* 立即响第一声，不必等满一个周期才开始提醒 */
        sound_for(CONFIG_BUZZER_ALARM_ON_MS);
    } else {
        ESP_LOGI(TAG, "报警关闭");

        /* 关声也进锁 + 推进代际：esp_timer_stop 拦不住已在执行的迟到回调，
         * 推进代际让它发现"自己过期"而放弃 sound_off 复活/误关（见 s_sound_gen） */
        xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
        esp_timer_stop(s_alarm_timer);
        esp_timer_stop(s_tone_timer);
        s_sound_gen++; /* 使任何在途的旧 tone 回调失效 */
        sound_off();
        xSemaphoreGive(s_sound_mutex);
    }

    return ESP_OK;
}

esp_err_t buzzer_stop(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_alarm_enabled = false;

    /* 同 buzzer_set_alarm(关闭)：锁内停定时器 + 推进代际 + 静音 */
    xSemaphoreTake(s_sound_mutex, portMAX_DELAY);
    esp_timer_stop(s_alarm_timer);
    esp_timer_stop(s_tone_timer);
    s_sound_gen++; /* 使任何在途的旧 tone 回调失效 */
    sound_off();
    xSemaphoreGive(s_sound_mutex);

    return ESP_OK;
}
