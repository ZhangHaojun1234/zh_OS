# zh_OS 镜像生成脚本
#
# 把引导扇区、第二阶段引导和内核平铺二进制拼成一个可启动的 raw 镜像。
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\mkimage.ps1 `
#       -Boot build\boot.bin -Stage2 build\stage2.bin -Kernel build\kernel.bin `
#       -Output build\zh_OS.img [-SizeMB 32]

param(
    [Parameter(Mandatory = $true)][string]$Boot,
    [Parameter(Mandatory = $true)][string]$Stage2,
    [Parameter(Mandatory = $true)][string]$Kernel,
    [Parameter(Mandatory = $true)][string]$Output,
    [int]$SizeMB = 32
)

$ErrorActionPreference = 'Stop'

$KERNEL_LBA = 2048
$SECTOR = 512

function Read-All([string]$path) {
    $full = (Resolve-Path -LiteralPath $path).Path
    return [System.IO.File]::ReadAllBytes($full)
}

$bootBytes = Read-All $Boot
$stage2Bytes = Read-All $Stage2
$kernelBytes = Read-All $Kernel

if ($bootBytes.Length -ne $SECTOR) {
    throw "boot.bin must be exactly 512 bytes, got $($bootBytes.Length)"
}
if ($stage2Bytes.Length -gt (2048 * $SECTOR)) {
    throw "stage2.bin is larger than 1 MiB"
}

$outFull = [System.IO.Path]::GetFullPath($Output)
$outDir = [System.IO.Path]::GetDirectoryName($outFull)
if (-not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

$sizeBytes = [int64]$SizeMB * 1MB
$kernelOffset = [int64]$KERNEL_LBA * $SECTOR
if ($stage2Bytes.Length + $SECTOR -gt $kernelOffset) {
    throw "stage2 overlaps the kernel area"
}
if ($kernelOffset + $kernelBytes.Length -gt $sizeBytes) {
    throw "kernel does not fit in a ${SizeMB} MiB image"
}

$stream = [System.IO.File]::Open($outFull, 'Create', 'ReadWrite')
try {
    $stream.SetLength($sizeBytes)
    $stream.Seek(0, 'Begin') | Out-Null
    $stream.Write($bootBytes, 0, $bootBytes.Length)

    $stream.Seek($SECTOR, 'Begin') | Out-Null
    $stream.Write($stage2Bytes, 0, $stage2Bytes.Length)

    $stream.Seek($kernelOffset, 'Begin') | Out-Null
    $stream.Write($kernelBytes, 0, $kernelBytes.Length)
}
finally {
    $stream.Close()
}

Write-Host ("  image    : {0}" -f $outFull)
Write-Host ("  size     : {0} MiB" -f $SizeMB)
Write-Host ("  boot     : 0x00000000 ({0} bytes)" -f $bootBytes.Length)
Write-Host ("  stage2   : 0x00000200 ({0} bytes)" -f $stage2Bytes.Length)
Write-Host ("  kernel   : 0x{0:X8} ({1} bytes)" -f $kernelOffset, $kernelBytes.Length)
