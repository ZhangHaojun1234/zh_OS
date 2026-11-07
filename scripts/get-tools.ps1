# zh_OS 依赖安装脚本
#
# 只做三件事:
#   1. 下载 NASM 到 tools\nasm
#   2. 检查 LLVM (clang / ld.lld / llvm-objcopy)
#   3. 检查 QEMU
#
# 不会修改系统 PATH，也不会安装到系统目录。

param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $root 'tools'
$nasmDir = Join-Path $toolsDir 'nasm'
$nasmExe = Join-Path $nasmDir 'nasm.exe'

function Write-Step([string]$text) {
    Write-Host ""
    Write-Host "  $text" -ForegroundColor Cyan
}

function Download-Nasm {
    $version = '2.16.03'
    $url = "https://www.nasm.us/pub/nasm/releasebuilds/$version/win64/nasm-$version-win64.zip"
    $zip = Join-Path $env:TEMP "nasm-$version-win64.zip"

    Write-Step "downloading NASM $version"
    Write-Host "        $url"
    if (Test-Path -LiteralPath $zip) {
        Remove-Item -LiteralPath $zip -Force
    }
    curl.exe -sSL --max-time 180 -o $zip $url
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $zip)) {
        throw "NASM download failed, install it manually and set NASM_PATH"
    }

    $extract = Join-Path $env:TEMP "nasm-extract-zh_OS"
    if (Test-Path -LiteralPath $extract) {
        Remove-Item -LiteralPath $extract -Recurse -Force
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force

    if (-not (Test-Path -LiteralPath $nasmDir)) {
        New-Item -ItemType Directory -Force -Path $nasmDir | Out-Null
    }
    $found = Get-ChildItem -LiteralPath $extract -Recurse -Filter 'nasm.exe' |
             Select-Object -First 1
    if (-not $found) {
        throw "nasm.exe not found in the downloaded archive"
    }
    Copy-Item -LiteralPath $found.FullName -Destination $nasmExe -Force
    Copy-Item -LiteralPath (Join-Path $found.DirectoryName 'ndisasm.exe') `
              -Destination (Join-Path $nasmDir 'ndisasm.exe') -Force -ErrorAction SilentlyContinue

    Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Force -or -not (Test-Path -LiteralPath $nasmExe)) {
    Download-Nasm
} else {
    Write-Step "NASM already present: $nasmExe"
}

Write-Step "checking toolchain"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'find-tools.ps1')

$json = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'find-tools.ps1') -Quiet
$tools = $json | ConvertFrom-Json

$problems = @()
if (-not $tools.clang)   { $problems += 'clang (LLVM)' }
if (-not $tools.lld)     { $problems += 'ld.lld (LLVM)' }
if (-not $tools.objcopy) { $problems += 'llvm-objcopy (LLVM)' }
if (-not $tools.nasm)    { $problems += 'nasm' }

if ($problems.Count -gt 0) {
    Write-Host ""
    Write-Host "  missing:" -ForegroundColor Yellow
    foreach ($p in $problems) { Write-Host "    - $p" }
    Write-Host ""
    Write-Host "  suggested install commands (winget):" -ForegroundColor Yellow
    Write-Host "    winget install LLVM.LLVM"
    Write-Host "    winget install SoftwareFreedomConservancy.QEMU"
    exit 1
}

if (-not $tools.qemu) {
    Write-Host ""
    Write-Host "  QEMU not found: you can still build the image," -ForegroundColor Yellow
    Write-Host "  but run_vm.bat needs it. Install with:" -ForegroundColor Yellow
    Write-Host "    winget install SoftwareFreedomConservancy.QEMU"
}

Write-Host ""
Write-Host "  toolchain ready" -ForegroundColor Green
