param([switch]$NoMenu)
$ErrorActionPreference = 'Stop'
$script:ClientRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$script:ProgramRoot = Join-Path $script:ClientRoot '程序'
$script:ClientExe = Join-Path $script:ProgramRoot 'luojiamaster.exe'
$script:PeerIni = Join-Path $script:ProgramRoot '队友视角.ini'

function Get-FileMd5 {
    param([string]$LiteralPath)
    $stream=$null; $algorithm=$null
    try {
        $stream=[IO.File]::Open($LiteralPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        $algorithm=[Security.Cryptography.MD5]::Create()
        [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','')
    } finally {
        if ($algorithm) { $algorithm.Dispose() }
        if ($stream) { $stream.Dispose() }
    }
}

function Get-TeammateSettings {
    param([string[]]$Lines = @())
    $role = 'A'; $peerText = ''; $listenPort = 47700
    foreach ($line in $Lines) {
        $trimmed = $line.Trim()
        if (-not $trimmed -or $trimmed.StartsWith('#')) { continue }
        $parts = $trimmed.Split(@('='), 2)
        if ($parts.Count -ne 2) { continue }
        $key = $parts[0].Trim(); $value = $parts[1].Trim()
        if (($key -ceq 'role' -or $key -ceq '角色') -and $value) {
            $first=$value.Substring(0,1).ToUpperInvariant()
            $role=if($first -in @('A','B')){$first}else{'A'}
        }
        elseif ($key -ceq 'peer') { $peerText = $value }
        elseif ($key -ceq 'listen') {
            $parsedPort = 0
            if (-not [int]::TryParse($value, [ref]$parsedPort) -or $parsedPort -lt 1 -or $parsedPort -gt 65535) { throw '队友视角.ini 的 listen 不是有效端口。' }
            $listenPort = $parsedPort
        }
    }
    if ($peerText.ToLowerInvariant() -in @('off','0')) {
        return [pscustomobject]@{ Enabled=$false; Role=$role; PeerHost=''; PeerPort=0; ListenPort=$listenPort }
    }
    if (-not $peerText) { $peerText = if ($role -eq 'B') { '10.77.0.1:47700' } else { '10.77.0.2:47700' } }
    if ($peerText -notmatch '^(?<endpoint>[^:\s]+):(?<port>\d+)$') { throw '队友地址格式不能识别，本工具不猜对端。请检查 peer=地址:端口。' }
    $endpointHost = $matches.endpoint; $endpointPort = [int]$matches.port
    if ($endpointPort -lt 1 -or $endpointPort -gt 65535) { throw '队友端口超出范围。' }
    [pscustomobject]@{ Enabled=$true; Role=$role; PeerHost=$endpointHost; PeerPort=$endpointPort; ListenPort=$listenPort }
}

function Read-TeammateSettings {
    $lines = if (Test-Path -LiteralPath $script:PeerIni) { [IO.File]::ReadAllLines($script:PeerIni, [Text.Encoding]::UTF8) } else { @() }
    Get-TeammateSettings -Lines $lines
}

function Test-AddressIsLocal {
    param([string]$Address, [string[]]$LocalAddresses)
    $parsedAddress = $null
    if (-not [Net.IPAddress]::TryParse($Address, [ref]$parsedAddress)) { throw '这里应传入解析后的IP地址。' }
    if ([Net.IPAddress]::IsLoopback($parsedAddress) -or $parsedAddress.Equals([Net.IPAddress]::Any) -or $parsedAddress.Equals([Net.IPAddress]::IPv6Any)) { return $true }
    foreach ($localAddress in $LocalAddresses) {
        $parsedLocal = $null
        if ([Net.IPAddress]::TryParse($localAddress, [ref]$parsedLocal) -and $parsedAddress.Equals($parsedLocal)) { return $true }
    }
    return $false
}

function Get-AdapterProtectionReason {
    param($Adapter, [string[]]$IPv4Addresses, [object[]]$DefaultRoutes)
    if ($Adapter.MediaType -ne '802.3' -or $Adapter.Status -ne 'Up') { return '必须选择已插线的物理以太网网卡。' }
    if (@($DefaultRoutes).Count -gt 0) { return '这块网卡有默认路由，不能用此工具修改。' }
    foreach ($address in $IPv4Addresses) {
        if ($address -like '192.168.12.*') { return '这是官方链路网卡，禁止修改。' }
        if ($address -notlike '169.254.*' -and $address -notin @('10.77.0.1','10.77.0.2')) { return '这块网卡有其他业务地址，禁止修改。' }
    }
    return ''
}

function Require-Administrator {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw '这项操作需要管理员。请关掉本工具，右键维护工具.bat，选以管理员身份运行。' }
}

function Show-LocalStatus {
    Write-Host "`n--- 本机状态（只查看，不发控制、不改设置）---"
    Get-NetAdapter -Physical | ForEach-Object {
        $adapter = $_
        $addresses = @(Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object { $_.IPAddress })
        [pscustomobject]@{ 编号=$adapter.ifIndex; 网卡=$adapter.Name; 状态=$adapter.Status; MAC=$adapter.MacAddress; IPv4=($addresses -join ', ') }
    } | Format-Table -AutoSize
    $settings = Read-TeammateSettings
    if ($settings.Enabled) { Write-Host ('当前兼容设置：{0}机；实际队友对端 {1}:{2}；本机监听 {3}' -f $settings.Role,$settings.PeerHost,$settings.PeerPort,$settings.ListenPort) }
    else { Write-Host '队友视角当前已关闭。' }
    Write-Host 'A/B是当前双客户端的人工选择，不会自动识别身份。'
    if (Test-Path -LiteralPath $script:ClientExe) { Get-Item -LiteralPath $script:ClientExe | Select-Object Name,Length,LastWriteTime | Format-Table; Write-Host ('EXE MD5：'+(Get-FileMd5 -LiteralPath $script:ClientExe)) }
    else { Write-Host '[缺少] 程序/luojiamaster.exe' -ForegroundColor Red }
    try {
        $names = @('RM26.Client.Teammate.TCP','RM26.Client.Official.UDP','RM26.Client.ICMP')
        $legacyNames = @('RM Teammate Video 47700','RM Client UDP 3334','RM ICMP Echo')
        $rules = @(Get-NetFirewallRule | Where-Object { $_.Name -in $names -or $_.DisplayName -in $legacyNames })
        if ($rules.Count) { $rules | Select-Object DisplayName,Enabled,Direction,Action | Format-Table -AutoSize }
        else { Write-Host '[提示] 未发现本客户端的已知防火墙规则；可在菜单5单独设置。' }
    } catch { Write-Host ('[未核对] 无法读取防火墙：'+$_.Exception.Message) }
    Write-Host '这里只说明配置和版本；连通性请选菜单2，实际出画仍需客户端检查。'
}

function Test-RemoteEndpoint {
    param([string]$Label, [string]$EndpointHost, [int]$Port)
    $localAddresses = @(Get-NetIPAddress -ErrorAction Stop | ForEach-Object { $_.IPAddress })
    $addresses = @([Net.Dns]::GetHostAddresses($EndpointHost) | Where-Object { $_.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork })
    if (-not $addresses.Count) { throw ('无法解析IPv4对端：'+$EndpointHost) }
    foreach ($address in $addresses) {
        if (Test-AddressIsLocal -Address $address.ToString() -LocalAddresses $localAddresses) {
            Write-Host ('[失败] {0}的目标{1}属于本机；ping自己不能证明对端在线。本次不连接。' -f $Label,$address) -ForegroundColor Red
            return
        }
    }
    foreach ($address in $addresses) {
        Write-Host ("`n{0}实际对端：{1}:{2}" -f $Label,$address,$Port)
        $ping = New-Object Net.NetworkInformation.Ping
        try { $reply=$ping.Send($address,1000); Write-Host ('ping：'+$reply.Status) }
        catch { Write-Host ('ping未成功：'+$_.Exception.Message) }
        finally { $ping.Dispose() }
        $tcp = New-Object Net.Sockets.TcpClient
        try {
            $pending=$tcp.ConnectAsync($address,$Port)
            if ($pending.Wait(1500) -and $tcp.Connected) { Write-Host '[连通] TCP端口可连接。' -ForegroundColor Green }
            else { Write-Host '[未连通] TCP未在1.5秒内连接。' -ForegroundColor Yellow }
        } catch { Write-Host ('[未连通] '+$_.Exception.GetBaseException().Message) -ForegroundColor Yellow }
        finally { $tcp.Dispose() }
    }
    Write-Host '端口连通只证明网络可达，不等于已收到或解码视频；这里不发送MQTT或键鼠消息。'
}

function Test-Peers {
    Write-Host '将测试官方对端192.168.12.1:3333及INI中的队友对端。'
    if ((Read-Host '输入 TEST 才开始') -cne 'TEST') { return }
    Test-RemoteEndpoint -Label '官方' -EndpointHost '192.168.12.1' -Port 3333
    $settings=Read-TeammateSettings
    if ($settings.Enabled) { Test-RemoteEndpoint -Label '队友' -EndpointHost $settings.PeerHost -Port $settings.PeerPort }
    else { Write-Host '队友视角已关闭，不测试队友连接。' }
}

function Set-CompatibilityRole {
    if (Get-Process -Name luojiamaster -ErrorAction SilentlyContinue) { throw '先关闭正在运行的客户端，再改设置。' }
    $selection=(Read-Host '选择兼容设置：A / B / OFF（关闭）').Trim().ToUpperInvariant()
    if ($selection -notin @('A','B','OFF')) { throw '没有选择有效设置，未修改。' }
    Write-Host ('将替换程序/队友视角.ini中的现有角色及自定义peer设置为：'+$selection)
    if ((Read-Host ('再次输入 '+$selection+' 才写入')) -cne $selection) { return }
    if (-not (Test-Path -LiteralPath $script:ProgramRoot -PathType Container)) { throw '程序目录不存在。' }
    if (Test-Path -LiteralPath $script:PeerIni) {
        $backupDir=Join-Path $env:LOCALAPPDATA 'luojiamaster\maintenance'
        [void][IO.Directory]::CreateDirectory($backupDir)
        Copy-Item -LiteralPath $script:PeerIni -Destination (Join-Path $backupDir '队友视角.ini.bak') -Force
        Write-Host ('上一次设置备份：'+(Join-Path $backupDir '队友视角.ini.bak'))
    }
    $content = if ($selection -eq 'OFF') { "# Compatibility: disabled`r`npeer=off`r`n" } else { "# Compatibility: manually selected`r`nrole=$selection`r`nlisten=47700`r`n" }
    [IO.File]::WriteAllText($script:PeerIni,$content,(New-Object Text.UTF8Encoding($false)))
    Write-Host '[完成] 设置已写入；下次由你双击客户端生效。本工具不启动客户端。'
}

function Set-SelectedTeammateAddress {
    Require-Administrator
    $adapters=@(Get-NetAdapter -Physical | Where-Object { $_.MediaType -eq '802.3' -and $_.Status -eq 'Up' })
    $adapters | Select-Object ifIndex,Name,InterfaceDescription,MacAddress | Format-Table -AutoSize
    $selectedIndex=0
    if (-not [int]::TryParse((Read-Host '输入明确接队友线的网卡编号ifIndex'),[ref]$selectedIndex)) { throw '网卡编号无效，未修改。' }
    $selected=@($adapters | Where-Object { $_.ifIndex -eq $selectedIndex })
    if ($selected.Count -ne 1) { throw '没有匹配到唯一的物理有线网卡，未修改。' }
    $adapter=$selected[0]
    $ipv4=@(Get-NetIPAddress -InterfaceIndex $selectedIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue)
    $defaultRoutes=@(Get-NetRoute -InterfaceIndex $selectedIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)
    $reason=Get-AdapterProtectionReason -Adapter $adapter -IPv4Addresses @($ipv4 | ForEach-Object { $_.IPAddress }) -DefaultRoutes $defaultRoutes
    if ($reason) { throw $reason }
    $role=(Read-Host '这块队友网卡使用A还是B地址').Trim().ToUpperInvariant()
    if ($role -notin @('A','B')) { throw '角色无效，未修改。' }
    $want=if($role -eq 'A'){'10.77.0.1'}else{'10.77.0.2'}
    Write-Host ('选中：编号{0}，{1}，MAC {2}，当前IPv4 {3}' -f $selectedIndex,$adapter.Name,$adapter.MacAddress,(@($ipv4 | ForEach-Object {$_.IPAddress}) -join ', '))
    Write-Host ('只将此网卡设为{0}/24，不设置网关；不改其他网卡或路由。' -f $want)
    $confirmation=('{0} {1}' -f $selectedIndex,$role)
    if ((Read-Host ('输入 '+$confirmation+' 才确认')) -cne $confirmation) { return }
    # Recheck after the human confirmation: an address/interface may have changed.
    $current=@(Get-NetIPAddress -InterfaceIndex $selectedIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue)
    $routes=@(Get-NetRoute -InterfaceIndex $selectedIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)
    $adapterNow=@(Get-NetAdapter -Physical | Where-Object { $_.ifIndex -eq $selectedIndex })
    if ($adapterNow.Count -ne 1) { throw '网卡列表已变化，未修改。' }
    $adapterNow=$adapterNow[0]
    if ($adapterNow.MacAddress -ne $adapter.MacAddress) { throw '网卡身份已变化，未修改。' }
    $reason=Get-AdapterProtectionReason -Adapter $adapterNow -IPv4Addresses @($current | ForEach-Object {$_.IPAddress}) -DefaultRoutes $routes
    if ($reason) { throw $reason }
    if (@(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -eq $want -and $_.InterfaceIndex -ne $selectedIndex }).Count) { throw '这个目标地址已在本机另一块网卡上，未修改。' }
    if (-not @($current | Where-Object {$_.IPAddress -eq $want -and $_.PrefixLength -eq 24}).Count) {
        New-NetIPAddress -InterfaceIndex $selectedIndex -IPAddress $want -PrefixLength 24 | Out-Null
    }
    $preferred=$false
    for($attempt=0;$attempt -lt 6;$attempt++) {
        $newState=@(Get-NetIPAddress -InterfaceIndex $selectedIndex -AddressFamily IPv4 | Where-Object { $_.IPAddress -eq $want -and $_.PrefixLength -eq 24 -and $_.AddressState -eq 'Preferred' })
        if($newState.Count -eq 1){$preferred=$true;break}
        Start-Sleep -Milliseconds 500
    }
    if(-not $preferred){throw '新地址尚未确认可用，旧队友地址未删除。请检查是否存在地址冲突。'}
    # Remove only the old dedicated peer address, never every address/route.
    foreach ($old in $current) {
        if ($old.IPAddress -in @('10.77.0.1','10.77.0.2') -and $old.IPAddress -ne $want) { $old | Remove-NetIPAddress -Confirm:$false }
    }
    Get-NetIPAddress -InterfaceIndex $selectedIndex -AddressFamily IPv4 | Select-Object InterfaceAlias,IPAddress,PrefixLength,AddressState | Format-Table
    Write-Host ('[完成] 网卡已设为'+$role+'地址；客户端角色仍需在菜单3手动选择相同A/B。')
}

