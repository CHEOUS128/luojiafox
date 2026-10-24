param(
  [string]$Port = "",
  [int]$Baud = 921600,
  [double]$Seconds = 30.0,
  [switch]$NoCrc,                    # 不过滤 CRC（看原始帧率用）
  [switch]$Raw,                      # 只统计字节/0xA5，不解析（怀疑波特率不对时用）
  [switch]$Loop                      # 自检：CH340 自己的 TX 和 RX 短接，看能不能原样收回
)
$ErrorActionPreference = 'Stop'
try {
  [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936)
  $OutputEncoding = [System.Text.Encoding]::GetEncoding(936)
} catch {}
. (Join-Path $PSScriptRoot 'crc0310.ps1')

function Show-HexDump([byte[]]$d, [int]$maxBytes = 320) {
  $n = [Math]::Min($d.Length, $maxBytes)
  for ($i = 0; $i -lt $n; $i += 16) {
    $hex = ""
    $asc = ""
    for ($j = 0; $j -lt 16; $j++) {
      $k = $i + $j
      if ($k -lt $n) {
        $hex += ("{0:X2} " -f $d[$k])
        $c = [int]$d[$k]
        if ($c -ge 32 -and $c -lt 127) { $asc += [char]$c } else { $asc += "." }
      } else {
        $hex += "   "
        $asc += " "
      }
      if ($j -eq 7) { $hex += " " }
    }
    Write-Host ("  {0:X4}  {1} |{2}|" -f $i, $hex, $asc)
  }
  if ($d.Length -gt $maxBytes) {
    Write-Host ("  ... 共 {0} 字节，上面只显示前 {1} 字节" -f $d.Length, $maxBytes)
  }
}

if ($Port -eq "" -or $Port -eq "ports") {
  Show-Ports0310
  Write-Host ""
  Write-Host "  Loop :  listen0310.bat COM7 -Loop       (CH340 自己 TX-RX 短接自检)"
  Write-Host "  Next:  listen0310.bat COM3"
  Write-Host "  Longer: listen0310.bat COM3 -Seconds 60"
  return
}

$sp = New-SerialPort0310 $Port $Baud
try {
  $sp.Open()
} catch {
  Write-Host ("打不开 {0}：{1}" -f $Port, $_.Exception.Message)
  Write-Host "  被占用 -> 拔掉 STM32 后重试；串口号不对 -> 直接双击 bat 看列表。"
  return
}

if ($Loop) {
  # 自检：每 200ms 发一行，看 RX 能不能原样收回。用来确认 CH340 的 TX/RX 哪根是哪根。
  Write-Host ("=== 回环自检 {0} @ {1} ===" -f $Port, $Baud)
  Write-Host "接线：把 CH340 自己的 TX 和 RX 直接短接（不接板子）。"
  Write-Host ""
  $sp.ReadTimeout = 300
  $ok = 0
  $bad = 0
  try {
    for ($i = 1; $i -le 10; $i++) {
      $msg = "LOOP{0:D2}" -f $i
      $bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
      $sp.Write($bytes, 0, $bytes.Length)
      Start-Sleep -Milliseconds 120
      $rx = ""
      try {
        $n = $sp.ReadByte()
        if ($n -ge 0) {
          $got = New-Object System.Collections.Generic.List[byte]
          $got.Add([byte]$n)
          while ($sp.BytesToRead -gt 0) { $got.Add([byte]$sp.ReadByte()) }
          $rx = -join ($got | ForEach-Object { [char]$_ })
        }
      } catch {}
      if ($rx -eq $msg) { $ok++ } elseif ($rx -ne "") { $bad++ }
      Write-Host ("  发 {0,-8} 收 {1}" -f $msg, $(if ($rx -eq "") { "（没收到）" } else { $rx }))
      Start-Sleep -Milliseconds 200
    }
  } finally {
    try { $sp.Close() } catch {}
    $sp.Dispose()
  }
  Write-Host ""
  if ($ok -ge 8) {
    Write-Host ("  ✅ 10 次里有 {0} 次原样收回 —— 这根线是 RX，CH340 工作正常。" -f $ok)
    Write-Host "     接下来把它接到板子的 TX（图传口的数据线）上再听。"
  } else {
    Write-Host ("  ❌ 只收回 {0} 次。TX/RX 没短接好，或者这根接的是 TX。" -f $ok)
    Write-Host "     把这根线换到另一个脚再跑一次本命令。"
  }
  return
}

