# ADR 0001: 采样链路采用 4kHz（OSR=1024 + 20MHz SPI + DRDY ISR 直启 GDMA）

Status: Accepted
Date: 2026-10-16

## Context

ADS131M02 在「4MHz SPI + 任务调度读取」路径下，读窗跨越转换完成时刻触发手册 §8.5.1.5 的 DRDY 抑制机制，实测采样率塌到 ~310Hz。为绕行曾把 OSR 降到 8192（500Hz 档）降低时序压力。500Hz 带宽不足以支撑示波器风格波形（1s 窗每槽仅数点、包络失效），而单纯把 SPI 轮询换 DMA 并不能消除 DRDY 塌频。备选方案：(a) 维持 500Hz；(b) 4kHz 但走任务路径（已被证伪）；(c) 4kHz + DRDY ISR 直启 GDMA 确定性短读窗。

## Decision

采用 (c)：CLOCK=0x030E（OSR=1024、HR，ODR=4000SPS）、20MHz SPI、DRDY ISR 内重臂描述符直启 GDMA、任务侧解析/CRC/累加，读窗压缩至 µs 级并远离转换完成时刻。500Hz（OSR=8192）保留为历史/低噪声可切档，不再作为文档现状口径。

## Consequences

- 正面：波形 tier0 可达 4000Hz×4000（1s 窗满分辨率），包络/量程逻辑有足够源点；测量链路对任务调度抖动免疫。
- 负面：噪声较 OSR=8192 高约 √8≈2.8 倍（显示层 8 点滑动平均 ≈500Hz 等效带宽补偿）；ISR IRAM 约束极严（`CONFIG_ESP_TIMER_IN_IRAM`、IRAM_ATTR、GPIO ISR IRAM safe 缺一即在 NVS 擦写窗口崩溃）。
- 后续约束：任何新增 ADC 控制面访问必须走「维护模式」纪律；20MHz 的 MISO 时序裕量是最大硬件风险，验收依赖引擎 δ 统计与 24h 长跑（无逻辑分析仪时以统计计数器为准）。
