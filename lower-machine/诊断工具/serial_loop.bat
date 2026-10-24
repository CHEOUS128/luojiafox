@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"
if "%~1"=="" goto bye
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0listen0310.ps1" -Port %* -Loop
goto end
:bye
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0listen0310.ps1"
echo.
echo   这个程序要带参数运行：serial_loop.bat COM7
echo   （COM 号看上面列表里 CH340 那个）
:end
echo.
pause
