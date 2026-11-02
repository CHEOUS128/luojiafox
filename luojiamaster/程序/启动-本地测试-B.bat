@echo off
chcp 936 >nul
cd /d "%~dp0"
rem ============================================================
rem  本地测试：同一台电脑开两个客户端（B 实例）
rem  前提：本文件旁边有 luojiamaster.exe
rem
rem  B：视频端口 3335，队友监听 47701，连 A 的 47700
rem  两个端口必须和「启动-本地测试-A.bat」对上。
rem ============================================================
start "" luojiamaster.exe --video_port 3335 --mqtt_server tcp://127.0.0.1:3333 --teammate-peer 127.0.0.1:47700 --teammate-listen 47701
