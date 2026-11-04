@echo off
chcp 936 >nul
rem ============================================================
rem  放行自定义客户端要用的三个入站项：
rem    TCP 47700    队友视频（两台客户端互相转发）
rem    UDP 3334     官方图传（选手端往你这台推，防火墙不会自动放行）
rem    ICMPv4 回显  让 ping 能通（Windows 默认拦入站 ping，联调排查全靠它）
rem  只用在【自定义客户端】那台电脑上；官方端不要跑（官方端是整机关防火墙）。
rem  右键 -> 以管理员身份运行，可重复运行。
rem ============================================================
set "FAILED=0"

netsh advfirewall firewall show rule name="RM Teammate Video 47700" >nul 2>nul
if not errorlevel 1 (
    echo [跳过] TCP 47700 规则已存在
) else (
    netsh advfirewall firewall add rule name="RM Teammate Video 47700" dir=in action=allow protocol=TCP localport=47700
    if errorlevel 1 (
        echo [失败] TCP 47700 没加上，多半是没给管理员权限。
        set "FAILED=1"
    ) else (
        echo [完成] 已放行入站 TCP 47700
    )
)

netsh advfirewall firewall show rule name="RM Client UDP 3334" >nul 2>nul
if not errorlevel 1 (
    echo [跳过] UDP 3334 规则已存在
) else (
    netsh advfirewall firewall add rule name="RM Client UDP 3334" dir=in action=allow protocol=UDP localport=3334
    if errorlevel 1 (
        echo [失败] UDP 3334 没加上，多半是没给管理员权限。
        set "FAILED=1"
    ) else (
        echo [完成] 已放行入站 UDP 3334
    )
)

netsh advfirewall firewall show rule name="RM ICMP Echo" >nul 2>nul
if not errorlevel 1 (
    echo [跳过] ICMP 回显规则已存在
) else (
    netsh advfirewall firewall add rule name="RM ICMP Echo" dir=in action=allow protocol=icmpv4:8,any
    if errorlevel 1 (
        echo [失败] ICMP 回显没放行：ping 会不通，但视频链路可能照常工作。
        set "FAILED=1"
    ) else (
        echo [完成] 已放行入站 ICMP 回显（ping 能通）
    )
)

echo.
if "%FAILED%"=="1" (
    echo [结果] 有规则没加上。右键这个文件，选「以管理员身份运行」，再试一次。
    pause
    exit /b 1
)
echo [结果] 三条规则都在了。校验命令：
echo   netsh advfirewall firewall show rule name="RM Teammate Video 47700"
echo   netsh advfirewall firewall show rule name="RM Client UDP 3334"
echo   netsh advfirewall firewall show rule name="RM ICMP Echo"
pause
