@echo off
setlocal EnableExtensions
chcp 936 >nul
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0内部\维护工具.ps1" %*
exit /b %ERRORLEVEL%
