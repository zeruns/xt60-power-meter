# ADS131M02 4 kHz 采样恢复实施方案：20 MHz SPI + DRDY ISR 直启 GDMA

## 1. 目标与验收标准

目标：将 ADS131M02 恢复到 OSR=1024 档（ODR = 4.096 MHz / 1024 = 4 kHz，250 µs/样本），并在整个运行期稳定拿到每一个转换结果。

量化验收标准：

| 指标 | 标准 |
|---|---|
| DRDY 实测频率 | 4000 Hz ±0.05% 持续成立（不再出现 ~310 Hz 塌频） |
| DRDY→首个 SCLK 启动延迟 | p99 ≤ 20 µs（逻辑分析仪实测） |
| 读窗口终点距下一转换完成 | ≥ 200 µs（即读窗口绝不跨越转换时刻） |
| 24 h 长跑 | CRC 失败 = 0、丢帧 = 0、重同步 = 0 |
| 200 ms 块统计（V/I/RMS/P） | 与现行 500 Hz 档偏差在测量噪声范围内 |

不变项：data_store 40 Hz 写入节奏、200 ms 块统计口径、UI 与显示链路均不改动。

## 2. 现状与差距

ADS131M02 位于 SPI3（SPI2 被 LCD QSPI 占用），独占一条总线，不存在跨设备仲裁冲突——这一判断正确。仍需管理的并发只有两类：**同一 SPI3 上的"采样帧"与"寄存器访问"自冲突**（现行代码用 `s_bus_mutex` 串行化），以及 **flash 擦写期间 cache 禁用对中断执行的影响**（与总线是否共享无关）[cite:8]。

| 维度 | 现状（ads131m02.c） | 本方案目标 |
|---|---|---|
| SPI 时钟 | 4 MHz（保守值） | 20 MHz（带降频护栏，见 §4.1） |
| DMA | `SPI_DMA_DISABLED`（"数据量极小，无需 DMA"） | GDMA 描述符直传 |
| 启动路径 | DRDY ISR → 信号量 → 任务 → `spi_device_polling_transmit()` | DRDY ISR 内直接装载并启动 GDMA |
| 采样档位 | OSR=8192 / 500 Hz（`ADS_CLOCK_VALUE=0x031A`） | OSR=1024 / 4 kHz（`0x030E`，恰为芯片复位值） |
| 帧格式 | 4×24 bit（STATUS+CH0+CH1+CRC） | 保留不变 |
| 无 CS | 帧边界靠 DRDY 对齐 + 整数字数 | 保留，新增 CRC 重同步状态机 |

原塌频机理（2026-10 实机排查结论，SBAS853 §8.5.1.5 原文佐证）：读取窗口跨越转换完成时刻时 DRDY 被硬件抑制 [cite:3]；4 kHz 档 250 µs 周期下，任务调度路径的抖动使读帧必然跨界。本方案的两个核心动作——20 MHz 缩短读窗、ISR 直启锁定相位——正是针对这两个成因。

## 3. 总体架构

数据面与控制面彻底分离：

```
DRDY# 下降沿
   │
   ▼
[DRDY ISR @IRAM]──检查引擎空闲→重装 RX 描述符→gdma_start()+SPI 触发
   │                                                  │
   │                                            SPI3 读 4 字帧（96 bit）
   │                                                  │
   ▼                                                  ▼
drdy_count++                                    [GDMA RX done 回调]
                                                       │  槽推进 + 唤醒
                                                       ▼
                                        [pm_sample 任务 Core1]
                                          解析 24bit → CRC 校验
                                          → 200ms 块累加（800 样本/块）
                                                       │
                                                       ▼
                                          data_store（40Hz 快照，不变）
```

- **数据面**只处理 NULL 命令帧（首字 STATUS 自动回读），唯一任务是"每 DRDY 一帧"。
- **控制面**（初始化、UNLOCK/WREG、诊断 RREG）在**维护模式**下运行：暂停数据面 → 按两帧 RREG/WREG 协议访问 → 恢复数据面。ADS131M02 输出 FIFO 每通道可缓 2 个样本 [cite:6]，短暂停采可被容忍，恢复后从最新样本继续。

## 4. 关键设计决策

### 4.1 SPI 20 MHz 与 MISO 时序护栏（本方案最大硬件风险）

芯片侧时序允许 20 MHz：SCLK 周期最小 40 ns（25 MHz 上限）[cite:1]。但主机侧存在临界项——

