# zh_OS VirtualBox 一键创建脚本
#
# 创建（或更新）一台名为 zh_OS 的 VirtualBox 虚拟机并挂载 build\zh_OS.vdi。
# 默认不启动虚拟机；创建完成后请用 VirtualBox 管理器双击运行。
#
# 用法:
#   powershell -File scripts\vbox-create.ps1
#   powershell -File scripts\vbox-create.ps1 -Name MyVM -Memory 256 -Start
#   powershell -File scripts\vbox-create.ps1 -Recreate
#   powershell -File scripts\vbox-create.ps1 -Stop

param(
    [string]$Name = 'zh_OS',
    [int]$Memory = 128,
    [switch]$Start,
    [switch]$Recreate,
    [switch]$Stop
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root

# ---------- 找 VBoxManage ----------
$candidates = @(
    'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe',
    'C:\Program Files (x86)\Oracle\VirtualBox\VBoxManage.exe',
    (Join-Path $env:ProgramFiles 'Oracle\VirtualBox\VBoxManage.exe')
)
$vbox = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path -LiteralPath $c)) { $vbox = $c; break }
}
if (-not $vbox) {
    $cmd = Get-Command VBoxManage.exe -ErrorAction SilentlyContinue
    if ($cmd) { $vbox = $cmd.Source }
}
if (-not $vbox) {
    throw "VBoxManage.exe not found. Install VirtualBox first: winget install Oracle.VirtualBox"
}

$disk = Join-Path $root 'build\zh_OS.vdi'

# ---------- 辅助函数 ----------
# VBoxManage 会把进度和警告写到 stderr。PowerShell 5.1 在
# $ErrorActionPreference = 'Stop' 下会把 stderr 当成终止错误，
# 导致正常操作被误判为失败。这里在调用原生命令时局部改成
# 'Continue'，再自己检查 $LASTEXITCODE。
$script:VBoxExit = 0
function Invoke-VBox {
    $ErrorActionPreference = 'Continue'
    $output = & $vbox @args 2>&1
    $script:VBoxExit = $LASTEXITCODE
    return $output
}

function Get-VMInfo {
    $info = Invoke-VBox showvminfo $Name --machinereadable
    if ($script:VBoxExit -ne 0) { return $null }
    return @($info)
}

function Get-VMState {
    $info = Get-VMInfo
    if (-not $info) { return $null }
    foreach ($line in $info) {
        if ($line -match '^VMState="(.+)"$') { return $Matches[1] }
    }
    return $null
}

function Wait-VMPowerOff {
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $s = Get-VMState
        if (-not $s -or $s -eq 'poweroff' -or $s -eq 'aborted') { return }
        Start-Sleep -Milliseconds 300
    }
}

function Stop-VMHard {
    $s = Get-VMState
    if (-not $s) { return }
    if ($s -eq 'saved') {
        Write-Host "  discarding saved state of VM '$Name' ..."
        Invoke-VBox discardstate $Name | Out-Null
    }
    elseif ($s -ne 'poweroff' -and $s -ne 'aborted') {
        Write-Host "  stopping VM '$Name' (state: $s) ..."
        Invoke-VBox controlvm $Name poweroff | Out-Null
    }
    Wait-VMPowerOff
}

