@echo off
setlocal
cd /d "%~dp0"
set PORT=%~1
if "%PORT%"=="" set PORT=COM7
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0selfcheck.ps1" -Port %PORT%
echo.
pause
