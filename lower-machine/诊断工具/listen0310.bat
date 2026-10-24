@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0listen0310.ps1" -Port %*
echo.
pause
