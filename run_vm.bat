@echo off
setlocal EnableDelayedExpansion
title zh_OS - Run VM

rem ============================================================
rem  zh_OS one-click virtual machine launcher
rem
rem  Steps:
rem    1. build build\zh_OS.img automatically if it is missing
rem    2. locate QEMU
rem    3. boot zh_OS in QEMU
rem
rem  Usage:
rem    run_vm.bat            normal start (graphical window)
rem    run_vm.bat debug      start with CPU/interrupt logs
rem    run_vm.bat serial     serial output in this console, no window
rem    run_vm.bat build      rebuild the image, then start
rem ============================================================

cd /d "%~dp0"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=normal"

if /I "%MODE%"=="build" (
    call make_image.bat
    if errorlevel 1 exit /b 1
    set "MODE=normal"
)

echo.
echo ============================================================
echo   zh_OS virtual machine
echo ============================================================

if not exist "build\zh_OS.img" (
    echo.
    echo [1/2] build\zh_OS.img not found, building it now ...
    call make_image.bat
    if errorlevel 1 (
        echo.
        echo [error] image build failed
        exit /b 1
    )
) else (
    echo.
    echo [1/2] image: build\zh_OS.img
    for %%f in ("build\zh_OS.img") do echo         size: %%~zf bytes
)

echo.
echo [2/2] starting QEMU ...

set "QEMU="
for /f "usebackq delims=" %%q in (`powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\get-tool-path.ps1" qemu`) do set "QEMU=%%q"

if not defined QEMU (
    echo.
    echo [error] QEMU was not found.
    echo.
    echo   install it with:
    echo     winget install SoftwareFreedomConservancy.QEMU
    echo.
    echo   or download it from https://www.qemu.org/download/#windows
    echo.
    exit /b 1
)

echo         %QEMU%

if not exist "build\logs" mkdir "build\logs"
set "SERIAL=build\logs\serial.log"
if exist "%SERIAL%" del /q "%SERIAL%"

rem ---------- pick a display backend that this QEMU build supports ----------
set "DISPLAY_OPT="
"%QEMU%" -display help > "%TEMP%\zhos-display.txt" 2>&1
findstr /I /X "gtk" "%TEMP%\zhos-display.txt" >nul 2>nul
if not errorlevel 1 set "DISPLAY_OPT=-display gtk"
if not defined DISPLAY_OPT (
    findstr /I /X "sdl" "%TEMP%\zhos-display.txt" >nul 2>nul
    if not errorlevel 1 set "DISPLAY_OPT=-display sdl"
)
if not defined DISPLAY_OPT set "DISPLAY_OPT=-display default"
del /q "%TEMP%\zhos-display.txt" >nul 2>nul

set "QEMU_ARGS=-drive file=build/zh_OS.img,format=raw,if=ide,index=0 -m 128 -boot c"

if /I "%MODE%"=="debug" goto :mode_debug
if /I "%MODE%"=="serial" goto :mode_serial
goto :mode_normal

:mode_debug
echo         mode: debug (CPU and interrupt log enabled)
set "QEMU_ARGS=!QEMU_ARGS! -d int,cpu_reset,guest_errors -D build/logs/qemu-debug.log"
set "QEMU_ARGS=!QEMU_ARGS! -serial file:build/logs/serial.log"
set "QEMU_ARGS=!QEMU_ARGS! -no-reboot !DISPLAY_OPT!"
goto :launch

:mode_serial
echo         mode: serial (console only)
set "QEMU_ARGS=!QEMU_ARGS! -display none -serial stdio"
goto :launch

:mode_normal
echo         mode: normal (%DISPLAY_OPT%)
set "QEMU_ARGS=!QEMU_ARGS! -serial file:build/logs/serial.log !DISPLAY_OPT!"

:launch
echo.
echo ------------------------------------------------------------
echo   type 'help' in the kernel shell to list commands
echo   type 'halt' or close the window to exit
echo   serial log: %SERIAL%
echo ------------------------------------------------------------
echo.

"%QEMU%" !QEMU_ARGS!
set "EXITCODE=%ERRORLEVEL%"

echo.
if "%EXITCODE%"=="0" (
    echo the virtual machine exited normally.
) else (
    echo the virtual machine exited with code %EXITCODE%.
    echo check the files under build\logs if it did not boot.
)

endlocal & exit /b %EXITCODE%
