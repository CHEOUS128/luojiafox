@echo off
rem ==========================================================================
rem  Build cw_sim.exe  (one click)
rem
rem  Usage: double-click this file, or run it from cmd.
rem
rem  This file only launches _build.ps1 next to it. All path math happens in
rem  PowerShell (it knows its own folder via PSScriptRoot), so this batch file
rem  never has to guess how many levels up the repo root is.
rem
rem  Why this file is pure ASCII:
rem    cmd.exe echoes bytes in the console code page. If the script encoding,
rem    the code page and the Windows locale do not agree, Chinese text turns
rem    into garbage and -- worse -- multi-byte characters can break the
rem    control flow (parentheses / pipes).
rem    So: this .bat is ASCII only. All Chinese messages come from the
rem    PowerShell script it calls, which prints through Console::WriteLine.
rem ==========================================================================
setlocal
chcp 936 >nul

set "BUILD_PS1=%~dp0_build.ps1"
if not exist "%BUILD_PS1%" goto :bad_dir

powershell -NoProfile -ExecutionPolicy Bypass -File "%BUILD_PS1%"
set "RC=%ERRORLEVEL%"

echo.
pause
exit /b %RC%

:bad_dir
echo [ERROR] Missing _build.ps1 next to this script.
echo         Expected: %BUILD_PS1%
echo.
pause
exit /b 1
