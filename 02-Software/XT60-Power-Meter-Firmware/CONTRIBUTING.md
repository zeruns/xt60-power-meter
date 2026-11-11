# 贡献指南

感谢参与 XT60 功率计固件开发！提交 Issue / PR 前请阅读本文，保证代码风格、
分层约束与提交规范一致，降低合并成本。

## 开发环境

- **ESP-IDF v6.1**（开发与验证基线；更早版本缺少本工程使用的 Kconfig 项与私有 HAL 接口，
  不保证可构建，请以 v6.1 验证后再提交）
- Windows：Espressif 官方安装器；Linux / macOS：按官方文档安装并 `export IDF_PATH`
- 可选工具：
  - 主机单元测试：任一主机端 C 编译器（gcc / clang / tcc，Windows 可用 MSYS2 / MinGW-w64）
  - UI 模拟器：Visual Studio 2017+（含 C++ 工具链与 "CMake 工具" 组件）、Python + Pillow

## 构建与烧录

Windows 一键脚本（自动探测 `IDF_PATH`，未设置时回退到脚本内置路径）：

```powershell
.\build_fw.bat            # 构建
.\flash_fw.bat [COMx]     # 烧录，串口从第一个参数取，缺省 COM3
```

Linux / macOS：

```bash
export IDF_PATH=/path/to/esp-idf
./build.sh                # 仅构建；烧录用 idf.py -p /dev/ttyUSB0 flash
```

已加载 IDF 环境的终端也可直接 `idf.py build` / `idf.py flash`。

> 两个 Windows 脚本结束时都会打印成功/失败摘要并 `pause`；自动化场景可加 `nopause`
> 参数、设 `XT60_NO_PAUSE=1` 或 `CI=1` 跳过暂停，退出码原样透传（`tools/ci/run_ci.ps1`
> 已设置 `XT60_NO_PAUSE=1`）。

**烧录范围提醒**（详见 [docs/build_and_flash.md](docs/build_and_flash.md)）：
`idf.py flash` 会烧 5 个镜像（bootloader / 分区表 / otadata / app / storage）。
其中 `storage.bin` 装的是 SPIFFS 中的**主界面全量中文字体**，**OTA 不会更新它**——
只要改过字库（`tools/fonts/make_fonts.py`），就必须**插 USB 全量烧录**，
否则会出现"升级成功但中文仍显示方框"。只改业务代码时可用 `idf.py app-flash` 省时间。
启动页文案用的是编进 app 的极小子集（`tools/fonts/make_splash_font.py`），
改启动页文案只需重跑脚本并重新构建 app，不必全量烧录。

## UI 改动必须先跑模拟器回归

改动 `components/services/ui/` 或界面相关代码后，**先跑 PC 模拟器回归再上实机**：

```powershell
powershell -ExecutionPolicy Bypass -File tools\ui_sim\build.ps1
```

- 脚本会构建模拟器、跑全部场景并生成截图（`tools/ui_sim/out/`）；
- 场景输出中的 `[scene] !!` 告警**必须为 0**，出现即视为回归失败，需修复后再提交；
- PR 中请附上关键截图作为验证证据。

## 代码规范

- 统一 `.clang-format`（工程根目录）：**4 空格缩进、行宽 100 列**、指针紧贴变量名、
  Linux 花括号风格。Trae / VS Code 的 clangd 会自动读取，请勿使用其他格式化配置；
- 注释与文档使用中文，讲解设计取舍与 API 用法，不写空话；
- 命名与分层细则见 [docs/architecture.md §10-11](docs/architecture.md)。

## 分层约束（硬性）

- 依赖方向只能自上而下：`main → services → middleware → drivers → bsp`；
  下层禁止包含/调用上层，业务代码禁止直接调用裸外设 API（`gpio_*` / `spi_*` / `i2c_*`）；
- **引脚编号只允许出现在 `components/bsp/include/board_pins.h`**，
  其他文件一律引用 `BOARD_PIN_*` 宏；新增引脚只改该文件并更新
  [docs/hardware_pinout.md](docs/hardware_pinout.md)。

## 提交规范

遵循**中文 Conventional Commits**：

```
type(scope): 中文标题（不超过 50 字）

中文正文：说明改动动机与取舍，必要时列出影响面
```

- `type`：`feat` / `fix` / `docs` / `refactor` / `test` / `chore` / `perf` / `build` / `ci`；
- `scope`：受影响模块，如 `ui` / `power_measure` / `bsp` / `docs`；
- **原子提交**：一个提交只做一件事，方便回溯与回滚；
- 纯文档、脚本改动同样遵循上述格式。

## 提交前自查清单

1. `.\build_fw.bat`（或 `build.sh`）构建通过；
2. UI 相关改动已跑 `tools\ui_sim\build.ps1`，`[scene] !!` 告警为 0；
3. 本地一键回归通过：`powershell -ExecutionPolicy Bypass -File tools\ci\run_ci.ps1`；
4. 引脚、分层约束未被违反；
5. 提交信息为中文 Conventional Commits，且为原子提交。
