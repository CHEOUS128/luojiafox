@echo off
rem ============================================================
rem  一键配置与编译：解压依赖 + CMake 配置 + Ninja 编译
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
echo   第四步：编译（Ninja Release，第一次要几分钟）
echo ============================================
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Module '%VSROOT%\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'; Enter-VsDevShell -VsInstallPath '%VSROOT%' -SkipAutomaticLocation -Arch amd64 -HostArch amd64; cmake --preset release 2>&1 | Tee-Object -FilePath 'cmake_build_log.txt'; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; Write-Host ''; Write-Host '============================================'; Write-Host '  开始编译 cmake --build --preset release'; Write-Host '============================================'; cmake --build --preset release 2>&1 | Tee-Object -FilePath 'cmake_build_log.txt' -Append; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }"

if errorlevel 1 (
    echo.
    echo [错误] 配置或编译失败，请把上面的报错发给我。
    pause
    exit /b 1
)

echo.
echo ============================================
echo   编译完成
echo ============================================
if not exist "build\release\luojiamaster.exe" (
    echo [错误] 没有找到 build\release\luojiamaster.exe
    pause
    exit /b 1
)
for %%F in ("build\release\luojiamaster.exe") do echo 产物：%%~fF
for %%F in ("build\release\luojiamaster.exe") do echo 时间：%%~tF   大小：%%~zF 字节
echo  MD5：
certutil -hashfile "build\release\luojiamaster.exe" MD5 | findstr /v ":"
echo.
echo 完整编译日志：%~dp0cmake_build_log.txt
echo 如果有 error，把这个文件整个发给我。
echo.
echo 下一步：把上面的 exe 复制到「程序」目录，覆盖旧的 luojiamaster.exe
pause
