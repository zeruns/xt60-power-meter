# XT60 功率计 · LVGL UI 模拟器一键脚本
#   构建（MSVC + VS 自带 CMake/Ninja）→ 运行场景 → 生成截图 → 转 PNG
#
# 依赖：Visual Studio 2017+（含 C++ 工具链与"CMake 工具"组件；
#       本机 VS 2022 Build Tools 已就绪）。无需额外安装。
#
# 用法：powershell -ExecutionPolicy Bypass -File build.ps1
param(
    [switch]$SkipRun  # 只构建，不运行场景
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# ---- 1. 定位 VS 工具链 ----
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Host "[ui_sim] 未找到 vswhere.exe（需要安装 Visual Studio 2017+）" -ForegroundColor Red
    exit 1
}
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    Write-Host "[ui_sim] 未找到含 C++ 工具链的 Visual Studio 安装" -ForegroundColor Red
    exit 1
}
Write-Host "[ui_sim] 工具链: $vsPath"

$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
foreach ($t in @($cmake, $ninja, $vcvars)) {
    if (-not (Test-Path $t)) {
        Write-Host "[ui_sim] 缺少组件: $t（请在 VS Installer 中勾选 CMake 工具组件）" -ForegroundColor Red
        exit 1
    }
}

# ---- 2. 注入 MSVC 环境（cl.exe 需要 PATH/INCLUDE/LIB） ----
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
    }
}

# ---- 3. 配置 + 构建 ----
$buildDir = Join-Path $here "build"
& $cmake -S $here -B $buildDir -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($SkipRun) {
    Write-Host "[ui_sim] 构建完成（已跳过运行）"
    exit 0
}

# ---- 4. 运行场景（生成 out/*.bmp） ----
$outDir = Join-Path $here "out"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
& (Join-Path $buildDir "ui_sim.exe")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 5. BMP → PNG（原尺寸 + 3 倍放大） ----
python (Join-Path $here "capture.py")
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ui_sim] PNG 转换失败（检查 Python/Pillow）" -ForegroundColor Yellow
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "[ui_sim] 完成。截图目录：$outDir" -ForegroundColor Green
Write-Host "         out/*.bmp          原始位图（284x240 / 240x284）"
Write-Host "         out/png/*.png       1x PNG（评审用）"
Write-Host "         out/png/*@3x.png    3x 放大 PNG（细节走查用）"