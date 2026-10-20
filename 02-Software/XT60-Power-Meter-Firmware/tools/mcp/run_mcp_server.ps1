# ESP-IDF MCP 服务器启动脚本（开发者工具，非固件代码）
#
# 作用：激活 ESP-IDF 环境后启动官方 `idf.py mcp-server`，把 IDF 的
#       build / flash / set-target / clean / create-project 能力以 MCP
#       （Model Context Protocol）标准协议暴露给 AI 客户端。
#
# 用法：在 AI 客户端 / IDE 的 MCP 配置里，把本脚本作为 stdio 命令：
#         command = powershell.exe
#         args    = -NoLogo -NoProfile -ExecutionPolicy Bypass -File "<本脚本绝对路径>"
#       也可手动运行本脚本自测（会阻塞，等待 stdio 上的 JSON-RPC 请求）。
#
# 说明：本脚本只服务于“主机侧开发流程”，不参与固件编译与运行，
#       因此不会影响设备原有功能逻辑。需要 Python `mcp` 包（本机已安装）。
# 注意：本文件须保存为「带 BOM 的 UTF-8」，否则 Windows PowerShell 5.1
#       会按本地代码页解析，中文注释将导致语法错误。

$ErrorActionPreference = 'Stop'

# 1) 载入 ESP-IDF 环境（扩展生成的 PowerShell 概要文件）。
#    屏蔽其全部输出，避免污染 MCP 的 stdout 协议流。
$idfProfile = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
if (Test-Path $idfProfile) {
    . $idfProfile *> $null
}

# 2) 默认工程目录：本脚本位于 <工程根>/tools/mcp/ 下，向上两级即工程根。
#    若外部已设置 IDF_MCP_WORKSPACE_FOLDER 则不覆盖。
if (-not $env:IDF_MCP_WORKSPACE_FOLDER) {
    $env:IDF_MCP_WORKSPACE_FOLDER = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

# 3) 启动 MCP 服务器（stdio 传输；阻塞运行，直到客户端断开连接）。
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' 'D:\Espressif\v6.1\esp-idf\tools\idf.py' mcp-server
