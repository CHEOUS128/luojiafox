@echo off
chcp 936 >nul
cd /d "%~dp0"
rem 不写网络抓包，用来和正常启动做 A/B 对比：如果这样就不卡死，说明瓶颈在抓包写盘。
start "" luojiamaster.exe --no_capture
