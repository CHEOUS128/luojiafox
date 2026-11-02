@echo off
chcp 936 >nul
setlocal EnableExtensions
rem ============================================================
rem  四台电脑摆法 · 自定义客户端现场自检
rem  在【自定义客户端】那台电脑上双击即可，不需要管理员
rem
rem  四台电脑标准摆法（两台车 + 两台官方端 + 两台客户端）：
rem    PC-A 官方端·步兵  主板 192.168.1.2    USB① 192.168.12.1 → PC-C
rem    PC-B 官方端·英雄  主板 DHCP/1.3      USB① 192.168.12.1 → PC-D
rem    PC-C 客户端·步兵  主板 192.168.12.2  USB② 10.77.0.1    ← 本机可能是它
rem    PC-D 客户端·英雄  主板 192.168.12.2  USB② 10.77.0.2    ← 也可能是它
rem  队友网线只连 PC-C 和 PC-D 的 USB②。
rem
rem  检查内容：
rem    1. 两块网卡地址在不在（192.168.12.2 + 10.77.0.x）
rem    2. 自己那条官方链路通不通（ping 192.168.12.1）
rem    3. 队友网线通不通（ping 10.77.0.1 / 10.77.0.2，对端那台必须通）
rem    4. 路由器与裁判端（192.168.1.1 / 192.168.1.2，离线下只能 [--]）
rem    5. 防火墙（TCP 47700 + UDP 3334）
rem    6. 端口（3333 MQTT / 47700 队友）
rem    7. 队友视角.ini（可选；没有文件就是默认 A 机）
rem    8. exe 的 MD5（两台客户端必须一模一样）
rem  每项打 [OK] / [--] / [!!]；只有 [!!] 是硬问题。
rem ============================================================
set "FAILED=0"
set "HERE=%~dp0"

echo.
echo ============ 自定义客户端现场自检 ============
hostname
echo.

echo ===== 1. 网卡地址 =====
ipconfig | findstr /C:"IPv4"
ipconfig | findstr /C:"192.168.12.2" >nul
if errorlevel 1 (
    echo [!!] 没找到 192.168.12.2 -- 接【自己那台】官方端的主板网卡没设对
    set "FAILED=1"
) else (
    echo [OK] 官方链路网卡 192.168.12.2 在
)
ipconfig | findstr /C:"10.77.0." >nul
if errorlevel 1 (
    echo [!!] 没找到 10.77.0.x -- 队友网线那块 USB 网卡没设 IP
    set "FAILED=1"
) else (
    echo [OK] 队友链路网卡 10.77.0.x 在
)
echo.

echo ===== 2. 官方链路 ping 192.168.12.1 =====
ping -n 3 -w 1000 192.168.12.1 | findstr /C:"TTL=" >nul
if errorlevel 1 (
    echo [!!] 不通。逐条查：自己那台官方端开机/选手端已登录、那根网线、网线两头网卡、官方端防火墙允许 3333
    set "FAILED=1"
) else (
    echo [OK] 官方链路通（自己那台官方端在线）
)
echo.

echo ===== 3. 队友网线 =====
ping -n 2 -w 1000 10.77.0.1 | findstr /C:"TTL=" >nul
if errorlevel 1 ( echo [--] ping 10.77.0.1 不通 ^(本机是 .2 时必须通^) ) else ( echo [OK] ping 10.77.0.1 通 )
ping -n 2 -w 1000 10.77.0.2 | findstr /C:"TTL=" >nul
if errorlevel 1 ( echo [--] ping 10.77.0.2 不通 ^(本机是 .1 时必须通^) ) else ( echo [OK] ping 10.77.0.2 通 )
ping -n 2 -w 1000 10.77.0.1 | findstr /C:"TTL=" >nul
if errorlevel 1 ( ping -n 2 -w 1000 10.77.0.2 | findstr /C:"TTL=" >nul )
if errorlevel 1 (
    echo [!!] 两个方向都不通 -- 队友网线/网卡/IP 有问题，队友画面一定出不来
    echo      两台客户端各只有一块 USB 网卡，确认插的是【互相连的那根】，不是官方那根
    set "FAILED=1"
) else (
    echo      ^(只要有一个方向通就行；两台客户端都在线时应该两个方向都通^)
)
echo.

