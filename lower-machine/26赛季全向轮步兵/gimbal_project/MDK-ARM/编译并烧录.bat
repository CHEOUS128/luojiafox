REM 一键编译并烧录（Keil MDK 命令行）。电控双击前先确认：
REM   1) Keil 装在 C:\Keil_v5（不是的话改下面 UV4 路径）
REM   2) J-Link 已插、主控板已上电
REM 编码：GBK + CRLF。看着乱码就是编码被改了，重新拿一份。
@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"

set UV4=C:\Keil_v5\UV4\UV4.exe
set PRJ=%~dp0standard_robot.uvprojx
set LOG=%~dp0build_log.txt

if not exist "%UV4%" (
  echo 找不到 Keil：%UV4%
  echo 改本文件里的 UV4= 那一行。
  pause
  exit /b 1
)

echo === 1/2 Rebuild ===
"%UV4%" -r "%PRJ%" -j0 -o "%LOG%"
type "%LOG%"

echo.
echo === 2/2 Download ===
"%UV4%" -f "%PRJ%" -j0 -o "%LOG%"
type "%LOG%"

echo.
echo 判读：
echo   有 0 Error(s) 且出现 Programming Done 才算成功。
echo   只有 0 Error(s) 没有 Programming Done = 没烧进去，车里跑的还是旧代码。
pause
