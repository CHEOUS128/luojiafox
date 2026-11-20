@echo off
setlocal
chcp 936 >nul
cd /d "%~dp0.."
if not exist "cw_live.exe" (
    echo [提示] 请先运行 scripts\编译-实时仿真.bat
    pause
    exit /b 1
)
set "CW_DEMO_INPUT=%~1"
if not defined CW_DEMO_INPUT if not exist "素材\car_raw.bgr" if exist "素材\演示-实车图传.mp4" set "CW_DEMO_INPUT=素材\演示-实车图传.mp4"
if not defined CW_DEMO_INPUT (
    "cw_live.exe" --profile "profiles\one-320-gop20.ini" --autostart
) else (
    "cw_live.exe" --profile "profiles\one-320-gop20.ini" --input "%CW_DEMO_INPUT%" --autostart
)
if errorlevel 1 pause
