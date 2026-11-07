# zh_OS 虚拟机磁盘生成脚本
#
# 把 raw 镜像转换成各虚拟机软件能直接挂载的磁盘格式。
#
#   build\zh_OS.vdi    VirtualBox 原生格式（推荐给 VirtualBox）
#   build\zh_OS.vmdk   VMware / VirtualBox 都支持
#
# 说明: VirtualBox 无法直接挂载 raw .img（会被判定为软盘而拒绝），
#       所以必须提供 VDI 或 VMDK。
#
# 找不到 qemu-img 时安静退出，不视为构建失败。

param(
    [string]$Image = 'build\zh_OS.img'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root

$toolsJson = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'find-tools.ps1') -Quiet
$tools = $toolsJson | ConvertFrom-Json

if (-not (Test-Path -LiteralPath $Image)) {
    throw "image not found: $Image"
}

if (-not $tools.qemu_img) {
    Write-Host "  qemu-img not found, skipping VDI/VMDK"
    Write-Host "  install QEMU: winget install SoftwareFreedomConservancy.QEMU"
    exit 0
}

# 每次重新生成时先删掉旧文件，避免 VirtualBox 媒体注册表里残留旧 UUID
$targets = @(
    @{ Path = 'build\zh_OS.vdi';  Format = 'vdi';  Label = 'VirtualBox' },
    @{ Path = 'build\zh_OS.vmdk'; Format = 'vmdk'; Label = 'VMware / VirtualBox' }
)

$made = 0
foreach ($target in $targets) {
    if (Test-Path -LiteralPath $target.Path) {
        Remove-Item -LiteralPath $target.Path -Force -ErrorAction SilentlyContinue
    }

    & $tools.qemu_img convert -f raw -O $target.Format $Image $target.Path 2>$null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $target.Path)) {
        Write-Host ("  skip {0} (conversion failed)" -f $target.Path) -ForegroundColor Yellow
        continue
    }

    $mb = [math]::Round((Get-Item -LiteralPath $target.Path).Length / 1MB, 1)
    Write-Host ("  {0,-20} {1,6} MiB  ({2})" -f $target.Path, $mb, $target.Label)
    $made++
}

if ($made -eq 0) {
    Write-Host "  no virtual disk was produced" -ForegroundColor Yellow
}
exit 0
