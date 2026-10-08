# 引脚分配表（XT60 功率计）

- 依据：`01-Hardware/02-上板原理图_V2.pdf`（更新 2026-09-22）、`01-Hardware/02-下板原理图.pdf`
- 代码唯一真相源：[board_pins.h](file:///e:/Electronic/80.Project/XT60-Power-Meter/02-Software/XT60-Power-Meter-Firmware/components/bsp/include/board_pins.h)
- 主控：ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB 八线 PSRAM）

## 1. 按功能分类的引脚分配

### 指示与提示

| 功能 | GPIO | 原理图网络 | 电路 / 备注 | 有效电平 |
|---|---|---|---|---|
| 系统状态指示灯 | IO39 | SYS_LED | 3V3 → R25 1kΩ → LED2 → IO39 | **低电平点亮** |
| 蜂鸣器（无源） | IO1 | BUZZER | 经 Q5 (AO3400A) 低边驱动 BUZZER1 | 高频方波（约 4 kHz 谐振点） |
| LCD 背光亮度 | IO45 | BL_CTR | 经 Q3 (AO3400A) 低边驱动 LED-，LEDC PWM | 占空比调光 |

### 输入

| 功能 | GPIO | 原理图网络 | 电路 / 备注 | 有效电平 |
|---|---|---|---|---|
| 按键 K1（兼 BOOT） | IO0 | GPIO0 | SW2，**strapping 脚** | 低 |
| 按键 K2 | IO4 | K2 | SW4 | 低 |
| 电池电压采样 | IO2 | BAT_ADC | VBAT → R32 100kΩ → IO2 → R38 100kΩ → GND，**1:1 分压** | 电压 = 读数 × 2 |
| 触摸中断 | IO46 | TP_INT | CST816D INT，**strapping 脚** | 低 |

### LCD（NV3030B，240×284 IPS，QSPI）

| 功能 | GPIO | 原理图网络 | 备注 |
|---|---|---|---|
| 片选 | IO15 | LCD_CS | |
| 时钟 | IO18 | LCD_SCK | |
| 数据 0 | IO16 | LCD_MOSI | QSPI SDIO0 |
| 数据 1 | IO17 | LCD_D1 | QSPI SDIO1 |
| 数据 2 | IO3 | LCD_D2 | QSPI SDIO2，**strapping 脚** |
| 数据 3 | IO8 | LCD_D3 | QSPI SDIO3 |
| 复位 | IO7 | LCD_RST | |
| 撕裂同步 | IO48 | LCD_TE | 与 LVGL 刷新同步 |

### 电容触摸（CST816D，I2C 从地址 0x15）

| 功能 | GPIO | 原理图网络 |
|---|---|---|
| SCL | IO5 | TP_SCL |
| SDA | IO6 | TP_SDA |
| INT | IO46 | TP_INT |

### ADS131M02（24bit 双通道 ΔΣ ADC，SPI）

| 功能 | GPIO | 原理图网络 | 备注 |
|---|---|---|---|
| SCLK | IO11 | ADC_SCLK | |
| MOSI（DIN） | IO9 | ADC_MOSI | |
| MISO（DOUT） | IO10 | ADC_MISO | |
| DRDY# | IO12 | ADC_DRDY# | 下降沿表示新数据就绪（中断输入） |
| SYNC#/RESET# | IO13 | ADC_RST | |
| 时钟 | IO14 | ESP_CLK → ADC_CLK(CLKIN) | R27/R29 两个 0Ω 电阻择一：由 ESP32 输出或板载 8.192MHz 晶振提供 |

### 传感器 I2C 总线（SCL=IO21 / SDA=IO47，2.2kΩ 上拉）

| 器件 | 型号 | 7 位地址 | 用途 |
|---|---|---|---|
| 温湿度 | AHT20 | 0x38 | 功率计内部温湿度 |
| 三轴加速度计 | SC7A20HTR | 0x19 | 屏幕方向自动旋转 |

### USB（原生 USB-Serial/JTAG，兼作日志与烧录口）

| 功能 | GPIO | 原理图网络 | 备注 |
|---|---|---|---|
| D- | IO19 | D- → R34 22Ω → USB_D- | |
| D+ | IO20 | D+ → R36 22Ω → USB_D+ | |

### 电源 / 模拟部分（非 GPIO）

| 器件 | 型号 | 说明 |
|---|---|---|
| 3.3V Buck（XT60 供电） | SY8511BADC | 最大 1 A |
| 3.3V Buck（USB/电池供电） | TLV62084DSGR | 最大 2 A |
| 电源优先级切换 | TPS2116DRLR | 优先 USB |
| 锂电池充电 | TP4057 | 充电电流 400 mA |
| 电流分流器 | 1 mΩ / 5W 四端开尔文 | 位于下板，信号 ISENSE+/ISENSE- |
| 电压分压（ADC 通道） | R33 100kΩ / R39 1kΩ（±0.1%） | 101:1 |
| ADC 时钟源 | 8.192 MHz 有源晶振 | 备用时钟源 |

## 2. Strapping 引脚约束

复位期间的电平决定启动行为，硬件已通过上下拉保证默认值，软件不得在启动早期将其拉向非默认电平：

| GPIO | 复位默认 | 相关器件 | 风险 |
|---|---|---|---|
| IO0 | 高 | 按键 K1/BOOT | 按住该键复位会进入下载模式 |
| IO3 | 高 | LCD_D2 | LCD 侧为输入，无冲突 |
| IO45 | 低 | BL_CTR（R10 10kΩ 下拉） | 上电时决定 VDD_SPI，须保持低 |
| IO46 | 高 | TP_INT | 触摸控制器输出，不影响启动模式 |

## 3. 未使用 / 不可用引脚

| GPIO | 说明 |
|---|---|
| IO33 ~ IO37 | 被模组内部八线 PSRAM 占用（SPIIO4~7 / SPIDQS），**不可作普通 IO** |
| IO35 ~ IO38、IO40 ~ IO42 | 悬空 |
| IO43 (TXD0)、IO44 (RXD0) | 悬空；控制台已配置为 USB-Serial/JTAG，UART0 未使用 |

## 4. 已用引脚汇总

```
IO0  BOOT 按键      IO1  蜂鸣器        IO2  电池采样     IO3  LCD_D2
IO4  K2 按键        IO5  触摸 SCL      IO6  触摸 SDA     IO7  LCD_RST
IO8  LCD_D3         IO9  ADC MOSI      IO10 ADC MISO     IO11 ADC SCLK
IO12 ADC DRDY#      IO13 ADC RST       IO14 ADC CLK      IO15 LCD_CS
IO16 LCD_SDIO0      IO17 LCD_SDIO1     IO18 LCD_SCK      IO19 USB D-
IO20 USB D+         IO21 I2C SCL       IO39 SYS_LED      IO45 LCD 背光
IO46 触摸 INT       IO47 I2C SDA       IO48 LCD_TE       EN   CHIP_PU
```

共占用 26 个 GPIO，另加 EN、GND、3V3。

注意 IO19/IO20 是 ESP32-S3 的原生 USB-Serial/JTAG 数据线，
既是烧录口也是日志口；**调试期间不可挪作他用**。

## 5. 总线与软件资源分配

| 外设 | 建议使用的总线 / 外设 | 原因 |
|---|---|---|
| LCD（NV3030B，QSPI 4bit） | ESP32-S3 的 **LCD 外设 + `esp_lcd`**，不要用 GPIO 模拟 | 硬件加速，CPU 开销低；LVGL 有官方后端 |
| ADS131M02（SPI） | **独占 SPI2** | 测量链路不能被其他设备的事务插入打断 |
| CST816D（触摸） | I2C0（IO5/IO6） | 单独一条总线，与传感器互不阻塞 |
| AHT20 / SC7A20HTR | I2C1（IO21/IO47） | 共享总线，地址不冲突（0x38 / 0x19） |
| 蜂鸣器 / LCD 背光 | LEDC（两路通道） | 需要方波与 PWM 调光 |
| 电池采样 | ADC1 单次采样模式 | 低频（1 秒一次）读取即可 |

## 6. 中断与内存约束（软件实现时必须遵守）

| 约束 | 说明 |
|---|---|
| `ADC_DRDY#`（IO12）走中断 | 中断里只发信号量 / 任务通知，采样与换算交给任务；**中断服务函数及其调用的代码、数据必须放在内部 RAM（`IRAM_ATTR`），绝不能访问 PSRAM** |
| 禁止忙等 | 任务看门狗已启用（5 秒）且检查两个核的 IDLE 任务，忙等会饿死 IDLE 导致复位 |
| 大缓冲放 PSRAM | LVGL 的显示缓冲（双缓冲约 240×284×2×2 ≈ 273KB）应放 PSRAM，避免占满内部 RAM |
| 热数据放内部 RAM | 采样缓冲、中断相关数据放内部 RAM，保证访问时序确定 |