- ADS131M02 的 DOUT 有效延迟 t_p(SCDO) 最大 20 ns（SCLK 上升沿→DOUT 有效，DVDD 2.7–3.6 V）[cite:2]；
- SPI mode 1 下主机在下降沿锁存，20 MHz 半周期仅 25 ns，扣除 t_p(SCDO) 后只剩 ≤5 ns，再扣 GPIO 矩阵布线与采样延迟，**裕量趋近于 0**；
- ESP-IDF 明确指出 MISO 建立时间是 SPI 频率的限制因素，并提供 `input_delay_ns`（从机数据有效时间）与 `spi_get_freq_limit(gpio_is_used, input_delay_ns)` 运行时上限查询 [cite:7]。

三层护栏：

1. `spi_device_interface_config_t.input_delay_ns = 20`（或裸引擎等效补偿）；
2. init 时调 `spi_get_freq_limit(true, 20000)`，若返回值 < 20 MHz，**自动降频到该上限**（15/12.5/10 MHz 任一档都远够用：10 MHz 时读窗仅 9.6 µs，占 250 µs 周期 3.8%）；
3. 逻辑分析仪实测 MISO 与锁存沿的相位关系（§8 验证计划）。

**降频不影响 4 kHz 目标**——4 kHz 的约束是"读窗不跨界"，不是带宽。20 MHz 是目标值，护栏保证任何降档都自动完成、无需人工干预。

### 4.2 ISR 直启 GDMA 的实现路线

ESP-IDF `spi_master` 的所有事务启动 API 都是任务上下文语义（`spi_device_queue_trans()` 入队即启动、`spi_device_polling_start()` 非 ISR 安全）[cite:7]，无法在 DRDY ISR 内按相位启动事务。因此"ISR 直启 GDMA"需要自研一个**最小 SPI3 数据引擎**，绕过 `spi_master` 驱动（同一 SPI 外设不可两套驱动并存，全部帧统一走引擎）。

已核实本机 ESP-IDF v6.1 的构建块（真实路径/符号）[cite:9]：

| 构件 | v6.1 位置 / 符号 |
|---|---|
| SPI 底层 | `components/esp_hal_gpspi/esp32s3/include/hal/spi_ll.h`（配 SPI3：mode 1、96 bit/帧、20 MHz 分频） |
| GDMA 驱动 | `components/esp_driver_dma/include/esp_private/gdma.h`：`gdma_new_ahb_channel()` → `gdma_connect(chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2))`（SPI3 = 实例 2）→ `gdma_config_transfer()` → `gdma_start(desc_base)` |
| 完成通知 | `gdma_register_rx_event_callbacks()`（on_recv_done） |
| 描述符 | `lldesc_t` 链表，RX/TX 各一 |

**DRDY ISR（全部 IRAM，`ESP_INTR_FLAG_IRAM`）流程**：

1. 原子检查引擎状态，防重入（漏帧只计数 `lost_frame++`，绝不排队叠帧）；
2. 将 RX 描述符指向环形缓冲当前槽（12 B，32 B 对齐）；
3. `gdma_start()` + SPI LL 启动 96 bit 传输；
4. `drdy_count++`，退出。

**GDMA RX done 回调**：槽指针推进（8 槽环形，= 2 ms @4 kHz）+ `xTaskNotifyGiveFromISR` 唤醒消费任务。解析、CRC、累加全部在任务侧完成，ISR 只搬指针。

**寄存器访问**由同一引擎实现现有两帧 RREG/WREG 协议（[ads131m02.c](../components/drivers/ads131m02/src/ads131m02.c) 的 `ads_read_reg/ads_write_reg` 时序语义原样移植），仅在维护模式运行，彻底消除数据面/控制面并发。

**兜底路线（推荐先跑通再升级）**：公开 API 的等价实现——`spi_master` 开 DMA（`SPI_DMA_CH_AUTO`）+ DRDY ISR 仅 `xTaskNotifyGiveFromISR` + Core1 prio 25 任务内 `spi_device_polling_transmit()`。该路径启动延迟典型 1–20 µs，同样满足 250 µs 预算，且全公开 API、零私有依赖。它的价值：① 作为引擎版的测试基准（δ 分布对照）；② 引擎遇到 HAL 问题时的回退形态。两版的验收指标完全相同。

### 4.3 无 CS 帧完整性与重同步

无 CS 接法下帧边界没有硬件锚点，错位不自愈（现行注释记录的毛刺值=STATUS 字错位读数即为此类）[cite:10]。对策：

- **保留 4 字帧**（含 CRC-16/CCITT，覆盖 STATUS+CH0+CH1 共 9 B）。不采用短帧（§8.5.1.11）省掉 CRC 字——无 CS 下 CRC 是唯一完整性防线，4.8 µs 的帧时长不值得省 [cite:4]；
- 每帧 CRC 校验 + STATUS 保留位模式检查，错误帧直接丢弃；
- **重同步状态机**：连续 4 帧 CRC 错 → 判定错位 → 停数据面 → 丢弃 1 帧重新对齐 DRDY → 仍失败则拉 RESET# 硬复位重初始化；
- 统计计数器：`drdy_count / frame_ok / crc_fail / lost_frame / resync_count`，经 `sys_monitor` 或日志暴露，验收时作为 24 h 稳定性的直接判据。

