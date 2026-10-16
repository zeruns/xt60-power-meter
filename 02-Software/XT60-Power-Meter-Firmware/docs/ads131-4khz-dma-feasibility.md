# ADS131M02 使用 DMA 与 DRDY 中断恢复 4 kHz 采样的可行性分析

## 结论

单独把当前 SPI 读取改为 DMA，不能解决 4 kHz 档 DRDY 塌频；把 DRDY 下降沿作为触发、再由预先配置的 DMA 在固定短窗口内完成一帧读取，则可以从机制上消除当前塌频原因。[cite:5][cite:6]

4 kHz 档的采样周期是 250 µs。当前驱动使用 4 MHz SPI 和 96 bit 数据帧，总线传输本身只有 24 µs，并不存在 SPI 带宽不足。塌频来自“任务被唤醒后再发起 SPI 事务”的软件响应延迟，使读取窗口跨越下一次转换完成，ADS131M02 因此抑制 DRDY。[cite:5]

因此，正确方案不是“DMA 读取”这个笼统表述，而是 **DRDY 边沿触发、零任务调度的确定性短读窗口**。DMA 是实现该窗口的合适手段，但不是充分条件。

## 当前系统与失效机制

当前 ADS131M02 使用 8.192 MHz CLKIN。HR 模式下调制频率为：

\[
f_{MOD}=\frac{8.192\,\text{MHz}}{2}=4.096\,\text{MHz}
\]

OSR=1024 时：

\[
ODR=\frac{4.096\,\text{MHz}}{1024}=4\,\text{kHz}
\]

OSR=8192 时：

\[
ODR=\frac{4.096\,\text{MHz}}{8192}=500\,\text{Hz}
\]

SBAS853 §8.5.1.5 明确说明：新转换在主机读取转换数据期间完成时，DRDY 脉冲会被屏蔽；为了获得一致的 DRDY 行为，应避免在新转换完成的时刻读取 ADC 数据。[cite:5]

当前固件的数据路径是：

1. DRDY ISR 仅释放信号量。
2. `pm_sample` 任务被调度。
3. 任务获取互斥锁并调用 `spi_device_polling_transmit()`。
4. SPI 外设传输 96 bit，再由 CPU 解析结果。

固件记录的实测结果是，该“等待 DRDY 加读取帧”的周期约为 3 ms，因此在 250 µs 转换周期内必然跨越多个转换完成点，DRDY 实际降到约 310 Hz。[cite:6] 这与手册描述的抑制机制一致，不是 DMA 缺失直接导致的带宽问题。[cite:5]

## 时间预算

ADS131M02 在 2.7–3.6 V 数字电源下要求 SCLK 周期至少 40 ns，即 SCLK 最高 25 MHz。[cite:1] 当前 4 MHz 配置距离该上限很远。

| 数据路径 | 计算 | 耗时 | 占 250 µs 周期 |
|---|---:|---:|---:|
| 当前 4 MHz、96 bit 帧 | \(96 / 4\,\text{MHz}\) | 24.0 µs | 9.6% |
| 20 MHz、96 bit 帧 | \(96 / 20\,\text{MHz}\) | 4.8 µs | 1.9% |
| 20 MHz、72 bit 短帧 | \(72 / 20\,\text{MHz}\) | 3.6 µs | 1.4% |

ESP-IDF 对 ESP32-S3 给出的单字节事务典型持续时间中，CPU 轮询约 9 µs，CPU 中断事务约 24 µs，DMA 轮询约 11 µs，DMA 中断事务约 26 µs。[cite:3] 这些是通用驱动事务开销示例，不能直接等同于本板 12 字节 ADS131 帧的实测值，但足以说明：**把轮询事务换成通用 DMA 中断事务，反而可能增加软件开销。**

因此，如果每次 DRDY 都经过 FreeRTOS 任务唤醒、互斥锁和 `spi_device_transmit()`，DMA 不能保证读窗口落入 250 µs 内。任务切换、锁竞争、缓存未命中和更高优先级中断都可能使启动延迟超过一个采样周期。

## 能成立的实现边界

TI 的 ADS131M02 代码示例本身采用 DRDY 下降沿中断，并由 DMA 在 SPI 输入到达时把 ADC 帧送入预定内存。[cite:4] 这证明该架构是手册认可的数据流模型，但它成立的前提仍是读窗口不跨越下一次转换。[cite:5]

对 ESP32-S3，满足该前提需要以下约束同时成立：

