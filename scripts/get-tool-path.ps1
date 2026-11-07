# 输出某个工具的路径 (供 bat 调用)
#
# 用法: powershell -File scripts\get-tool-path.ps1 qemu
# 找不到时输出空字符串并以退出码 0 结束，方便 bat 直接判断。

param(
    [Parameter(Mandatory = $true)][ValidateSet('qemu', 'qemu-img', 'nasm', 'clang', 'lld', 'objcopy')]
    [string]$Name
)

$ErrorActionPreference = 'SilentlyContinue'

$json = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'find-tools.ps1') -Quiet
$tools = $json | ConvertFrom-Json

$value = switch ($Name) {
    'qemu'     { $tools.qemu }
    'qemu-img' { $tools.qemu_img }
    'nasm'     { $tools.nasm }
    'clang'    { $tools.clang }
    'lld'      { $tools.lld }
    'objcopy'  { $tools.objcopy }
}

if ($value) { Write-Output $value }
exit 0
