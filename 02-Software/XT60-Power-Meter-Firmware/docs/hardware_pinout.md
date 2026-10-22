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

### 量程与告警上限（v0.6.9 校正）

| 项 | 设计值 | 依据 |
|---|---|---|
| 电压测量 | **0~90V** | 101:1 分压链路理论满量程 ±121V，但辅助电源最高输入 100V，故限制 90V |
| 电流峰值 | **±75A** | 1mΩ 分流器 + PGA 16x 的 ADS131M02 满量程 |
| 电流额定 | **±30A** | 5W 分流器功率上限 70A；XT60 接口额定 30A（换 XT90 底板可提高到 70A） |
| 告警阈值上限 | **90V / 70A / 6300W** | 与上述量程对齐（设备端 UI、网页/API 同一组上限） |

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
| LCD（NV3030B，QSPI 4bit） | ESP32-S3 的 **LCD 外设 + `esp_lcd`（占用 SPI2）**，不要用 GPIO 模拟 | 硬件加速，CPU 开销低；LVGL 有官方后端 |
| ADS131M02（SPI） | **独占 SPI3** | 测量链路不能被其他设备的事务插入打断。**不是 SPI2**：SPI2 已被 LCD QSPI 占用，而 ESP32-S3 的一个 SPI 外设同一时刻只能映射一组引脚（LCD 在 IO15~18、ADC 在 IO9~12，无法共存）；「独占」的本意是"不被打断"，不是字面上的外设编号 |
| CST816D（触摸） | I2C0（IO5/IO6） | 单独一条总线，与传感器互不阻塞 |
| AHT20 / SC7A20HTR | I2C1（IO21/IO47） | 共享总线，地址不冲突（0x38 / 0x19） |
| 蜂鸣器 / LCD 背光 | LEDC（两路通道） | 需要方波与 PWM 调光 |
| 电池采样 | ADC1 单次采样模式 | 低频（1 秒一次）读取即可 |

### 显示子系统（LCD + 背光）注意事项

| 项 | 说明 |
|---|---|
| 驱动 IC 命名 | 模组规格书叫 **NV3030B**，厂商官方 ESP32 例程用 `espressif/esp_lcd_spd2010` 组件驱动它，例程里另有一份名为 tk0134 的自写驱动 —— 三者是同一块屏的不同叫法/批次。本工程代码目录保留 `lcd_nv3030b` |
| **初始化序列必须用厂商验证版** | ⚠ 注册表 `espressif/esp_lcd_spd2010` **1.0.2 自带的默认初始化表对本模组不适用**（约 200 条分页式序列），直接用会**满屏花屏**。厂商随模组资料提供的同组件版本已把该默认表整段注释，替换为一套更短的序列（以 `{0xFF,0x20,0x10,0x10}` 开头，与 STM32 例程 `Lcd_Initialize()` 一致）。本工程通过 `spd2010_vendor_config_t.init_cmds` 注入该序列（见 `lcd_nv3030b.c` 的 `s_lcd_init_cmds`），**不要删、不要让组件用回默认表** |
| QSPI 命令封装 | 线上 32bit 命令字必须是 `(opcode << 24) \| (cmd << 8)`：写命令 `0x02`、写显存 `0x32`。**`esp_lcd` 不会替你补 opcode**，直接发 8bit 命令面板会全部忽略（现象：背光正常但整屏黑）。opcode 封装由 spd2010 组件完成 |
| QSPI 数据线 | `spi_bus_config_t` 里 `quadwp/quadhd` 与 `data2/data3` 共用匿名联合，只能写一组，否则后写的会把前面的覆盖成 -1（等于只配 2 根数据线） |
| 方向（MADCTL 0x36） | NV3030B 手册 7.1.25 为**标准 MIPI 位**：MY=0x80 / MX=0x40 / MV=0x20 / BGR=0x08（TFT_eSPI 官方 NV3030B 驱动同此）。竖屏 0x08；**横屏(rotation 1)=MX\|MV\|BGR=0x68**，本工程在厂商序列末尾直写 0x36=0x68。⚠ 只写 0x28(无 MX) 会镜像错位、横条变竖条。不要用组件 swap_xy()（硬编码不支持）。240×284 玻璃在 240×320 RAM 内 gap=0 |
| 控制器 vs 玻璃 | NV3030B 控制器 240RGB×320，本模组玻璃 240×284（用其中 284 行）；240×284 无需 CGRAM 偏移（TFT_eSPI 仅在 240×280 时加 20 偏移） |
| 色彩 / 像素字节序 | 面板线上要求每像素**高字节在前（big-endian）**（厂商 STM32 例程先发 `Color>>8`）。LVGL 显示格式须配 **`LV_COLOR_FORMAT_RGB565_SWAPPED`**（`CONFIG_LV_COLOR_FORMAT_RGB565_SWAPPED=y`），让 LVGL 直接渲染成 big-endian；**不要再做软件字节交换**（否则交换两次反而错色）。用小端 RGB565 会整体偏色、列错位 |
| 背光极性 | IO45 经 Q3 低边驱动，**高电平点亮**（占空比越大越亮）；IO45 是 strapping 脚，复位期间须为低（板上有 R10 10kΩ 下拉），故驱动初始化全程保持背光灭，等面板初始化并刷过清屏色后再点亮，避免"上电先白屏" |
| 反显 | 初始化序列已含 `0x21(INVON)`，代码里**不要再**调 `esp_lcd_panel_invert_color()` |
| 触摸坐标映射 | 触摸 IC 输出**物理玻璃坐标**（px∈0..239, py∈0..283），不随显示方向变化。每个旋转档位配一组「交换/翻转」参数（`lvgl_port.c` 的 `s_touch_map[]`）：`x0=swap?py:px`、`y0=swap?px:py`、`lx=flip_x?(宽-1-x0):x0`、`ly=flip_y?(高-1-y0):y0`。**ROT_90 三项为 `T/T/T`，是上板实测校正值**（曾因未翻 X 导致左右镜像）。换档位方向不对时只改这张表，不要动 MADCTL |
| 旋转切换 | 统一入口 `lvgl_port_set_rotation(rot)`：内部依次 ① 写 MADCTL（`lcd_nv3030b_set_rotation`）② 更新触摸映射档位 ③ 更新 LVGL 显示分辨率。档位与 MADCTL：ROT_0=0x08 竖屏 / ROT_90=0x68 横屏(默认) / ROT_180=0xC8 / ROT_270=0xA8。供后续 IMU 自动转屏复用 |
| QSPI 时钟 | 写显存时钟 40MHz（厂商例程为 20MHz，本工程提速一倍）。若上板花屏/丢像素，依次降回 20/10MHz 定位 |
| 中文字体 | 用 LVGL 内置**思源黑体** `LV_FONT_SOURCE_HAN_SANS_SC_16_CJK`（标题）/`_14_CJK`（副标题）。工程无 node 工具链，无法离线生成自定义字号中文，故只用内置两级 |
| 撕裂同步 | IO48 为 LCD_TE，当前驱动未使用。LVGL 适配层走的是「异步传输 + 传输完成回调」：LVGL 提交一块区域后不再复用绘图缓冲，直到传输真正完成（见 `lcd_nv3030b_register_flush_done_cb()`） |