- DRDY ISR 直接启动已经配置好的 SPI/GDMA 描述符，不唤醒任务、不等待互斥锁。
- DMA 缓冲区位于 DMA 可用内部 RAM，并满足驱动的对齐要求。[cite:3]
- 以 DRDY 下降沿作为唯一相位基准，SCLK 在固定的数微秒内开始。
- 整帧在距离下一个 250 µs 转换边界足够远时结束，保留中断抖动和时钟偏差裕量。
- 禁止运行期寄存器诊断在 4 kHz 采样期间插入 SPI 时钟；无 CS 接法下，额外时钟会破坏帧边界。[cite:6]
- 保持 GPIO 中断抖动、DMA 完成中断和后续 CRC/数值处理分开。数值处理可延迟，但 SCLK 启动不能延迟。

在这些条件下，20 MHz、96 bit 帧仅占用 4.8 µs。即使 DRDY 到首个 SCLK 的硬件与软件延迟达到 20 µs，完整读取仍可在约 25 µs 内结束，相对 250 µs 周期留有约 225 µs 裕量。该计算只证明理论预算充足，具体裕量仍需用逻辑分析仪测量 DRDY 与末个 SCLK 的相位后确认。

## 无 CS 带来的限制

本板 ADS131M02 的 CS 固定为选中状态，帧边界不能依靠 CS 上升沿恢复，而只能依靠 DRDY 后连续时钟出整数个字。[cite:6] DMA 一旦漏掉一次 DRDY、重复启动或被其他 SPI 访问插入，错位不会像有 CS 系统那样在下一事务自动恢复。

因此 DMA 方案必须具备：

- 单次 DRDY 只允许启动一个描述符，使用原子状态防止重入。
- DMA 完成后再重新使能下一次启动。
- 连续校验 STATUS 与 CRC，发现错位后执行受控的重新同步，而不是继续解析数据。
- 初始化、寄存器读写和 4 kHz 采样分时进行，不能共享“随时取互斥锁再发 SPI”的模型。

## 对测量能力的影响

恢复 4 kHz 只表示每个 250 µs 周期获得一次转换结果，不等于自动获得 2 kHz 模拟带宽。数字抽取滤波器、前端抗混叠网络和后续 200 ms 功率计算都会限制可用带宽。OSR 从 8192 降到 1024 还会提高单样本噪声，当前固件选择 8192 的噪声收益约为 \(\sqrt{8}\approx2.8\) 倍。[cite:6]

此外，4 kHz 两通道原始数据率为：

\[
4000\times2\times4=32\,\text{KiB/s}
\]

若只在 RAM 中按 200 ms 功率块消费，数据率并不构成 ESP32-S3 的主要瓶颈；若要求无损持续记录或实时上传，则存储和通信链路需要另行核算。

## 实施判断

- **仅把 `spi_device_polling_transmit()` 换成 DMA 版 `spi_device_transmit()`：不能可靠解决。** 根因是事务启动相位不确定。[cite:3][cite:5]
- **DRDY ISR 直接触发预配置 GDMA：原则上可以解决当前 4 kHz DRDY 抑制问题。** 总线时间预算充足，且符合 TI 推荐的数据流结构。[cite:1][cite:4]
- **在改硬件前仍应先做只读测量：** 用逻辑分析仪确认 DRDY 周期、DRDY 到 SCLK 延迟和 SCLK 结束相位。这是验证 4 kHz 方案是否真正避开转换边界的最终证据。[cite:5]

## Sources

[cite:1] [ADS131M02 Timing Requirements, SBAS853 §6.6](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-XXXXXXXX-SF0T-XXXX-XXXX-000000194778)

[cite:2] [ADS131M02 Short SPI Frames, SBAS853 §8.5.1.11](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-4CA275EF-85FF-4928-9F36-9BAD485ED166.html)

[cite:3] [ESP32-S3 SPI Master Driver, ESP-IDF v5.0](https://docs.espressif.com/projects/esp-idf/en/release-v5.0/esp32s3/api-reference/peripherals/spi_master.html)

[cite:4] [ADS131M02 Code Example, SBAS853 §9.1.6](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-85D79E3D-8C39-47EB-A994-9FF480E93F4C.html)

[cite:5] [ADS131M02 Data Ready Behavior, SBAS853 §8.5.1.5](https://www.ti.com/document-viewer/ADS131M02/datasheet/GUID-FEB819FE-9B54-44EF-BEE3-6A61FEFFBDE3.html)

[cite:6] XT60 固件 ADS131M02 驱动实现与 2026-10 实机排查记录：`components/drivers/ads131m02/src/ads131m02.c`
