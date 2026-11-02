@echo off
chcp 936 >nul
cd /d "%~dp0"

rem ============================================================
rem  固定为 B 机（可选，不是必需）
rem
rem  平时不用跑这个脚本：直接双击 luojiamaster.exe 就是 A 机，
rem  要 B 机就在队友小窗标题条上点一下 [默认A] 标签。
rem
rem  本脚本只是把 队友视角-B机.ini 复制成 队友视角.ini，
rem  让这台电脑每次都以 B 机启动（不听标题条上临时点的那个）。
rem
rem  注意：先关掉正在运行的客户端，否则 ini 可能被占用。
rem ============================================================

if not exist "luojiamaster.exe" (
    echo [错误] 找不到 luojiamaster.exe，请把本脚本和 exe 放在同一个文件夹
    pause
    exit /b 1
)

tasklist /FI "IMAGENAME eq luojiamaster.exe" 2>nul | find /I "luojiamaster.exe" >nul
if not errorlevel 1 (
    echo [提示] 客户端正在运行，请先关掉它再跑本脚本
    pause
    exit /b 1
)

if not exist "队友视角-B机.ini" (
    echo [错误] 找不到 队友视角-B机.ini
    pause
    exit /b 1
)

copy /y "队友视角-B机.ini" "队友视角.ini" >nul
if errorlevel 1 (
    echo [错误] 复制失败，检查这个文件夹是否可写
    pause
    exit /b 1
)

echo [完成] 已固定为 B 机，找 10.77.0.1:47700
echo        正在启动客户端...
start "" luojiamaster.exe
