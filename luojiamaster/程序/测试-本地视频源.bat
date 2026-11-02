@echo off
setlocal EnableDelayedExpansion
chcp 936 >nul
cd /d "%~dp0"
title RM26 local video source

rem ============================================================
rem  本地假图传：模拟官方 UDP 图传（没有实车 / 没有 ffmpeg 时用）
rem
rem  用法：
rem     本文件                      -> 发到 127.0.0.1:3334（A 实例），100% 速度
rem     本文件 3335                 -> 发到 127.0.0.1:3335（B 实例）
rem     本文件 3335 50              -> 同上，半速（看丢帧、卡顿用）
rem
rem  画面来源：测试画面.h265
rem     从 2026-10-25 的真实官方图传抓包里抽出来的一段，
rem     1920x1080 HEVC，120 帧，字节就是官方图传的字节。
rem  不需要 ffmpeg，只用 Windows 自带的 PowerShell。
rem  每隔 5 秒打印一行 sent ... 说明在发。
rem ============================================================

set "PORT=%~1"
if "%PORT%"=="" set "PORT=3334"
set "SPEED=%~2"
if "%SPEED%"=="" set "SPEED=100"

set "SRC=%~dp0测试画面.h265"
if not exist "%SRC%" (
    echo [错误] 找不到 "%SRC%"
    echo        这是从真实抓包里抽出来的测试码流，必须和本 bat 放在同一目录。
    pause
    exit /b 1
)
for %%F in ("%SRC%") do echo 码流: %%~nxF ^(%%~zF 字节^)

rem  ---- 发包本体（PowerShell）----
rem  数据包格式（和固件/客户端一致）：
rem    u16 大端 frame_id | u16 大端 shard_id | u32 大端 frame_bytes | 分片数据
rem  按 HEVC 的 access unit 切帧（一个 VCL NAL 开一帧），shard_id 从 0 连续。
rem  路径和参数用环境变量传进去：Windows 环境变量是 UTF-16，
rem  不受 .ps1 文件编码影响，中文路径不会出问题。
set "RM26_SRC=%SRC%"
set "RM26_PORT=%PORT%"
set "RM26_SPEED=%SPEED%"
set "PS_FILE=%TEMP%\rm26_localsrc_send.ps1"
> "%PS_FILE%" echo $ErrorActionPreference='Stop'
>> "%PS_FILE%" echo $path=$env:RM26_SRC; $port=[int]$env:RM26_PORT; $speed=[int]$env:RM26_SPEED
>> "%PS_FILE%" echo if($speed -le 0){ $speed=100 }
>> "%PS_FILE%" echo $bytes=[IO.File]::ReadAllBytes($path)
>> "%PS_FILE%" echo $nals=New-Object System.Collections.Generic.List[object]; $start=-1
>> "%PS_FILE%" echo for($i=0;$i -lt $bytes.Length-3;$i++){ if($bytes[$i] -eq 0 -and $bytes[$i+1] -eq 0 -and $bytes[$i+2] -eq 1){ if($start -ge 0){ $nals.Add(@($start,$i)) }; $start=$i; $i+=2 } }
>> "%PS_FILE%" echo if($start -ge 0){ $nals.Add(@($start,$bytes.Length)) }
>> "%PS_FILE%" echo $aus=New-Object System.Collections.Generic.List[byte[]]; $cur=New-Object System.Collections.Generic.List[byte]; $hasVcl=$false
>> "%PS_FILE%" echo foreach($n in $nals){ $t=($bytes[$n[0]+3] -shr 1) -band 0x3F; $mid=New-Object byte[] ($n[1]-$n[0]); [Array]::Copy($bytes,$n[0],$mid,0,$mid.Length); if($t -le 31){ if($hasVcl){ $aus.Add([byte[]]$cur.ToArray()); $cur=New-Object System.Collections.Generic.List[byte] }; $cur.AddRange($mid); $hasVcl=$true } else { if($hasVcl){ $aus.Add([byte[]]$cur.ToArray()); $cur=New-Object System.Collections.Generic.List[byte]; $hasVcl=$false }; $cur.AddRange($mid) } }
>> "%PS_FILE%" echo if($cur.Count -gt 0){ $aus.Add([byte[]]$cur.ToArray()) }
>> "%PS_FILE%" echo $shard=1332; $framePkts=New-Object System.Collections.Generic.List[object]; $frameLens=New-Object System.Collections.Generic.List[int]
>> "%PS_FILE%" echo foreach($au in $aus){ $len=$au.Length; $total=[int][Math]::Ceiling($len/[double]$shard); if($total -lt 1){ $total=1 }; $plist=New-Object System.Collections.Generic.List[byte[]]
>> "%PS_FILE%" echo for($s=0;$s -lt $total;$s++){ $off=$s*$shard; $clen=$len-$off; if($clen -gt $shard){ $clen=$shard }; $pkt=New-Object byte[] (8+$clen); $pkt[2]=[byte](($s -shr 8) -band 0xFF); $pkt[3]=[byte]($s -band 0xFF); $pkt[4]=[byte](($len -shr 24) -band 0xFF); $pkt[5]=[byte](($len -shr 16) -band 0xFF); $pkt[6]=[byte](($len -shr 8) -band 0xFF); $pkt[7]=[byte]($len -band 0xFF); [Array]::Copy($au,$off,$pkt,8,$clen); $plist.Add($pkt) }
>> "%PS_FILE%" echo $framePkts.Add($plist); $frameLens.Add($len) }
>> "%PS_FILE%" echo Write-Host ('access units: ' + $framePkts.Count)
>> "%PS_FILE%" echo $client=New-Object System.Net.Sockets.UdpClient; $target=New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback,$port)
>> "%PS_FILE%" echo $frameMs=33.333*100.0/$speed; $sw=[Diagnostics.Stopwatch]::StartNew(); $passSw=[Diagnostics.Stopwatch]::StartNew(); $fid=0; $sent=0; $sentBytes=0; $nextReport=5; $count=$framePkts.Count
>> "%PS_FILE%" echo while($true){
>> "%PS_FILE%" echo $i=0; $passSw.Restart()
>> "%PS_FILE%" echo while($i -lt $count){
>> "%PS_FILE%" echo $end=$i+16; if($end -gt $count){ $end=$count }
>> "%PS_FILE%" echo while($i -lt $end){ $hi=[byte](($fid -shr 8) -band 0xFF); $lo=[byte]($fid -band 0xFF)
>> "%PS_FILE%" echo foreach($p in $framePkts[$i]){ $p[0]=$hi; $p[1]=$lo; [void]$client.Send($p,$p.Length,$target) }
>> "%PS_FILE%" echo $sentBytes += $frameLens[$i]; $sent++; $fid=($fid+1) -band 0xFFFF; $i++ }
>> "%PS_FILE%" echo $wait=[int]($i*$frameMs)-[int]$passSw.ElapsedMilliseconds; if($wait -gt 0){ Start-Sleep -Milliseconds $wait }
>> "%PS_FILE%" echo if($sw.Elapsed.TotalSeconds -ge $nextReport){ Write-Host ('sent ' + $sent + ' frames, ' + [Math]::Round($sentBytes/1MB,2) + ' MB, frame_id=' + $fid); $nextReport=$nextReport+5 }
>> "%PS_FILE%" echo }
>> "%PS_FILE%" echo }

echo.
echo [发送] 目标 127.0.0.1:%PORT%   速度 %SPEED%%%   ^(Ctrl+C 停止^)
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_FILE%"
echo.
echo 发送结束。
pause