# 从 IDE 控制器上摘掉磁盘；返回是否成功。
function Detach-Disk {
    $info = Get-VMInfo
    if (-not $info) { return $false }
    foreach ($line in $info) {
        if ($line -match '^"IDE-0-0"="(.+)"$' -and $Matches[1] -ne 'none') {
            Invoke-VBox storageattach $Name --storagectl IDE --port 0 --device 0 `
                --type hdd --medium none | Out-Null
            return ($script:VBoxExit -eq 0)
        }
    }
    return $true
}

# 按路径把旧的磁盘注册信息从 VirtualBox 媒体库里移除，
# 否则重新生成的 VDI（UUID 已变）会和新文件冲突。
function Close-DiskMedium {
    $hdds = @(Invoke-VBox list hdds)
    if ($script:VBoxExit -ne 0) { return }
    $uuid = $null
    foreach ($line in $hdds) {
        if ($line -match '^UUID:\s+(\S+)') { $uuid = $Matches[1]; continue }
        if ($line -match '^Location:\s+(.+?)\s*$') {
            if ($uuid -and [string]::Equals($Matches[1], $disk,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                Invoke-VBox closemedium disk $uuid | Out-Null
                return
            }
        }
    }
}

# ---------- 检查现有虚拟机 ----------
$vmExists = @(Invoke-VBox list vms | Select-String -SimpleMatch "`"$Name`"").Count -gt 0
$state = if ($vmExists) { Get-VMState } else { $null }
$busyStates = @('running', 'paused', 'stuck', 'gurumeditation', 'teleporting', 'live-snapshotting')
$isRunning = $state -in $busyStates

# -Stop 只负责把虚拟机关掉（给 make_image.bat 释放磁盘文件），不做别的。
if ($Stop) {
    if ($vmExists) {
        Stop-VMHard
        $s = Get-VMState
        if ($s -and $s -ne 'poweroff' -and $s -ne 'aborted') {
            Write-Host "  could not stop VM '$Name' (state: $s)" -ForegroundColor Yellow
            exit 1
        }
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $disk)) {
    throw "build\zh_OS.vdi not found. Run make_image.bat first."
}

if ($vmExists) {
    if ($Recreate) {
        if ($isRunning -or $state -eq 'saved') {
            Stop-VMHard
            $state = Get-VMState
            if ($state -and $state -ne 'poweroff' -and $state -ne 'aborted') {
                throw "could not stop VM '$Name' (state: $state)"
            }
        }

        $cfg = $null
        foreach ($line in @(Get-VMInfo)) {
            if ($line -match '^CfgFile="(.+)"$') { $cfg = $Matches[1]; break }
        }

        $detached = Detach-Disk
        Close-DiskMedium

        Write-Host "  removing existing VM '$Name' ..."
        if ($detached) {
            Invoke-VBox unregistervm $Name --delete | Out-Null
        }
        else {
            Write-Host "  could not detach the disk; unregistering without deleting files ..." -ForegroundColor Yellow
            Invoke-VBox unregistervm $Name | Out-Null
            if ($cfg) {
                $cfgDir = Split-Path -Parent $cfg
                if (Test-Path -LiteralPath $cfgDir) {
                    Remove-Item -LiteralPath $cfgDir -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
        if ($script:VBoxExit -ne 0) {
            throw "failed to remove the existing VM '$Name'"
        }
        $vmExists = $false
    }
    elseif ($isRunning) {
        # 虚拟机正在运行时无法热插拔 IDE 磁盘，直接忽略更新请求，
        # 不要像以前那样报 "Controller 'IDE' does not support hot-plugging"。
        Write-Host "  VM '$Name' is already running." -ForegroundColor Yellow
        Write-Host "  The disk cannot be re-attached while the VM is running."
        Write-Host "  Close it first if you want to boot the newest image."
        Write-Host ""
        Write-Host "  VM ready: $Name (already running)" -ForegroundColor Green
        Write-Host "    disk   : build\zh_OS.vdi"
        exit 0
    }
    else {
        Write-Host "  VM '$Name' already exists."
        if ($state -eq 'saved') { Stop-VMHard }
        Write-Host "  re-attaching the newest disk image ..."
        Detach-Disk | Out-Null
        Close-DiskMedium
    }
}

if (-not $vmExists) {
    Write-Host "  creating VM '$Name' ..."
    Invoke-VBox createvm --name $Name --ostype Other --register | Out-Null
    if ($script:VBoxExit -ne 0) { throw "failed to create VM '$Name'" }
}

Write-Host "  configuring VM ..."
$out = Invoke-VBox modifyvm $Name `
    --memory $Memory `
    --vram 16 `
    --boot1 disk --boot2 none --boot3 none --boot4 none `
    --acpi off `
    --audio-enabled off `
    --usb off `
    --nic1 none `
    --graphicscontroller vboxvga
if ($script:VBoxExit -ne 0) {
    throw ("failed to configure VM '$Name': " + (($out | Out-String).Trim()))
}

# ---------- 控制器与磁盘 ----------
$info = Get-VMInfo
if (-not $info) { throw "failed to read the configuration of VM '$Name'" }

$hasIde = $false
foreach ($line in $info) {
    if ($line -match '^storagecontrollername\d+="IDE"$') { $hasIde = $true; break }
}
if (-not $hasIde) {
    Invoke-VBox storagectl $Name --name IDE --add ide --controller PIIX4 | Out-Null
    if ($script:VBoxExit -ne 0) { throw "failed to add the IDE controller" }
}

$out = Invoke-VBox storageattach $Name --storagectl IDE --port 0 --device 0 `
    --type hdd --medium $disk
if ($script:VBoxExit -ne 0) {
    throw ("failed to attach build\zh_OS.vdi: " + (($out | Out-String).Trim()))
}

# ---------- 检测 VT-x 是否可用 ----------
# 有些机器上 Windows 的 Hyper-V / VBS 占用了虚拟化层，但没有开启
# "Windows 虚拟机监控程序平台"(WHP)，VirtualBox 就拿不到 VT-x，
# 一启动就闪退并报 VERR_VMX_NO_VMX / VERR_NEM_NOT_AVAILABLE。
#
# 这里直接查系统状态，不启动虚拟机（启动探测会留下难清理的会话锁）。
#   HypervisorPresent = True  表示 Windows 已占用虚拟化层
#   HypervisorPlatform = 1    表示 WHP 已开启，VirtualBox 可以借用
function Test-VtxUsable {
    try {
        $cs = Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction Stop
        $hvPresent = [bool]$cs.HypervisorPresent
    } catch {
        return $true
    }
    if (-not $hvPresent) {
        return $true
    }
    try {
        $f = Get-CimInstance -ClassName Win32_OptionalFeature `
             -Filter "Name='HypervisorPlatform'" -ErrorAction Stop
        return ($f.InstallState -eq 1)
    } catch {
        return $false
    }
}

$engine = 'hardware (VT-x)'
if (-not (Test-VtxUsable)) {
    Write-Host ""
    Write-Host "  VT-x is not available on this host." -ForegroundColor Yellow
    Write-Host "  Hyper-V/VBS is running but the 'Windows Hypervisor Platform'"
    Write-Host "  feature is off, so VirtualBox cannot use hardware virtualization."
    Write-Host "  Zh_OS is a 32-bit kernel, so the software engine works fine,"
    Write-Host "  just slower."
    Write-Host ""
    Write-Host "  To get hardware acceleration later, run as Administrator:"
    Write-Host "    Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All"
    Write-Host "  then reboot."
    Write-Host ""
    Write-Host "  switching to the software recompiler engine ..."

    Invoke-VBox modifyvm $Name --vm-execution-engine=recompiler `
        --hwvirtex=off --paravirt-provider=none | Out-Null
    $engine = 'recompiler (software)'
}

Write-Host ""
Write-Host "  VM ready: $Name" -ForegroundColor Green
Write-Host "    disk   : build\zh_OS.vdi"
Write-Host "    memory : $Memory MiB"
Write-Host "    boot   : disk only"
Write-Host "    engine : $engine"
Write-Host ""

$finalInfo = Invoke-VBox showvminfo $Name --machinereadable
$finalInfo |
    Select-String '^(memory|boot1|graphicscontroller)=|^"IDE-0-0"' |
    ForEach-Object { Write-Host ("    " + $_.Line) }

if ($Start) {
    Write-Host ""
    Write-Host "  starting VM ..."
    $out = Invoke-VBox startvm $Name --type gui
    if ($script:VBoxExit -ne 0) {
        Write-Host "  could not start the VM automatically." -ForegroundColor Yellow
        Write-Host "  open VirtualBox Manager and start '$Name' manually."
        Write-Host ("  " + (($out | Out-String).Trim()))
    }
}
exit 0
