<#
.SYNOPSIS
    在 PC 上编译并运行 power_calc 的单元测试（不需要开发板）。

.DESCRIPTION
    嵌入式项目里，把"纯计算逻辑"从硬件代码中分离出来后，
    就可以在 PC 上做快速、大量的测试。本脚本负责找到可用的 C 编译器、
    编译测试程序并运行，最后把退出码透传给调用方（便于接入 CI）。

    注意：本文件必须以「UTF-8 带 BOM」保存。
    Windows PowerShell 5.1 在没有 BOM 时会按系统本地编码（中文 Windows 为 GBK）
    解析脚本文件，导致其中的中文注释与字符串变成乱码、脚本解析报错。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File run_host_tests.ps1
#>

$ErrorActionPreference = 'Stop'

# 以本脚本所在目录为基准定位源码，避免受调用时的工作目录影响
$here       = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Resolve-Path (Join-Path $here '..\..')
$calcInc    = Join-Path $projectDir 'components\services\power_measure\include'
$calcSrc    = Join-Path $projectDir 'components\services\power_measure\src\power_calc.c'
$testSrc    = Join-Path $here 'test_power_calc.c'
$outExe     = Join-Path $env:TEMP 'test_power_calc.exe'

# 按优先级寻找主机端编译器。
# 注意：ESP-IDF 自带的 xtensa 工具链是交叉编译器，生成的是 ESP32 程序，
# 不能在 PC 上运行，因此不在候选列表中。
$candidates = @('gcc', 'clang', 'tcc')
$compiler = $null
foreach ($name in $candidates) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { $compiler = $cmd.Source; break }
}

if (-not $compiler) {
    Write-Host '[跳过] 未找到主机端 C 编译器（gcc / clang / tcc）。' -ForegroundColor Yellow
    Write-Host '       安装任一即可运行本测试；例如安装 MSYS2 / MinGW-w64 后把其 bin 目录加入 PATH。'
    Write-Host '       提示：ESP-IDF 自带的 xtensa 工具链无法用于主机测试。'
    exit 0
}

Write-Host "使用编译器: $compiler"
Write-Host "包含目录  : $calcInc"

# -Wall -Wextra 打开常用告警，让潜在问题在编译期就暴露出来
& $compiler -std=c11 -Wall -Wextra -I $calcInc $testSrc $calcSrc -o $outExe
if ($LASTEXITCODE -ne 0) {
    Write-Host '[失败] 编译未通过' -ForegroundColor Red
    exit 1
}

& $outExe
$code = $LASTEXITCODE

if ($code -eq 0) {
    Write-Host '[通过] 全部用例通过' -ForegroundColor Green
} else {
    Write-Host "[失败] 存在失败用例（退出码 $code）" -ForegroundColor Red
}
exit $code
