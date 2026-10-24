@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"
set ARG=%~1
if "%ARG%"=="" set ARG=ports
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0listen0310.ps1" -Port %ARG% %2 %3 %4 %5 %6
echo.
pause