echo ===== 4. 路由器 与 裁判端 =====
ipconfig | findstr /C:"192.168.1.1" >nul
if errorlevel 1 (
    ipconfig | findstr /C:"192.168.31." >nul
    if not errorlevel 1 (
        echo [!!] 看到 192.168.31.x -- 小米路由器还是出厂网段
        echo      主控固件写死服务器地址 192.168.1.2，路由器 LAN 必须改成 192.168.1.1，否则裁判系统上不了线
        set "FAILED=1"
    ) else (
        echo [--] 本机看不到 192.168.1.1 网关
        echo      客户端这台不一定要连路由器；确认 PC-A（引擎那台）能看到 192.168.1.1 就算过
    )
) else (
    echo [OK] 网关 192.168.1.1 在（路由器已改成赛场网段）
)
ping -n 2 -w 1000 192.168.1.2 | findstr /C:"TTL=" >nul
if errorlevel 1 (
    echo [--] ping 192.168.1.2 不通 ^(引擎那台没开、或者本机没接路由器; 离线练习时正常^)
) else (
    echo [OK] 裁判端 192.168.1.2 在线
)
echo.

echo ===== 5. 防火墙（客户端这两条必须放行）=====
netsh advfirewall firewall show rule name="RM Teammate Video 47700" | findstr /C:"RM Teammate Video 47700" >nul
if errorlevel 1 (
    echo [!!] 没有 TCP 47700 规则 -- 右键 防火墙-放行客户端端口.bat 以管理员运行
    set "FAILED=1"
) else (
    echo [OK] TCP 47700 已放行（队友视频）
)
netsh advfirewall firewall show rule name="RM Client UDP 3334" | findstr /C:"RM Client UDP 3334" >nul
if errorlevel 1 (
    echo [!!] 没有 UDP 3334 规则 -- 主画面可能永远黑
    set "FAILED=1"
) else (
    echo [OK] UDP 3334 已放行（官方图传）
)
echo.

echo ===== 6. 端口 =====
netstat -ano | findstr /C:":3333" | findstr /C:"LISTENING" >nul
if errorlevel 1 (
    echo [--] 3333 没有监听 -- MQTT 在【自己那台官方端】的选手端上，本机收不到是正常的
) else (
    echo [OK] 3333 在监听（本机也跑着选手端，少见但可用）
)
netstat -ano | findstr /C:":47700" | findstr /C:"LISTENING" >nul
if errorlevel 1 (
    echo [--] 47700 没有监听 -- 客户端还没开就正常；开了还这样，看 logs 里的「队友链路」
) else (
    echo [OK] 47700 在监听（队友链路已起来）
)
echo.

echo ===== 7. 队友视角.ini（可选）=====
rem 默认没有这个文件：双击 exe 就是 A 机；B 机点一下标题条的 [默认A] 标签。
if not exist "%HERE%队友视角.ini" (
    echo [OK] 没有 队友视角.ini -- 正常，默认 A 机；要 B 机就在标题条点 [默认A]
) else (
    findstr /B /C:"peer=off" /C:"peer=OFF" "%HERE%队友视角.ini" >nul
    if not errorlevel 1 (
        echo [--] 队友视角.ini 写了 peer=off -- 队友小窗被你关掉了
    ) else (
        echo [OK] 找到 队友视角.ini，内容如下（peer 必须是对方 IP，listen 两端都是 47700）
        findstr /B /C:"role=" /C:"peer=" /C:"listen=" "%HERE%队友视角.ini"
    )
)
echo.

echo ===== 8. exe =====
if exist "%HERE%luojiamaster.exe" (
    echo [OK] 找到 luojiamaster.exe
    certutil -hashfile "%HERE%luojiamaster.exe" MD5 | findstr /R /C:"^[0-9a-f][0-9a-f]*$"
    echo      两台客户端的 MD5 必须完全一样（协议版本不一样会解不出对方画面）
) else (
    echo [!!] 本目录下没有 luojiamaster.exe
    set "FAILED=1"
)
echo.
echo ============================================================
if "%FAILED%"=="1" (
    echo [结论] 有硬问题，按上面的 [!!] 逐条处理；一次只改一处再重跑。
) else (
    echo [结论] 本机该配的都配好了。队友画面还没有就去看对面那台。
)
echo ============================================================
pause
