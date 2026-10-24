@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" goto list
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0inject0310_h264.ps1" -Port %*
goto end
:list
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0inject0310_h264.ps1" -ListPorts
echo.
echo   Next:  inject_h264.bat COM7
echo   More:  inject_h264.bat COM7 -Hz 5 -Seconds 60
:end
echo.
pause