Write-Host ("=== 监听 {0} @ {1} 8N1，{2} 秒 ===" -f $Port, $Baud, $Seconds)
Write-Host "接线（只接两根，接错会短路）："
Write-Host "  CH340 的 RX  ->  板子的 TX（图传口里那根数据线）"
Write-Host "  CH340 的 GND ->  板子 GND（必须是板子的地，不能只接 USB 的地）"
Write-Host "  CH340 的 TX  ->  ★悬空不接★（板子正在驱动这根线）"
Write-Host ""
Write-Host "  不确定哪根是 RX？先跑：listen0310.bat COM7 -Loop（TX、RX 短接自检）"
Write-Host ""

$buf      = New-Object byte[] 8192
$acc      = New-Object System.Collections.Generic.List[byte]
$all      = New-Object System.Collections.Generic.List[byte]
$seqs     = New-Object System.Collections.Generic.List[int]
$good     = 0
$badCrc   = 0
$badHead  = 0
$headOkCrcBad = 0
$headHits = 0
$innerOk  = 0
$accBase  = 0
$lastHeadPos = -1
$spans    = New-Object System.Collections.Generic.List[int]
$total    = 0
$readErr  = 0
$lastErr  = ""
$firstSeen = $false
$firstSeq = -1
$lastSeq  = -1
$recent   = New-Object System.Collections.Generic.Queue[string]
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$nextReport = 2000

