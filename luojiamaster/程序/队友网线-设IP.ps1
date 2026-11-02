# 把接队友网线的那块网卡设成固定地址（四台电脑标准摆法：2 官方端 + 2 客户端）
#   A 机 -> 10.77.0.1    B 机 -> 10.77.0.2   掩码 255.255.255.0，网关留空
#
# 由 设置-队友网线.bat 调用，也可以自己跑：
#   powershell -NoProfile -ExecutionPolicy Bypass -File 队友网线-设IP.ps1 -Role A
#   powershell -NoProfile -ExecutionPolicy Bypass -File 队友网线-设IP.ps1 -Role B -Name "以太网 3"
#
# 自动挑网卡规则（按顺序排除）：
#   1. 不是有线网卡（Wi-Fi、蓝牙、虚拟网卡）的不算
#   2. 已经带 192.168.12.x 地址的不算 —— 那是官方链路网卡
#   3. 有默认网关、或者已经带 192.168.x.x 地址的不算 —— 那是接路由器的网卡
#   4. 剩下的必须恰好一块；不唯一就列出来，让人用 -Name 指定
param(
    [ValidateSet("A", "B")][string]$Role = "A",
    [string]$Name = ""
)

$ErrorActionPreference = "Stop"
$want = if ($Role -eq "B") { "10.77.0.2" } else { "10.77.0.1" }
$peer = if ($Role -eq "B") { "10.77.0.1" } else { "10.77.0.2" }

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[失败] 需要管理员权限。右键 设置-队友网线.bat -> 以管理员身份运行。" -ForegroundColor Red
    exit 1
}

if (-not $Name) {
    Write-Host "正在找接队友网线的那块有线网卡..."
    $adapters = @(Get-NetAdapter -Physical | Where-Object {
        $_.MediaType -eq "802.3" -and $_.Status -eq "Up" -and $_.InterfaceDescription -notmatch "Wi-?Fi|Wireless"
    })
    if ($adapters.Count -eq 0) {
        Write-Host "[失败] 一块已连接的有线网卡都没有。先插好线、等网卡灯亮。" -ForegroundColor Red
        exit 1
    }
    $candidates = @()
    foreach ($a in $adapters) {
        $ips = @(Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
                Where-Object { $_.IPAddress -notlike "169.254.*" })
        $hasOfficial = $false
        $hasOther = $false
        foreach ($ip in $ips) {
            if ($ip.IPAddress -like "192.168.12.*") { $hasOfficial = $true }
            elseif ($ip.IPAddress -notlike "10.77.*") { $hasOther = $true }
        }
        if ($hasOfficial) {
            Write-Host ("  跳过 {0}（有 192.168.12.x，是官方链路那块）" -f $a.Name)
            continue
        }
        if ($hasOther) {
            Write-Host ("  跳过 {0}（有 192.168.x 之类的地址，是接路由器那块）" -f $a.Name)
            continue
        }
        $candidates += , $a
    }
    if ($candidates.Count -eq 0) {
        Write-Host "[失败] 没有剩下的网卡了：官方链路和上网网卡都被排除了。" -ForegroundColor Red
        Write-Host "        想手动指定就带上名字再跑，例如：-Name \"以太网 3\""
        exit 1
    }
    if ($candidates.Count -gt 1) {
        Write-Host "[失败] 找到不止一块候选网卡，分不清哪块接的是队友网线：" -ForegroundColor Red
        foreach ($c in $candidates) { Write-Host ("    {0}   （{1}）" -f $c.Name, $c.InterfaceDescription) }
        Write-Host "        先跑 ipconfig 看哪块没有 192.168.12.x，再带上名字重新运行："
        Write-Host "        设置-队友网线.bat $Role `"以太网 3`""
        exit 1
    }
    $Name = $candidates[0].Name
}

Write-Host ""
Write-Host ("本机角色：{0}" -f $Role)
Write-Host ("队友网线网卡：{0}" -f $Name)
Write-Host ("即将设置：{0} / 255.255.255.0 / 网关空" -f $want)
Write-Host ""

$adapter = Get-NetAdapter -Name $Name -ErrorAction SilentlyContinue
if (-not $adapter) {
    Write-Host ("[失败] 找不到叫「{0}」的网卡。跑 ipconfig 核对名字。" -f $Name) -ForegroundColor Red
    exit 1
}

# 先清掉这块网卡上旧的 IPv4 地址 / 旧网关，再设新的。
# 用 Remove-NetRoute 之前要带上 -Confirm:$false，否则在无人值守时会卡在交互确认。
Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
Get-NetRoute -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Remove-NetRoute -Confirm:$false -ErrorAction SilentlyContinue
New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress $want -PrefixLength 24 |
        Out-Null

Write-Host "===== 设置结果 ====="
Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 |
        Select-Object IPAddress, PrefixLength, InterfaceAlias | Format-Table -AutoSize

Write-Host "===== ping 队友（现在多半还不通，对面设好之后必须通）====="
Test-Connection -ComputerName $peer -Count 3 -ErrorAction SilentlyContinue |
        Format-Table -AutoSize

Write-Host "============================================================"
Write-Host " 完成。下一步："
Write-Host ("   1. 另一台电脑同样跑一次，角色反过来：设置-队友网线.bat {0}" -f $(if ($Role -eq "A") { "B" } else { "A" }))
Write-Host ("   2. 两边都设完，确认 ping {0} 通" -f $peer)
Write-Host "   3. 本机 A 直接双击 luojiamaster.exe；"
Write-Host "      本机 B 双击后点小窗标题条的 [默认A] 变成 [B]（会记住）"
Write-Host "============================================================"
exit 0
