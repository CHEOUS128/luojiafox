@echo off
chcp 936 >nul
cd /d "%~dp0"

rem ============================================================
rem  关闭队友视角（可选）
rem
rem  作用：把 队友视角-关闭.ini 复制成 队友视角.ini。
rem  之后客户端完全回到没这个功能的样子：不出小窗、不占 47700、
rem  不注册 K / L 键。
rem
rem  想再打开：删掉 队友视角.ini 就行（或者在客户端里点一下角色标签）。
rem ============================================================

if not exist "队友视角-关闭.ini" (
    echo [错误] 找不到 队友视角-关闭.ini
    pause
    exit /b 1
)

tasklist /FI "IMAGENAME eq luojiamaster.exe" 2>nul | find /I "luojiamaster.exe" >nul
if not errorlevel 1 (
    echo [提示] 客户端正在运行，请先关掉它再跑本脚本
    pause
    exit /b 1
)

copy /y "队友视角-关闭.ini" "队友视角.ini" >nul
if errorlevel 1 (
    echo [错误] 复制失败，检查这个文件夹是否可写
    pause
    exit /b 1
)

echo [完成] 队友视角已关闭，以后双击 luojiamaster.exe 不会出队友小窗
pause
