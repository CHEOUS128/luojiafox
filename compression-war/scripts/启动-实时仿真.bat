@echo off
chcp 936 >nul
cd /d "%~dp0.."
if not exist "cw_live.exe" (
    echo [错误] 请先运行 scripts\编译-实时仿真.bat
    pause
    exit /b 1
)
if "%~1"=="" (
    "cw_live.exe"
) else (
    "cw_live.exe" --input "%~1"
)
if errorlevel 1 pause
