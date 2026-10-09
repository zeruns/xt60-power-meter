<#
.SYNOPSIS
    在 PC 上编译并运行全部主机单元测试（不需要开发板）。

.DESCRIPTION
    嵌入式项目里，把"纯逻辑"从硬件代码中分离出来后，就可以在 PC 上做快速、
    大量的测试。本脚本负责找到可用的 C 编译器，逐个编译并运行 test_*.c
    测试程序，任一失败则以非零码退出（便于接入 CI）。

    当前测试套件：
      · test_power_calc.c —— 功率换算纯逻辑（power_calc.c）
      · test_button_fsm.c —— 按键状态机纯逻辑（button_fsm.c）

    注意：本文件必须以「UTF-8 带 BOM」保存。Windows PowerShell 5.1 在没有
    BOM 时会按系统本地编码（中文 Windows 为 GBK）解析，导致中文乱码/报错。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File run_host_tests.ps1
#>

$ErrorActionPreference = 'Stop'

$here       = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Resolve-Path (Join-Path $here '..\..')

# 按优先级寻找主机端编译器（ESP-IDF 的 xtensa 工具链是交叉编译器，不在候选内）
$candidates = @('gcc', 'clang', 'tcc')
$compiler = $null
foreach ($name in $candidates) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { $compiler = $cmd.Source; break }
}

if (-not $compiler) {
    Write-Host '[跳过] 未找到主机端 C 编译器（gcc / clang / tcc）。' -ForegroundColor Yellow
    Write-Host '       安装任一即可运行本测试；例如 MSYS2 / MinGW-w64。'
    exit 0
}

# 测试套件表：名称 → 测试文件 + 源文件 + 额外包含目录
$driversInc = Join-Path $projectDir 'components\drivers\button\include'
$calcInc    = Join-Path $projectDir 'components\services\power_measure\include'
$fsmSrc     = Join-Path $projectDir 'components\drivers\button\src\button_fsm.c'
$calcSrc    = Join-Path $projectDir 'components\services\power_measure\src\power_calc.c'

$suites = @(
    @{
        Name    = 'power_calc'
        TestSrc = Join-Path $here 'test_power_calc.c'
        Srcs    = @($calcSrc)
        Incs    = @($calcInc)
    },
    @{
        Name    = 'button_fsm'
        TestSrc = Join-Path $here 'test_button_fsm.c'
        Srcs    = @($fsmSrc)
        Incs    = @($driversInc)
    }
)

$failedSuites = 0
foreach ($suite in $suites) {
    Write-Host ''
    Write-Host ("==== 测试套件: {0} ====" -f $suite.Name) -ForegroundColor Cyan
    Write-Host "使用编译器: $compiler"

    $outExe = Join-Path $env:TEMP ("test_{0}.exe" -f $suite.Name)
    $args = @('-std=c11', '-Wall', '-Wextra')
    foreach ($inc in $suite.Incs) { $args += @('-I', $inc) }
    $args += $suite.TestSrc
    $args += $suite.Srcs
    $args += @('-o', $outExe)

    & $compiler @args
    if ($LASTEXITCODE -ne 0) {
        Write-Host ('[失败] {0} 编译未通过' -f $suite.Name) -ForegroundColor Red
        $failedSuites++
        continue
    }

    & $outExe
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("[失败] {0} 存在失败用例（退出码 {1}）" -f $suite.Name, $LASTEXITCODE) -ForegroundColor Red
        $failedSuites++
    }
}

Write-Host ''
if ($failedSuites -eq 0) {
    Write-Host '[通过] 全部主机测试套件通过' -ForegroundColor Green
    exit 0
}
Write-Host ("[失败] {0} 个测试套件未通过" -f $failedSuites) -ForegroundColor Red
exit 1
