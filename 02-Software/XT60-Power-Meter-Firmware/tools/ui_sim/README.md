# LVGL UI 模拟器（tools/ui_sim）

用与真机**完全相同的源码与字体**在 PC 上运行、交互、截图界面（重点：设置页），
用于烧录前的 UI 走查与回归验证。**零额外依赖**（用本机 VS 2022 Build Tools 自带工具链）。

## 为什么不是 SquareLine / EEZ Studio

两者是**设计器**（只能运行它们自己生成的 UI 工程）；本工程界面是手写 LVGL C 代码，
把设置页搬到设计器等于重写一遍并放弃现有工程约定（字体注入/命名/注释）。
**源码级模拟**是唯一能保证"模拟 = 真机"的路线：

| 一致性维度 | 真机 | 模拟器 |
|---|---|---|
| LVGL 内核 | 9.6（managed_components） | 同一份源码 |
| 分辨率/色深度 | 284×240，RGB565 | 相同（含 SWAPPED 字节序） |
| 刷新方式 | PARTIAL 40 行分块 | 相同（flush 缓冲逐字节一致） |
| 中文字体 | notosc_reg/bold.ttf + tiny_ttf | 同一 TTF、同一 tiny_ttf、同一 7 档字号 |
| 界面代码 | components/services/ui/src | 同一份文件直接编译 |
| 时钟 | FreeRTOS tick 5ms | 虚拟时钟 5ms（可复现） |

## 一键使用

```powershell
# 构建 → 跑全部场景 → 生成截图 → 转 PNG
powershell -ExecutionPolicy Bypass -File tools/ui_sim/build.ps1

# 只构建
powershell -ExecutionPolicy Bypass -File tools/ui_sim/build.ps1 -SkipRun
```

产物：

```
tools/ui_sim/out/*.bmp        原始位图（284x240 横屏 / 240x284 竖屏）
tools/ui_sim/out/png/*.png    1x PNG（评审用）
tools/ui_sim/out/png/*@3x.png 3x 放大（细节走查用）
```

## 工作方式

- **引擎** `src/sim_main.c`：headless 渲染（无窗口）+ BMP 导出；
  触摸由程序化注入（按下/移动/抬起），虚拟时钟步进 ⇒ **同一场景每次结果一致**。
- **场景** `src/sim_scenes.c`：**可执行的验收流程**。按文本语义查找控件
  （`find_text`，自动跳过隐藏对象与屏幕外对象）并模拟真实触摸；
  任意步骤失败都会打印 `[scene] !!` 告警（包括"找不到控件"和"翻页未到位"）。
  当前覆盖：启动页、三页首屏、设置首页、按下态、显示设置（亮度拖动/竖屏/恢复）、
  蜂鸣器（滑条/试听）、WiFi 路线图、电能统计（确认框/清零/关框）、关于、标定向导
  （通道选择/步进/应用/完成）、竖屏测量页、首页快速菜单（打开/切刷新率/清除电能/
  关闭），共 27 张截图。
- **桩与 mock**：`include/`（模拟器版 lvgl_port / lcd_nv3030b 头）、
  `src/stubs/`（esp_err / esp_log / esp_app_desc）、
  `src/mocks/`（settings / buzzer / power_measure / data_store / 背光）。

## 回归闭环（每次改 UI 后执行）

1. 跑 `build.ps1`；
2. 看控制台：`[scene] !!` 告警应为 **0 条**（有告警 = 某交互未被命中，属回归失败）；
3. 看截图：`out/png/` 逐张走查（本次已借此发现并修复：校准页减号字形缺失、
   应用按钮被挤出首屏、返回按钮被内容容器覆盖、旋转后页面偏移等问题）。

## 依赖

Visual Studio 2017+（含 C++ 工具链与"CMake 工具"组件）。本机已验证 VS 2022
Build Tools 可用；不需要 SDL、不需要联网、不需要额外下载。

## 已知事项（模拟器与真机的边界，如实声明）

- **栈与内存池均已在真机修复**（2026-10，两次 coredump 定位）：
  ① `taskLVGL` 栈溢出（tiny_ttf/stb_truetype 首次光栅化新字形）→ 栈 7168→12288
  （见 `lvgl_port.c` 注释）；
  ② 128KB LVGL 内建池被"字形缓存 + 二级页对象"耗尽（`stbtt__new_active` 断言）
  → **LVGL 改用系统堆（CLIB）**：小块优先内部 RAM、大块走 PSRAM（见
  `sdkconfig.defaults` 的 `CONFIG_LV_USE_CLIB_MALLOC`，`.bss` 直接释放 128KB），
  同时字形缓存条数 64→32。串口巡检常驻打印 LVGL 栈水位与内部 RAM 水位，
  今后新增字号/字体集后请复查这两个值。
- **字体缺字有闭环工具**：`tools/fonts/audit_glyphs.py` 做"UI 文案字符 vs 字体
  子集"比对（当前 0 缺失）；`make_fonts.py` 已内置"扫描 UI 源码字符"，新文案
  的字会自动进子集，无需手工补字。
- **波形页图表区域起手滑动不翻页**（从标题区起手可翻页）：待真机复核。
- **旋转**：已修复"旋转后 tileview 偏移"（ui_nav 监听分辨率变化事件自动吸附）；
  竖屏版式为真实重排结果（部分页面需滚动），触摸映射表中仅横屏档经上板校正。
- 场景中的蜂鸣器/持久化等副作用只打日志（mock），不影响 PC。