try {
  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $n = 0
    try {
      $n = $sp.Read($buf, 0, $buf.Length)
    } catch [System.TimeoutException] {
      $n = 0
    } catch {
      $n = 0
      $readErr++
      $lastErr = $_.Exception.Message
    }
    if ($n -gt 0) {
      if (-not $firstSeen) {
        $firstSeen = $true
        Write-Host ("  [{0:N1}s] 收到第一笔数据 {1} 字节" -f $sw.Elapsed.TotalSeconds, $n)
      }
      $total += $n
      if ($all.Count -lt 8388608) {
        if ($n -eq $buf.Length) {
          $all.AddRange($buf)
        } else {
          $tmp = New-Object byte[] $n
          [System.Array]::Copy($buf, $tmp, $n)
          $all.AddRange($tmp)
        }
      }
      if ($Raw) {
        for ($i = 0; $i -lt $n; $i++) {
          if ($buf[$i] -eq 0xA5) { $badHead++ }
        }
      } else {
        for ($i = 0; $i -lt $n; $i++) { $acc.Add($buf[$i]) }
        $accBase += [Math]::Max(0, $acc.Count - $n)
        $scan = 0
        while (($acc.Count - $scan) -ge 9) {
          if ($acc[$scan] -ne 0xA5) {
            $scan++
            $badHead++
            continue
          }
          # 只靠帧头 7 字节和 CRC8 定位。不要求后面 309 字节全都到齐 ——
          # 线上的帧可能被截断（USB 丢 64 字节、CH340 丢字节），
          # 以前「结构对但 CRC 不对就跳 309」会把残帧后面的好帧一起吃掉。
          $headOk = $true
          if ($acc[$scan + 1] -ne 0x2C -or $acc[$scan + 2] -ne 0x01) { $headOk = $false }
          if ($acc[$scan + 5] -ne 0x10 -or $acc[$scan + 6] -ne 0x03) { $headOk = $false }
          if ($headOk -and -not $NoCrc) {
            $h4 = New-Object byte[] 4
            for ($k = 0; $k -lt 4; $k++) { $h4[$k] = $acc[$scan + $k] }
            if ($acc[$scan + 4] -ne [byte](Get-Crc8 $h4)) { $headOk = $false }
          }
          if (-not $headOk) {
            $scan++
            $badHead++
            continue
          }

          # 帧头 OK。到这里才用「结构对但 CRC 不对」这个分支。
          # 上一轮因为「帧还没收全」break 时，这个帧头会再被扫到一次，
          # 用绝对位置去重，避免重复计数。
          $absPos = $accBase + $scan
          if ($absPos -ne $lastHeadPos) {
            $headHits++
            if ($lastHeadPos -ge 0) { $spans.Add($absPos - $lastHeadPos) | Out-Null }
            $lastHeadPos = $absPos
          }

          if (($acc.Count - $scan) -lt 309) { break }   # 尾巴还没收全，留到下一轮

          $frame = $acc.GetRange($scan, 309).ToArray()
          $ok = $true
          if (-not $NoCrc) {
            $crcRecv = [int]$frame[307] -bor ([int]$frame[308] -shl 8)
            if ($ok -and $crcRecv -ne (Get-Crc16 ([byte[]]$frame[0..306]))) { $ok = $false }
          }
          if ($ok) {
            $good++
            $sq = [int]$frame[3]
            if ($firstSeq -lt 0) { $firstSeq = $sq }
            $lastSeq = $sq
            $seqs.Add($sq) | Out-Null
            if ($recent.Count -ge 5) { [void]$recent.Dequeue() }
            $innerLen = ([int]$frame[7] -shl 8) -bor [int]$frame[8]
            $innerSeq = ([int]$frame[9] -shl 8) -bor [int]$frame[10]
            if ($innerLen -ge 2 -and $innerLen -le 298) { $innerOk++ }
            $recent.Enqueue(("seq={0,-3} 内层长度={1,-3} 内层序号={2}" -f $sq, $innerLen, $innerSeq))
            $scan += 309
          } else {
            $badCrc++
            $headOkCrcBad++
            # 关键：不跳 309，只跳 1 个字节重新找下一个帧头。
            # 残帧长度不确定，跳 309 一定会把下一个好帧的帧头吃掉。
            $scan++
          }
        }
        if ($scan -gt 0) {
          $drop = [Math]::Min($scan, $acc.Count)
          $acc.RemoveRange(0, $drop)
          $accBase += $drop
        }
        if ($acc.Count -gt 8192) {
          $drop = $acc.Count - 8192
          $acc.RemoveRange(0, $drop)
          $accBase += $drop
        }
      }
    }
    if ($sw.ElapsedMilliseconds -ge $nextReport) {
      $nextReport += 2000
      Write-Host ("  [{0,4:N0}s] 收到 {1,7:N0} 字节 | 帧头 {2} | 完整帧 {3} 残帧 {4} | 乱字节 {5}" -f `
        $sw.Elapsed.TotalSeconds, $total, $headHits, $good, $badCrc, $badHead)
    }
  }
} finally {
  try { $sp.Close() } catch {}
  $sp.Dispose()
}

$elapsed = $sw.Elapsed.TotalSeconds
Write-Host ""
Write-Host "=== 结果 ==="
Write-Host ("  时长        {0:N1} 秒" -f $elapsed)
Write-Host ("  收到字节    {0:N0}（{1:N2} KB/s）" -f $total, ($total / 1024.0 / [Math]::Max($elapsed, 0.001)))
Write-Host ("  好帧头      {0} 个（{1:N2} 帧/秒）" -f $headHits, ($headHits / [Math]::Max($elapsed, 0.001)))
Write-Host ("  完整帧      {0} 帧      残帧 {1}     乱字节 {2}" -f $good, $badCrc, $badHead)
if ($good -gt 0) {
  Write-Host ("  内层长度合法 {0}/{1}" -f $innerOk, $good)
}
if ($spans.Count -gt 0) {
  $sorted = ($spans | Sort-Object)
  $med = $sorted[[int]($sorted.Count / 2)]
  $mn  = $sorted[0]
  $mx  = $sorted[$sorted.Count - 1]
  $full = 0; $trunc = 0; $over = 0
  foreach ($s in $spans) {
    if ($s -eq 309) { $full++ } elseif ($s -lt 309) { $trunc++ } else { $over++ }
  }
  Write-Host ("  帧间隔      中位 {0} 字节（最小 {1} 最大 {2}）  正好309={3} 偏短={4} 偏长={5}" -f `
    $med, $mn, $mx, $full, $trunc, $over)
}
if ($readErr -gt 0) {
  Write-Host ("  读取异常    {0} 次  最后一条：{1}" -f $readErr, $lastErr)
}

if ($total -gt 0 -and $all.Count -le 200000) {
  $a5 = 0
  foreach ($b in $all) { if ($b -eq 0xA5) { $a5++ } }
  Write-Host ("  其中 0xA5   {0} 个" -f $a5)
}

if ($total -gt 0) {
  $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
  $outPath = Join-Path $PSScriptRoot ("listen0310_{0}_{1}.bin" -f $Port, $stamp)
  try {
    [System.IO.File]::WriteAllBytes($outPath, $all.ToArray())
    Write-Host ""
    Write-Host "=== 收到的原始字节（前 320）==="
    Show-HexDump $all.ToArray() 320
    Write-Host ""
    Write-Host ("  已存盘：{0}" -f $outPath)
  } catch {
    Write-Host ("  存盘失败：{0}" -f $_.Exception.Message)
  }
}

Write-Host ""
if ($good -gt 0) {
  $uniq = ($seqs | Sort-Object -Unique).Count
  Write-Host ("  外序号      {0} → {1}，不同值 {2} 个" -f $firstSeq, $lastSeq, $uniq)
  Write-Host "  最近 5 帧："
  foreach ($s in $recent) { Write-Host ("    " + $s) }
  Write-Host ""
  Write-Host "  ✅ 收到完整的 309 字节 0x0310 帧 —— 板子确实在往这根线上发视频。"
  Write-Host "     下一步：把这根线接到 VT03 的 RX 焊盘，客户端就能看到画面。"
} elseif ($headHits -gt 0) {
  Write-Host "  ⚠ 这根线上确实有 0x0310 帧头（CRC8 都对），但 309 字节的整帧收不齐。"
  Write-Host "     说明板子在发视频，断的是「传输过程中丢字节」，不是固件。"
  Write-Host "     按帧间隔那一行判断："
  Write-Host "       间隔中位数 < 309、每次少掉几十字节 -> USB/CH340 丢包；换 CH340、降波特率试"
  Write-Host "       间隔忽大忽小、乱字节多          -> 没共地 / 线接触不良 / 线太长"
  Write-Host "       间隔是 309 附近但 CRC 错          -> 波特率偏差（确认 921600 8N1）"
  Write-Host "     夹在中间的「乱字节」不代表收到错数据，是残帧中间那些没被认出来的字节。"
} elseif ($total -gt 0) {
  Write-Host "  ⚠ 收到了字节，但一整帧都没凑齐（60 秒正常应是 9 万字节量级）。"
  Write-Host "     先看上面那份十六进制：以 A5 12 01 开头 -> 是视频帧，只是没发完/被截断；"
  Write-Host "     全是 00 或 FF -> 线悬空/没共地，收到的只是噪声；"
  Write-Host "     是规整重复的花纹 -> 波特率对不上。"
  Write-Host "     同时去 Jetson 上看一眼「成帧」还在不在涨（见下面的命令）。"
} else {
  Write-Host "  ❌ 一个字节都没收到。按下面顺序排查："
  Write-Host "     0) 先自检这根线是不是 RX：把 CH340 自己的 TX、RX 短接，跑"
  Write-Host "        listen0310.bat COM7 -Loop      （收得回来 = 这根是 RX，工具没问题）"
  Write-Host "     1) 这根线是不是接在板子正在发的那根 TX 上，GND 是不是板子的 GND"
  Write-Host "     2) 先确认 Jetson 还在出帧：在 Jetson 上跑"
  Write-Host "        grep -a '1包/帧' ~/my_lf_INF_20261025/logs/autostart.log | tail -n 2"
  Write-Host "        两次间隔看「成帧」有没有涨；不涨说明 STM32 根本没收到视频（Jetson 卡住/线松）"
  Write-Host "     3) 换个 CH340 的口/换根线；万用表量数据线对 GND 是不是 3.3V 空闲电平"
}
