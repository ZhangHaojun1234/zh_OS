# zh_OS 工具链查找脚本
#
# 输出 JSON，供 build.bat / run_vm.bat 使用。
# 查找顺序: 环境变量指定的路径 -> 项目 tools 目录 -> PATH -> 常见安装目录

param(
    [switch]$Quiet
)

$ErrorActionPreference = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot

function Find-Tool {
    param(
        [string]$Name,
        [string[]]$Candidates,
        [string[]]$ExtraNames = @()
    )

    $fromEnv = [Environment]::GetEnvironmentVariable(($Name.ToUpper() + '_PATH'))
    if ($fromEnv -and (Test-Path -LiteralPath $fromEnv)) {
        return (Resolve-Path -LiteralPath $fromEnv).Path
    }

    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $expanded = $candidate.Replace('%ROOT%', $root)
        if (Test-Path -LiteralPath $expanded) {
            return (Resolve-Path -LiteralPath $expanded).Path
        }
    }

    $names = @($ExtraNames) + @("$Name.exe")
    foreach ($n in $names) {
        if ([string]::IsNullOrWhiteSpace($n)) { continue }
        $cmd = Get-Command $n -CommandType Application
        if ($cmd) {
            return $cmd.Source
        }
    }

    return $null
}

$clangCandidates = @(
    '%ROOT%\tools\llvm\bin\clang.exe',
    'C:\Program Files\LLVM\bin\clang.exe',
    'C:\Program Files (x86)\LLVM\bin\clang.exe',
    "$env:LOCALAPPDATA\Programs\LLVM\bin\clang.exe"
)
$llvmBinCandidates = @(
    'C:\Program Files\LLVM\bin',
    'C:\Program Files (x86)\LLVM\bin',
    "$env:LOCALAPPDATA\Programs\LLVM\bin",
    '%ROOT%\tools\llvm\bin'
)

$clang = Find-Tool -Name 'clang' -Candidates $clangCandidates

$llvmBin = $null
if ($clang) {
    $llvmBin = Split-Path -Parent $clang
} else {
    foreach ($c in $llvmBinCandidates) {
        $expanded = $c.Replace('%ROOT%', $root)
        if (Test-Path -LiteralPath (Join-Path $expanded 'clang.exe')) {
            $llvmBin = $expanded
            break
        }
    }
}

$ldlld = $null
$objcopy = $null
if ($llvmBin) {
    foreach ($n in @('ld.lld.exe', 'lld-link.exe')) {
        $p = Join-Path $llvmBin $n
        if (Test-Path -LiteralPath $p) { $ldlld = $p; break }
    }
    foreach ($n in @('llvm-objcopy.exe', 'objcopy.exe')) {
        $p = Join-Path $llvmBin $n
        if (Test-Path -LiteralPath $p) { $objcopy = $p; break }
    }
}

$nasm = Find-Tool -Name 'nasm' -Candidates @(
    '%ROOT%\tools\nasm\nasm.exe',
    '%ROOT%\tools\nasm-2.16.03\nasm.exe',
    'C:\Program Files\NASM\nasm.exe',
    'C:\Program Files (x86)\NASM\nasm.exe'
)

$qemuCandidates = @(
    '%ROOT%\tools\qemu\qemu-system-i386.exe',
    'C:\Program Files\qemu\qemu-system-i386.exe',
    'C:\Program Files (x86)\qemu\qemu-system-i386.exe',
    "$env:LOCALAPPDATA\Programs\qemu\qemu-system-i386.exe"
)
$qemu = Find-Tool -Name 'qemu-system-i386' -Candidates $qemuCandidates

$qemuImg = $null
if ($qemu) {
    $candidate = Join-Path (Split-Path -Parent $qemu) 'qemu-img.exe'
    if (Test-Path -LiteralPath $candidate) { $qemuImg = $candidate }
}

$result = [ordered]@{
    root       = $root
    clang      = $clang
    lld        = $ldlld
    objcopy    = $objcopy
    llvm_bin   = $llvmBin
    nasm       = $nasm
    qemu       = $qemu
    qemu_img   = $qemuImg
    ok         = [bool]($clang -and $ldlld -and $objcopy -and $nasm)
    can_run    = [bool]($clang -and $ldlld -and $objcopy -and $nasm -and $qemu)
}

if (-not $Quiet) {
    Write-Host "  clang   : $($result.clang)"
    Write-Host "  ld.lld  : $($result.lld)"
    Write-Host "  objcopy : $($result.objcopy)"
    Write-Host "  nasm    : $($result.nasm)"
    Write-Host "  qemu    : $($result.qemu)"
}

ConvertTo-Json -InputObject $result -Compress
