@echo off
chcp 936 >nul
cd /d "%~dp0.."
if not exist "cw_live.exe" (
    echo [提示] 请先运行 scripts\编译-实时仿真.bat
    pause
    exit /b 1
)
if "%~1"=="" (
    "cw_live.exe" --profile "profiles\one-320-gop20.ini" --autostart
) else (
    "cw_live.exe" --profile "profiles\one-320-gop20.ini" --input "%~1" --autostart
)
if errorlevel 1 pause
