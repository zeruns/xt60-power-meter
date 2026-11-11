<#
.SYNOPSIS
    本地一键回归入口：依次执行固件构建、主机单元测试、UI 模拟器回归，并汇总结果。

.DESCRIPTION
    与 CI（.github/workflows/ci.yml）互补：
      · Linux CI 跑 idf.py build 与 gcc 主机单测（两个独立 job）；
      · 依赖 Windows 的 UI 模拟器回归只能在本脚本执行；本脚本把构建、
        主机单测、UI 回归串起来，便于本机一次跑完全部检查。
    任一步骤失败（退出码非 0，或 UI 场景输出含 "[scene] !!" 告警），
    本脚本最终以非零码退出。

    步骤：
      1. build_fw.bat                固件构建（沿用已设置的 IDF_PATH，
                                     未设置时用脚本内的默认安装路径）
      2. test\host\run_host_tests.ps1 主机端纯逻辑单元测试
      3. tools\ui_sim\build.ps1       LVGL UI 模拟器回归（要求 [scene] !! 为 0）

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\ci\run_ci.ps1
#>

$ErrorActionPreference = 'Continue'
# 批处理脚本（build_fw.bat / flash_fw.bat）执行结束默认 pause 以便双击查看结果；
# CI/自动化场景必须跳过暂停，否则回归会卡在“请按任意键继续”。
$env:XT60_NO_PAUSE = '1'
$root = Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\..')
Set-Location $root

# 回归步骤表：名称 + 调用方式（脚本路径为空表示直接跑批处理）
$steps = @(
    @{ Name = '固件构建 (idf.py build)';      Kind = 'bat'; Path = 'build_fw.bat' },
    @{ Name = '主机单元测试 (test/host)';     Kind = 'ps1'; Path = 'test\host\run_host_tests.ps1' },
    @{ Name = 'UI 模拟器回归 (tools/ui_sim)'; Kind = 'ps1'; Path = 'tools\ui_sim\build.ps1' }
)

$results = @()

foreach ($s in $steps) {
    Write-Host ''
    Write-Host ("==== {0} ====" -f $s.Name) -ForegroundColor Cyan

    $output = @()
    if ($s.Kind -eq 'bat') {
        $output = & cmd /c $s.Path 2>&1
        $code = $LASTEXITCODE
    } else {
        $output = & powershell -ExecutionPolicy Bypass -File $s.Path 2>&1
        $code = $LASTEXITCODE
    }
    $output | ForEach-Object { Write-Host $_ }

    # UI 回归的场景告警 "[scene] !!" 必须为 0（见 CONTRIBUTING.md），
    # 模拟器本身不一定以非零码退出，故在此兜底判定
    $sceneWarnings = @($output | Where-Object { "$_" -match '\[scene\] !!' })
    $failed = ($code -ne 0) -or ($sceneWarnings.Count -gt 0)

    $results += [pscustomobject]@{
        Name     = $s.Name
        ExitCode = $code
        Warns    = $sceneWarnings.Count
        Pass     = -not $failed
    }
}

Write-Host ''
Write-Host '==== 回归结果汇总 ====' -ForegroundColor Cyan
foreach ($r in $results) {
    $tag = if ($r.Pass) { 'PASS' } else { 'FAIL' }
    $color = if ($r.Pass) { 'Green' } else { 'Red' }
    $warnInfo = if ($r.Warns -gt 0) { ("（[scene] !! 告警 {0} 条）" -f $r.Warns) } else { '' }
    Write-Host ("[{0}] {1}  退出码={2}{3}" -f $tag, $r.Name, $r.ExitCode, $warnInfo) -ForegroundColor $color
}

$failedCount = @($results | Where-Object { -not $_.Pass }).Count
if ($failedCount -gt 0) {
    Write-Host ("`n回归失败：{0}/{1} 步骤未通过。" -f $failedCount, $results.Count) -ForegroundColor Red
    exit 1
}

Write-Host "`n回归全部通过。" -ForegroundColor Green
exit 0
