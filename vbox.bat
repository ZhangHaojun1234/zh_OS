@echo off
setlocal
title zh_OS - VirtualBox

rem ============================================================
rem  zh_OS VirtualBox launcher
rem
rem  Creates (or updates) a VirtualBox VM named "zh_OS" that uses
rem  build\zh_OS.vdi, then opens VirtualBox Manager.
rem
rem  Usage:
rem    vbox.bat             create/update the VM and open Manager
rem    vbox.bat run         create/update the VM and start it
rem    vbox.bat recreate    delete the old VM and build a fresh one
rem
rem  Note: VirtualBox cannot boot a raw .img directly, so this
rem  script uses the .vdi produced by make_image.bat.
rem ============================================================

cd /d "%~dp0"

set "ACTION=%~1"
if /I "%ACTION%"=="recreate" goto :recreate
if /I "%ACTION%"=="run" goto :run

rem ---------- default: create/update the VM, then open Manager ----------
call :check_vbox
if errorlevel 1 goto :fail

call :ensure_image
if errorlevel 1 goto :fail

echo.
echo [2/2] creating/updating VM "zh_OS" ...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\vbox-create.ps1"
if errorlevel 1 goto :fail
goto :open_manager

rem ---------- recreate: stop VM, rebuild image, recreate VM ----------
:recreate
call :check_vbox
if errorlevel 1 goto :fail

call :prepare_recreate
if errorlevel 1 goto :fail

echo.
echo [1/2] rebuilding image ...
call make_image.bat
if errorlevel 1 goto :fail

echo.
echo [2/2] recreating VM "zh_OS" ...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\vbox-create.ps1" -Recreate
if errorlevel 1 goto :fail
goto :open_manager

rem ---------- run: create/update the VM and start it ----------
:run
call :check_vbox
if errorlevel 1 goto :fail

call :ensure_image
if errorlevel 1 goto :fail

echo.
echo [2/2] starting VM "zh_OS" ...
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\vbox-create.ps1" -Start
if errorlevel 1 goto :fail
goto :done

rem ---------- helpers ----------
:ensure_image
if exist "build\zh_OS.vdi" exit /b 0
echo.
echo [1/2] build\zh_OS.vdi not found, building it now ...
call make_image.bat
if errorlevel 1 exit /b 1
exit /b 0

:prepare_recreate
rem A running VM locks build\zh_OS.vdi, which makes make_image.bat fail
rem to replace it. Power the VM off before rebuilding the image.
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\vbox-create.ps1" -Stop
if errorlevel 1 (
    echo [error] could not stop the existing VM
    exit /b 1
)
exit /b 0

:check_vbox
if exist "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" exit /b 0
if exist "C:\Program Files (x86)\Oracle\VirtualBox\VBoxManage.exe" exit /b 0
where VBoxManage.exe >nul 2>nul
if not errorlevel 1 exit /b 0
echo.
echo [error] VirtualBox is not installed.
echo         install it with: winget install Oracle.VirtualBox
exit /b 1

:open_manager
set "VBOXGUI=C:\Program Files\Oracle\VirtualBox\VirtualBox.exe"
if not exist "%VBOXGUI%" set "VBOXGUI=C:\Program Files (x86)\Oracle\VirtualBox\VirtualBox.exe"
echo.
if exist "%VBOXGUI%" (
    echo opening VirtualBox Manager ...
    start "" "%VBOXGUI%"
    echo.
    echo select the "zh_OS" VM and click Start.
) else (
    echo VirtualBox Manager was not found; start the VM from your Start menu.
)
goto :done

:fail
echo.
echo ============================================================
echo   VirtualBox setup failed
echo ============================================================
echo.
echo   common causes:
echo     - VirtualBox is not installed: winget install Oracle.VirtualBox
echo     - the VM is running: close it first, or run "vbox.bat recreate"
echo     - a stale VM blocks the name: run "vbox.bat recreate"
echo.
pause
exit /b 1

:done
endlocal
exit /b 0
