# Context

## Glossary

| Term | Meaning | Notes |
|---|---|---|
| 4kHz 采样链路 | OSR=1024（CLOCK=0x030E）+ 20MHz SPI + DRDY ISR 直启 GDMA 的唯一现行采样口径 | 见 ADR 0001；500Hz（OSR=8192）仅为历史/低噪声可切档，文档不得再写 500SPS 为现状 |
| 墙钟块（200ms block） | 采样任务按墙钟每 200ms 输出一帧 V/I/功率快照，不按样本数计 | `PM_BLOCK_SAMPLES` 仅历史参考 |
| data_store 单写者 | 全固件仅测量核（Core1）写快照，seqlock 无锁读 | 出现第二写者必须改互斥锁方案 |
| 维护模式 | 暂停流式→两帧寄存器 RREG/WREG→恢复的 ADC 控制面访问纪律 | 禁止与流式 kick 并发 |
| 可信仪器版（v0.2.0） | 以「读数可信」为验收的发布里程碑：标定 + 长跑验收 + 仪器功能批 | 与 4kHz 工程里程碑 V-M* 前缀区分 |
| 联网数据版（v0.5.0） | WiFi 配网 / Web 控制台 / CSV 导出 / OTA 的发布里程碑 | — |
| 社区化发布版（v1.0.0） | MQTT/HA、多语言、成本报表的发布里程碑 | — |
| 块均值 | 直流信号块内平均值（实现口径） | 对外文案曾误称 RMS，统一口径见 Spec Q2 |
| 软件旋转（sw_rotate） | 屏幕旋转由 LVGL 软件旋转实现，面板物理方向固定 | 不是 MADCTL 硬旋转，历史文档曾写错 |
| bug-wave-spike / bug-eng-timeout | 两个在途缺陷代号与取证探针标记 | 修复闭环后代码中必须清零（grep 一发命中） |
