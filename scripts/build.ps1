# zh_OS 编译脚本 (由 build.bat 调用)
#
# 1. 用 NASM 汇编引导扇区、stage2 与内核入口
# 2. 用 clang 编译所有 C 源文件 (freestanding, i386)
# 3. 用 ld.lld 链接成 ELF，再用 llvm-objcopy 转成平铺二进制
#
# 注意: 所有传给外部工具的路径都使用工作目录内的相对路径 (纯 ASCII)，
#       这样即使项目所在目录包含中文或空格，工具链也能正常工作。

param(
    [string]$Config = 'release',
    [switch]$Verbose
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root

$toolsJson = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'find-tools.ps1') -Quiet
if (-not $toolsJson) {
    throw "cannot query toolchain"
}
$tools = $toolsJson | ConvertFrom-Json

$missing = @()
if (-not $tools.clang)   { $missing += 'clang' }
if (-not $tools.lld)     { $missing += 'ld.lld' }
if (-not $tools.objcopy) { $missing += 'llvm-objcopy' }
if (-not $tools.nasm)    { $missing += 'nasm' }
if ($missing.Count -gt 0) {
    throw ("missing tools: " + ($missing -join ', '))
}

if (-not (Test-Path -LiteralPath 'build\obj')) {
    New-Item -ItemType Directory -Force -Path 'build\obj' | Out-Null
}

$commonFlags = @(
    '--target=i386-none-elf',
    '-march=i386',
    '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-ffreestanding',
    '-fno-pic', '-fno-pie',
    '-fno-stack-protector',
    '-fno-asynchronous-unwind-tables',
    '-fno-unwind-tables',
    '-fno-builtin',
    '-fno-common',
    '-fno-omit-frame-pointer',
    '-std=gnu11',
    '-Wall', '-Wextra',
    '-Wno-unused-parameter',
    '-Ikernel/include'
)

if ($Config -eq 'debug') {
    $commonFlags += @('-O0', '-g', '-DDEBUG')
} else {
    $commonFlags += @('-O2', '-DNDEBUG')
}

Write-Host ""
Write-Host "  [1/4] assembling bootloader" -ForegroundColor Cyan
& $tools.nasm -f bin 'boot/boot.asm' -o 'build/boot.bin'
if ($LASTEXITCODE -ne 0) { throw "failed to assemble boot/boot.asm" }
& $tools.nasm -f bin 'boot/stage2.asm' -o 'build/stage2.bin'
if ($LASTEXITCODE -ne 0) { throw "failed to assemble boot/stage2.asm" }

& $tools.nasm -f elf32 'kernel/arch/entry.asm' -o 'build/obj/kernel_arch_entry.asm.o'
if ($LASTEXITCODE -ne 0) { throw "failed to assemble kernel/arch/entry.asm" }

Write-Host "  [2/4] compiling kernel sources" -ForegroundColor Cyan
$objects = @('build/obj/kernel_arch_entry.asm.o')
$sources = Get-ChildItem -Path 'kernel' -Recurse -Filter '*.c' | Sort-Object FullName
foreach ($src in $sources) {
    $relative = $src.FullName.Substring($root.Length + 1) -replace '\\', '/'
    $objName = ($relative -replace '[^A-Za-z0-9_.-]', '_') -replace '\.c$', '.o'
    $objPath = "build/obj/$objName"

    if ($Verbose) {
        Write-Host "        clang $relative"
    }

    & $tools.clang @commonFlags -c $relative -o $objPath
    if ($LASTEXITCODE -ne 0) {
        throw "failed to compile $relative"
    }
    $objects += $objPath
}

Write-Host ("  [3/4] linking {0} objects" -f $objects.Count) -ForegroundColor Cyan
$linkArgs = @('-T', 'kernel/linker.ld', '-o', 'build/kernel.elf', '--nostdlib', '--no-undefined') + $objects
& $tools.lld @linkArgs
if ($LASTEXITCODE -ne 0) {
    throw "link failed"
}

& $tools.objcopy -O binary 'build/kernel.elf' 'build/kernel.bin'
if ($LASTEXITCODE -ne 0) {
    throw "objcopy failed"
}

Write-Host "  [4/4] flat image summary" -ForegroundColor Cyan
Write-Host ("        kernel.bin  {0,8:N0} bytes" -f (Get-Item -LiteralPath 'build\kernel.bin').Length)
Write-Host ("        boot.bin    {0,8:N0} bytes" -f (Get-Item -LiteralPath 'build\boot.bin').Length)
Write-Host ("        stage2.bin  {0,8:N0} bytes" -f (Get-Item -LiteralPath 'build\stage2.bin').Length)

Write-Host ""
Write-Host "  build complete: build\kernel.elf, build\kernel.bin" -ForegroundColor Green
