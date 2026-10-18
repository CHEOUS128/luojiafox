@echo off
cd /d "%~dp0"
echo ============================================================
echo  RoboGo dependency unpacker (one-time)
echo  From: _archives\   To: current folder
echo ============================================================
echo.

if not exist "_archives\boost-1.91.0-1-cmake.tar.xz" (
    echo [ERROR] _archives folder missing or incomplete.
    pause
    exit /b 1
)

del /q "_archives\_speedtest.bin" 2>nul

call :unpack directxtk12       "directxtk12-may2026.tar.gz"
if errorlevel 1 goto :failed
call :unpack spdlog            "spdlog-v1.17.0.tar.gz"
if errorlevel 1 goto :failed
call :unpack async_mqtt        "async_mqtt-10.3.0.tar.gz"
if errorlevel 1 goto :failed
call :unpack protobuf          "protobuf-v35.0.tar.gz"
if errorlevel 1 goto :failed
call :unpack imgui             "imgui-v1.92.8.tar.gz"
if errorlevel 1 goto :failed
call :unpack abseil-cpp        "abseil-cpp-fe126c7.tar.gz"
if errorlevel 1 goto :failed
call :unpack readerwriterqueue "readerwriterqueue-131af2c.tar.gz"
if errorlevel 1 goto :failed
call :unpack stb               "stb-31c1ad3.tar.gz"
if errorlevel 1 goto :failed
call :unpack_boost             "boost-1.91.0-1-cmake.tar.xz"
if errorlevel 1 goto :failed

echo.
echo ============================================================
echo  ALL DONE - all dependencies unpacked.
echo ============================================================
pause
exit /b 0

:failed
echo.
echo ############################################################
echo  SOMETHING FAILED - take a screenshot of this window
echo ############################################################
pause
exit /b 1

:unpack
set "NAME=%~1"
set "FILE=%~2"
if exist "%NAME%\.unpacked" (
    echo [%NAME%] already unpacked, skip.
    exit /b 0
)
if not exist "%NAME%" mkdir "%NAME%"
echo [%NAME%] extracting ... ^(boost takes 1-3 minutes, window may look frozen^)
echo    start: %TIME%
tar -xf "_archives\%FILE%" -C "%NAME%" --strip-components=1
if errorlevel 1 (
    echo    [ERROR] extraction failed for %NAME%
    exit /b 1
)
echo done > "%NAME%\.unpacked"
echo    done:  %TIME%
exit /b 0

:unpack_boost
set "NAME=%~1"
set "FILE=%~2"
if exist "%NAME%\.unpacked" (
    echo [%NAME%] already unpacked, skip.
    exit /b 0
)
if not exist "%NAME%" mkdir "%NAME%"
echo [%NAME%] extracting ... ^(this takes 1-3 minutes, window may look frozen^)
echo    start: %TIME%
tar -xf "_archives\%FILE%" -C "%NAME%" --strip-components=1 2> "%TEMP%\rm26_boost_tar_err.txt"
if not exist "%NAME%\CMakeLists.txt" (
    echo    [ERROR] boost extraction failed - CMakeLists.txt missing
    type "%TEMP%\rm26_boost_tar_err.txt"
    exit /b 1
)
echo    note: if you saw "Can't create ...modules\ROOT\examples" above, ignore it.
echo          That is a docs-only symlink that Windows can't create - not needed for the build.
echo done > "%NAME%\.unpacked"
echo    done:  %TIME%
exit /b 0
