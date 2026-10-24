@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" goto list
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0inject0310_tool.ps1" -Port %*
goto end
:list
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0inject0310_tool.ps1" -ListPorts
echo.
echo   Next:  inject0310.bat COM3
echo   More:  inject0310.bat COM3 -Hz 1 -Seconds 30
:end
echo.
pause