### 4.4 OSR=1024 的噪声代价与缓解

OSR 从 8192 降到 1024，单样本噪声按 √8 ≈ 2.8 倍上升（过采样比 8 倍）。影响面评估：

- **200 ms 块统计不受影响**：800 样本平均后噪声贡献 ≈ 2.8/√800 ≈ 0.1 倍原始单样本噪声，相对 500 Hz 档（250 样本/块）反而略优；
- **波形/瞬时读数**更"毛"：显示路径加一阶指数平滑或 4~8 点滑动平均（仅 UI 层，不进统计）；
- CLOCK 寄存器档位保留运行时可配（现有注释机制沿用），默认 1024，需要低噪声慢速测量时可切回 8192。

### 4.5 下游管线适配

| 模块 | 改动 |
|---|---|
| `power_measure` | 采样循环 500 Hz→4 kHz（2 ms→250 µs）；200 ms 块样本数 250→800；累加器计算量 ×8，估算 Core1 占用 < 5%（实测复核） |
| `data_store` | 无代码改动（25 ms 显示子窗 / 40 Hz 写快照 / 200 ms 块均不变） |
| 波形显示 | 4 kHz 数据已覆盖现有 40 Hz 刷新需求，下采样逻辑不变；受益点是峰值捕获分辨率 ×8 |
| 内存 | 帧 12 B × 4000 Hz = 48 KB/s；8 槽环形缓冲仅 96 B，外加消费侧 800 样本块累加器，总增量 < 2 KB |

## 5. 时序预算

| 环节 | 路线 B（ISR 直启） | 路线 A（兜底） | 依据 |
|---|---|---|---|
| DRDY→ISR 进入 | 1–3 µs | 1–3 µs | IRAM 中断 [cite:8] |
| 装载描述符 + 启动 | ≤ 2 µs | 任务切换 1–20 µs | 估算，实测验收 |
| SCLK 传输（96 bit） | 4.8 µs @20 MHz / 9.6 µs @10 MHz | 同左 | [cite:1] |
| **读窗口总长** | **≤ 10 µs @20 MHz** | ≤ 30 µs | — |
| 距下一转换完成余量 | ≥ 240 µs | ≥ 220 µs | 250 µs 周期 − 读窗 |

只要"DRDY 后启动延迟 + 传输时长 < 250 µs"稳定成立，DRDY 就不会被抑制 [cite:3]。两条路线均有 ≥200 µs 余量；路线 B 的意义在于把余量从"调度余量"变成"中断余量"，扛住 flash 擦写、UI 高负载等极端场景。

## 6. 模块与文件改动清单

| 文件 | 改动 |
|---|---|
| `components/drivers/ads131m02/`（拆新文件 `ads131m02_engine.c`，或并入现有 .c，倾向拆分） | SPI3+GDMA 引擎：描述符管理、DRDY ISR、RX done 回调、两帧 RREG/WREG、重同步状态机、统计计数 |
| [ads131m02.h](../components/drivers/ads131m02/include/ads131m02.h) | `ADS_ODR_HZ` 4000；`ADS_CLOCK_VALUE` 0x030E（= 复位值，注释呼应此前修正）；新增 `ads131m02_stats_t` 与查询/重同步 API |
| [power_measure.c](../components/services/power_measure/src/power_measure.c) / `.h` | 采样周期、块样本数 800、500 Hz→4 kHz 口径注释 |
| [app_tasks.c](../main/app_tasks.c) | `pm_sample` 优先级/亲和调整说明（路线 A 需 prio 25；路线 B 消费任务维持 22 即可） |
| `components/drivers/ads131m02/CMakeLists.txt` | 依赖 `esp_driver_dma`（`gdma.h` 为 esp_private 头，同组件内可用）、`esp_hal_gpspi`、`esp_hal_dma`（组件名以 v6.1 实测为准） |
| `sdkconfig.defaults` | 无需新增项；若弃用 spi_master 则清理 `CONFIG_SPI_MASTER_*` 相关注释口径 |

## 7. 实施步骤（里程碑）

1. **M1 基线链路**：OSR=1024 + 20 MHz（带 §4.1 护栏）+ DMA 传输 + 两帧协议回归 → 逻辑分析仪测 δ 分布 → 验收 §1 启动延迟指标。
2. **M2 数据引擎**：SPI3+GDMA 裸引擎 + DRDY ISR 直启 + 8 槽环形缓冲 → 与 M1 同指标对照。
3. **M3 控制面与容错**：维护模式寄存器访问、CRC 重同步状态机、统计计数器。
4. **M4 下游适配**：power_measure 4 kHz / 800 样本块、显示平滑、文档口径更新（架构图、`data_store.h`、`sdkconfig.defaults` 注释）。
5. **M5 全量验证**：§8 测试 + 24 h 长跑 +（若 M2 引擎受阻，回退路线 A 冻结发布）。

