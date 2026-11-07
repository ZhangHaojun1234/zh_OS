@echo off
setlocal
title zh_OS - Build

rem ============================================================
rem  zh_OS compile only (no disk image)
rem
rem  Use this when you only want build\kernel.elf / kernel.bin
rem  for debugging with gdb or objdump. Most users want
rem  make_image.bat instead.
rem
rem  Usage:
rem    build.bat            release build (-O2)
rem    build.bat debug      debug build (-O0 -g)
rem    build.bat clean      remove the build directory
rem ============================================================

cd /d "%~dp0"

set "CONFIG=%~1"
if /I "%CONFIG%"=="clean" goto :clean
if "%CONFIG%"=="" set "CONFIG=release"

where powershell >nul 2>nul
if errorlevel 1 (
    echo [error] PowerShell was not found. Windows 10/11 is required.
    exit /b 1
)

echo.
echo ============================================================
echo   zh_OS build
echo ============================================================

powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\check-tools.ps1"
if errorlevel 1 (
    echo.
    echo   preparing the toolchain ...
    powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\get-tools.ps1"
    if errorlevel 1 exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\build.ps1" -Config "%CONFIG%"
if errorlevel 1 (
    echo.
    echo [error] build failed
    exit /b 1
)

echo.
echo ============================================================
echo   build finished
echo ============================================================
echo.
echo   kernel.elf : build\kernel.elf
echo   kernel.bin : build\kernel.bin
echo.
goto :eof

:clean
if exist "build" rmdir /s /q "build"
echo build directory removed.
goto :eof