function Add-ClientFirewallRules {
    Require-Administrator
    if (-not (Test-Path -LiteralPath $script:ClientExe -PathType Leaf)) { throw '找不到程序/luojiamaster.exe，未设置规则。' }
    Write-Host '将添加本程序的入站TCP47700、UDP3334及本地子网ICMP回显规则。不会关闭防火墙。'
    if ((Read-Host '输入 FIREWALL 才设置') -cne 'FIREWALL') { return }
    $specs=@(@{Name='RM26.Client.Teammate.TCP';Title='RM26 client teammate TCP47700';Protocol='TCP';Port=47700},
             @{Name='RM26.Client.Official.UDP';Title='RM26 client official UDP3334';Protocol='UDP';Port=3334})
    foreach($spec in $specs) {
        if (Get-NetFirewallRule -Name $spec.Name -ErrorAction SilentlyContinue) { Write-Host ('[保留] 同名规则已存在，请用菜单1核对启用状态：'+$spec.Name);continue }
        New-NetFirewallRule -Name $spec.Name -DisplayName $spec.Title -Group 'RM26 Client' -Direction Inbound -Action Allow -Profile Any -Program $script:ClientExe -Protocol $spec.Protocol -LocalPort $spec.Port -RemoteAddress LocalSubnet | Out-Null
        Write-Host ('[已添加] '+$spec.Title)
    }
    if (-not (Get-NetFirewallRule -Name 'RM26.Client.ICMP' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name 'RM26.Client.ICMP' -DisplayName 'RM26 client local subnet ICMP echo' -Group 'RM26 Client' -Direction Inbound -Action Allow -Profile Any -Protocol ICMPv4 -IcmpType 8 -RemoteAddress LocalSubnet | Out-Null
    }
    Write-Host '本次只添加缺少的规则；已有规则保留原样。菜单1可查看，不把规则存在当作实际连通。'
}

function Show-MaintenanceMenu {
    while ($true) {
        Write-Host "`n维护工具：只维护当前基础版，不设置赛季或新角色。"
        Write-Host '1 查看本机状态和版本    2 测试实际对端连接'
        Write-Host '3 手动设置兼容A/B或关闭  4 指定网卡后设置队友IP'
        Write-Host '5 添加客户端防火墙规则  0 退出'
        try {
            switch ((Read-Host '输入菜单编号').Trim()) {
                '1' { Show-LocalStatus }; '2' { Test-Peers }; '3' { Set-CompatibilityRole }
                '4' { Set-SelectedTeammateAddress }; '5' { Add-ClientFirewallRules }; '0' { return }
                default { Write-Host '请输0到5。' }
            }
        } catch { Write-Host ('[未完成] '+$_.Exception.Message) -ForegroundColor Red }
    }
}
if (-not $NoMenu) { Show-MaintenanceMenu }