每个里程碑独立可编译、可烧录、可验证，出问题可单步回退。

## 8. 验证计划

| 层 | 手段 | 判据 |
|---|---|---|
| 物理层 | 逻辑分析仪（DRDY + SCLK + MISO） | DRDY 周期 250 µs；δ 直方图 p99 ≤ 20 µs；读窗终点距转换完成 ≥ 200 µs；20 MHz 下 MISO 与锁存沿相位目检 |
| 固件计数 | `ads131m02_stats_t` 定期快照 | 24 h：crc_fail = 0、lost_frame = 0、resync = 0；drdy_count = 4000×t |
| 测量一致性 | 与 500 Hz 档同负载对比 | 200 ms 块 V/I/RMS/P 一致（噪声范围内）；单样本噪声按预期升 ~2.8 倍 |
| 压力 | UI 连滑 + NVS 保存 + 长跑 | δ 与丢帧指标不劣化（flash 擦写窗口是 ISR 直启的核心价值场景 [cite:8]） |
| 恢复 | 上电、复位、人为打断 SPI 后 | 重同步状态机自愈，无需断电 |

## 9. 风险与回退

| 风险 | 概率/影响 | 缓解 / 回退 |
|---|---|---|
| 20 MHz MISO 裕量不足（t_p(SCDO)=20 ns vs 半周期 25 ns）[cite:2] | 中/中 | `input_delay_ns` + `spi_get_freq_limit` 自动降频（10–15 MHz 仍满足全部指标）；逻辑分析仪定档 |
| HAL/私有 API（`esp_private/gdma.h`、`spi_ll`）随 IDF 版本演进 | 中/低 | 项目锁定 v6.1；引擎收敛在单文件；路线 A（公开 API）为回退形态 |
| 无 CS 帧错位不自愈 | 低/高 | CRC 校验 + 重同步状态机 + 兜底硬复位；统计计数可观测 |
| flash 擦写禁用 cache 打断中断 | 中/中 | ISR 全 IRAM、描述符/缓冲放 DRAM 且 32 B 对齐；DRDY 中断 `ESP_INTR_FLAG_IRAM` [cite:8] |
| OSR=1024 单样本噪声 ×2.8 | 必然/低 | 显示层平滑；块统计无感（§4.4）；档位可切回 8192 |

## Sources

[cite:1] [ADS131M02 Timing Requirements, SBAS853 §6.6](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-XXXXXXXX-SF0T-XXXX-XXXX-000000194778)

[cite:2] [ADS131M02 Switching Characteristics, SBAS853 §6.7](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-XXXXXXXX-SF0T-XXXX-XXXX-000000194779)

[cite:3] [ADS131M02 Data Ready Behavior, SBAS853 §8.5.1.5](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-FEB819FE-9B54-44EF-BEE3-6A61FEFFBDE3.html)

[cite:4] [ADS131M02 SPI Communication Frames, SBAS853 §8.5.1.7](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-257EF576-2FE9-44F6-BC90-ABB1152FD4F9.html)

[cite:5] [ADS131M02 Code Example（DRDY 中断 + DMA 数据流）, SBAS853 §9.1.6](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-85D79E3D-8C39-47EB-A994-9FF480E93F4C.html)

[cite:6] [ADS131M02 数据采集暂停与 FIFO 行为, SBAS853 §8.5.1.9.1](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-26B5DE1F-0DE7-4814-B943-90F65B55B8E8.html)

[cite:7] [ESP32-S3 SPI Master Driver（input_delay_ns / spi_get_freq_limit / 事务模型）, ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.4.3/esp32s3/api-reference/peripherals/spi_master.html)

[cite:8] [ESP-IDF IRAM-Safe Interrupt Handlers 与 flash 并发约束](https://docs.espressif.com/projects/esp-idf/en/v5.0.5/esp32s3/api-reference/storage/spi_flash_concurrency.html)

[cite:9] 本机 ESP-IDF v6.1 源码：`components/esp_driver_dma/include/esp_private/gdma.h`（`gdma_new_ahb_channel` / `gdma_connect` / `gdma_start`）、`components/esp_hal_gpspi/esp32s3/include/hal/spi_ll.h`、`components/esp_hal_dma/esp32s3/include/hal/gdma_ll.h`

[cite:10] 项目驱动实现与实机排查记录：`components/drivers/ads131m02/src/ads131m02.c`（帧错位毛刺、无 CS 约束、DRDY 抑制实测）