## 6. 中断与内存约束（软件实现时必须遵守）

| 约束 | 说明 |
|---|---|
| `ADC_DRDY#`（IO12）走中断 | 中断里只发信号量 / 任务通知，采样与换算交给任务；**中断服务函数及其调用的代码、数据必须放在内部 RAM（`IRAM_ATTR`），绝不能访问 PSRAM** |
| 禁止忙等 | 任务看门狗已启用（5 秒）且检查两个核的 IDLE 任务，忙等会饿死 IDLE 导致复位 |
| 大缓冲放 PSRAM | LVGL 的显示缓冲（双缓冲约 240×284×2×2 ≈ 273KB）应放 PSRAM，避免占满内部 RAM |
| 热数据放内部 RAM | 采样缓冲、中断相关数据放内部 RAM，保证访问时序确定 |

### ADS131M02 的帧同步要求（重要，寄存器访问出问题先看这条）

`DRDY#` 不只是"数据就绪"标志，它**界定了 SPI 帧边界**：

- 一帧 = `[STATUS][CH0][CH1]` = 3 个 24bit 字 = 9 字节；
- 主机必须在**每个 DRDY 下降沿之后时钟恰好一帧**；
- 命令字放在**帧的首字**，其响应出现在**下一帧的首字**（TI 手册：
  the device outputs the register contents in the command response word of the following frame）。

因此寄存器读写是「等 DRDY → 时钟一帧发命令」+「等 DRDY → 时钟一帧读响应」两步。

**实测教训**：曾不等 DRDY、一次性时钟两帧就想读完响应，结果帧边界漂移，
把**转换数据**当成了寄存器值——症状是回读值随每次上电都不同
（GAIN1 读到 `0x0013`/`0x40A6`/`0x00A6`，器件 ID 读到 `0x2818`/`0x333E`）。
改成 DRDY 同步后，`WREG` 应答 `0x4200`、`GAIN1` 回读 `0x0040`、
器件 ID 读回 `0x2205`（与手册一致）三者全部正确。

**通信锚点**：优先看**器件 ID 寄存器（0x00）**——只读、值固定（典型 `0x2205`），
能读回即说明 SPI 模式、字序、DRDY 帧同步都正常。
RESET 命令的响应（理论 `0xFF22`）实测为命令回显，**不要**拿它做判定。

