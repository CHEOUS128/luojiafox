@echo off
rem ============================================================
rem  一键配置：解压依赖 + 生成构建文件
rem  用法：把本文件放到「源码」目录里再双击
rem ============================================================
chcp 936 >nul
cd /d "%~dp0"

if not exist "CMakeLists.txt" (
    echo [错误] 请在「源码」目录里运行本脚本。
    echo         当前目录：%CD%
    pause
    exit /b 1
)

echo ============================================
echo   第一步：解压第三方依赖
echo ============================================
if not exist "3rdparty_downloads\boost\.unpacked" (
    if exist "3rdparty_downloads\解压依赖.bat" (
        call "3rdparty_downloads\解压依赖.bat"
    ) else (
        echo [错误] 缺少 3rdparty_downloads\解压依赖.bat
        pause
        exit /b 1
    )
) else (
    echo 依赖已解压，跳过
)

echo.
echo ============================================
echo   第二步：建立短路径跳板（解决中文路径链接错误）
echo ============================================
set "PORTAL="
if exist "C:\rm26build\CMakeLists.txt" set "PORTAL=C:\rm26build"
if not defined PORTAL if exist "C:\Users\%USERNAME%\rm26build\CMakeLists.txt" set "PORTAL=C:\Users\%USERNAME%\rm26build"
if defined PORTAL goto :ready

set "SRC=%~dp0."
mklink /J "C:\rm26build" "%SRC%" >nul 2>nul
if exist "C:\rm26build\CMakeLists.txt" ( set "PORTAL=C:\rm26build" & goto :ready )
mklink /J "C:\Users\%USERNAME%\rm26build" "%SRC%" >nul 2>nul
if exist "C:\Users\%USERNAME%\rm26build\CMakeLists.txt" ( set "PORTAL=C:\Users\%USERNAME%\rm26build" & goto :ready )

echo [错误] 无法建立短路径跳板。
pause
exit /b 1

:ready
echo 使用短路径：%PORTAL%
cd /d "%PORTAL%"

set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSROOT%" set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\Community"
if not exist "%VSROOT%" set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Professional"
if not exist "%VSROOT%" (
    echo [错误] 找不到 Visual Studio 2022
    pause
    exit /b 1
)

echo.
echo ============================================
echo   第三步：CMake 配置
echo ============================================
powershell -NoExit -ExecutionPolicy Bypass -Command "Import-Module '%VSROOT%\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'; Enter-VsDevShell -VsInstallPath '%VSROOT%' -SkipAutomaticLocation -Arch amd64 -HostArch amd64; cmake --preset release"
