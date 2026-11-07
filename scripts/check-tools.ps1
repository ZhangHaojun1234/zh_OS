# zh_OS 工具链检查 (供 bat 调用)
#
# 退出码: 0 = 可以构建, 1 = 缺少构建工具

$ErrorActionPreference = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
$json = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'find-tools.ps1') -Quiet
$tools = $json | ConvertFrom-Json

$missing = @()
if (-not $tools.clang)   { $missing += 'clang (LLVM)' }
if (-not $tools.lld)     { $missing += 'ld.lld (LLVM)' }
if (-not $tools.objcopy) { $missing += 'llvm-objcopy (LLVM)' }
if (-not $tools.nasm)    { $missing += 'nasm' }

if ($missing.Count -eq 0) {
    Write-Host "  clang   : $($tools.clang)"
    Write-Host "  ld.lld  : $($tools.lld)"
    Write-Host "  objcopy : $($tools.objcopy)"
    Write-Host "  nasm    : $($tools.nasm)"
    Write-Host "  qemu    : $($tools.qemu)"
    exit 0
}

Write-Host "  缺少以下工具:" -ForegroundColor Yellow
foreach ($m in $missing) { Write-Host "    - $m" }
exit 1
