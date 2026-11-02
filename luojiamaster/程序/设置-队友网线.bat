@echo off
chcp 936 >nul
setlocal EnableExtensions
rem ============================================================
rem  把【接队友网线的那块网卡】设成固定地址（四台电脑标准摆法）
rem
rem    PC-C 步兵客户端 → 10.77.0.1
rem    PC-D 英雄客户端 → 10.77.0.2
rem
rem  用法：右键 → 以管理员身份运行
rem    设置-队友网线.bat              本机当 A 机，设 10.77.0.1
rem    设置-队友网线.bat B            本机当 B 机，设 10.77.0.2
rem    设置-队友网线.bat B "以太网 3"  指定网卡名字（有歧义时）
rem
rem  真正的活在 队友网线-设IP.ps1 里：自动跳过官方链路网卡（192.168.12.x），
rem  也会跳过接路由器那块（有默认网关）。网关一律留空，否则会抢默认路由。
rem
rem  建议顺序：先插官方网线、把官方网卡设成 192.168.12.2 并 ping 通 12.1，
rem  再插队友网线跑本脚本。这样脚本能靠 12.x 认出官方网卡，绝不会写错。
rem ============================================================
set "ROLE=A"
if /i "%~1"=="B" set "ROLE=B"
set "NAME=%~2"
set "PS=%~dp0队友网线-设IP.ps1"

if not exist "%PS%" (
    echo [失败] 找不到 %PS%
    echo        这个脚本和本 bat 必须在同一个目录里。
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS%" -Role %ROLE% -Name "%NAME%"
if errorlevel 1 (
    echo.
    echo [失败] 上一步没成功，把上面的报错发给我。
    pause
    exit /b 1
)
pause
