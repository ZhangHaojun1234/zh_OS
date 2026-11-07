@echo off
setlocal
title zh_OS - Make Image

rem ============================================================
rem  zh_OS one-click image builder
rem
rem  Steps:
rem    1. check toolchain (NASM / clang / ld.lld / llvm-objcopy)
rem    2. build bootloader and kernel
rem    3. write build\zh_OS.img   (raw disk image, bootable)
rem    4. write build\zh_OS.vmdk  (open directly in VMware/VirtualBox)
rem
rem  Usage:
rem    make_image.bat            build the image
rem    make_image.bat debug      build with -O0 -g
rem    make_image.bat clean      remove the build directory
rem ============================================================

cd /d "%~dp0"

set "CONFIG=%~1"
if /I "%CONFIG%"=="clean" goto :clean
if "%CONFIG%"=="" set "CONFIG=release"

echo.
echo ============================================================
echo   zh_OS image builder
echo ============================================================

where powershell >nul 2>nul
if errorlevel 1 goto :no_powershell

rem ---------- toolchain ----------
echo.
echo [1/4] checking toolchain ...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\check-tools.ps1"
if errorlevel 1 goto :prepare_tools
goto :build

:prepare_tools
echo.
echo   trying to prepare the toolchain automatically ...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\get-tools.ps1"
if errorlevel 1 goto :fail
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\check-tools.ps1"
if errorlevel 1 goto :fail

:build
rem ---------- compile ----------
echo.
echo [2/4] building bootloader and kernel ...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\build.ps1" -Config "%CONFIG%"
if errorlevel 1 goto :fail

rem ---------- raw image ----------
echo.
echo [3/4] packing raw disk image ...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\mkimage.ps1" -Boot "build\boot.bin" -Stage2 "build\stage2.bin" -Kernel "build\kernel.bin" -Output "build\zh_OS.img" -SizeMB 32
if errorlevel 1 goto :fail

rem ---------- vdi / vmdk ----------
echo.
echo [4/4] creating VirtualBox and VMware disks ...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\mkdisks.ps1"
if not exist "build\zh_OS.vdi" (
    echo   note: qemu-img not available, VDI/VMDK skipped.
    echo         build\zh_OS.img works with QEMU only.
)

echo.
echo ============================================================
echo   done
echo ============================================================
echo.
echo   raw image : build\zh_OS.img
if exist "build\zh_OS.vdi"  echo   virtualbox: build\zh_OS.vdi
if exist "build\zh_OS.vmdk" echo   vmware    : build\zh_OS.vmdk
echo.
echo   next step : run_vm.bat
echo.
goto :eof

:clean
echo.
echo removing build directory ...
if exist "build" rmdir /s /q "build"
echo done.
goto :eof

:no_powershell
echo [error] PowerShell was not found. Windows 10/11 is required.
goto :fail

:fail
echo.
echo ============================================================
echo   build failed, see the messages above
echo ============================================================
echo.
echo   common causes:
echo     - LLVM (clang / ld.lld) is not installed
echo     - NASM is not installed
echo     - the project path contains unusual characters
echo.
echo   install the tools:
echo     powershell -ExecutionPolicy Bypass -File scripts\get-tools.ps1
echo.
exit /b 1
