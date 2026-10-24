@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" goto list
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0listen0310.ps1" -Port %*
goto end
:list
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0listen0310.ps1"
echo.
echo   Next:  listen0310.bat COM3
echo   More:  listen0310.bat COM3 -Seconds 60
echo   Raw :  listen0310.bat COM3 -Raw
:end
echo.
pause
