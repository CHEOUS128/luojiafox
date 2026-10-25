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
$gapNeed  = New-Object System.Collections.Generic.List[int]
$prevIlen = -1
$total    = 0
$readErr  = 0
$consecErr = 0
$portLost = $false
$lostAt   = 0.0
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
    if ($portLost) { Start-Sleep -Milliseconds 100; continue }
    try {
      $n = $sp.Read($buf, 0, $buf.Length)
      $consecErr = 0
    } catch [System.TimeoutException] {
      $n = 0
      $consecErr = 0
    } catch {
      $n = 0
      $readErr++
      $consecErr++
      $lastErr = $_.Exception.Message
      # 串口被拔掉/关掉时 Read 会每毫秒抛一次，以前会刷 100 万行「读取异常」。
      # 连错 200 次（约 0.2 秒）就认定端口没了，停止采样并给出结论。
      if ($consecErr -ge 200) {
        $portLost = $true
        $lostAt = $sw.Elapsed.TotalSeconds
        Write-Host ""
        Write-Host ("  [{0:N1}s] 串口断了：{1}" -f $lostAt, $lastErr)
        Write-Host "  这通常是 CH340 从 USB 口被拔掉（或接触不良掉线）。已经停止采样。"
        break
      }
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

          # 帧头 OK（7 字节 + CRC8）。
          # 不再要求后面 309 字节全到齐 —— 线上常把帧尾的零填充/CRC 切掉，
          # 那样永远凑不满 309，会把每一帧都误判成「残帧」。
          # 用「内层长度」按帧切：一帧 = 11 + 内层长度。内层长度不合法就退化成 309。
          $absPos = $accBase + $scan
          if ($absPos -ne $lastHeadPos) {
            $headHits++
            if ($lastHeadPos -ge 0) {
              $spans.Add($absPos - $lastHeadPos) | Out-Null
              if ($prevIlen -ge 0) { $gapNeed.Add(($absPos - $lastHeadPos) - $prevIlen - 11) | Out-Null }
            }
            $lastHeadPos = $absPos
          }
          if (($acc.Count - $scan) -lt 9) { break }
          $innerLenHdr = ([int]$acc[$scan + 7] -shl 8) -bor [int]$acc[$scan + 8]
          $prevIlen = $innerLenHdr
          $need = 309
          if ($innerLenHdr -ge 2 -and $innerLenHdr -le 296) { $need = 11 + $innerLenHdr }
          if (($acc.Count - $scan) -lt $need) { break }   # 这一帧还没收全，留到下一轮

          $frame = $acc.GetRange($scan, $need).ToArray()
          $crcOk = $true
          if ($need -ge 309) {
            $crcRecv = [int]$frame[307] -bor ([int]$frame[308] -shl 8)
            if ($crcRecv -ne (Get-Crc16 ([byte[]]$frame[0..306]))) { $crcOk = $false }
          }
          if ($crcOk) {
            $good++
            $sq = [int]$frame[3]
            if ($firstSeq -lt 0) { $firstSeq = $sq }
            $lastSeq = $sq
            $seqs.Add($sq) | Out-Null
            if ($recent.Count -ge 5) { [void]$recent.Dequeue() }
            $innerSeq = ([int]$frame[9] -shl 8) -bor [int]$frame[10]
            if ($innerLenHdr -ge 2 -and $innerLenHdr -le 296) { $innerOk++ }
            $recent.Enqueue(("seq={0,-3} 内层长度={1,-3} 内层序号={2}" -f $sq, $innerLenHdr, $innerSeq))
            $scan += $need
          } else {
            $badCrc++
            $headOkCrcBad++
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
if ($portLost) {
  Write-Host ("  ⚠ 串口在 {0:N1} 秒时掉了（{1}），后面这段时间等于没在听。" -f $lostAt, $lastErr)
  if ($lostAt -lt 2.0) {
    Write-Host "    一开始就掉 = CH340 根本没插稳：换 USB 口、换根 USB 线，再跑一次。"
  }
}
if ($portLost) {
  Write-Host "  ⇒ 这次的结果不能用。先解决 CH340 掉线，重新跑。"
} elseif ($total -eq 0) {
  Write-Host "  ❌ 一个字节都没收到。先确认 Jetson 还在出帧（第 2 条给的两行 grep），"
  Write-Host "     再确认 CH340 的 RX 真的接在板子的 TXD（3pin 的 2 号脚）上。"
}
Write-Host ("  收到字节    {0:N0}（{1:N2} KB/s）" -f $total, ($total / 1024.0 / [Math]::Max($elapsed, 0.001)))
Write-Host ("  好帧头      {0} 个（{1:N2} 帧/秒）" -f $headHits, ($headHits / [Math]::Max($elapsed, 0.001)))
Write-Host ("  完整帧      {0} 帧      残帧 {1}     乱字节 {2}" -f $good, $badCrc, $badHead)
Write-Host ("  帧率        {0:N2} 帧/秒  <- 判读就看这个数" -f ($good / [Math]::Max($elapsed, 0.001)))
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
if ($gapNeed.Count -gt 0) {
  $gs = ($gapNeed | Sort-Object)
  $gmed = $gs[[int]($gs.Count / 2)]
  $gpos = 0; $gneg = 0; $gzero = 0
  foreach ($g in $gapNeed) {
    if ($g -gt 0) { $gpos++ } elseif ($g -lt 0) { $gneg++ } else { $gzero++ }
  }
  Write-Host ("  帧尾余量    中位 {0} 字节（最小 {1} 最大 {2}）  正好=0:{3} 多出:{4} 少:{5}" -f `
    $gmed, $gs[0], $gs[$gs.Count - 1], $gzero, $gpos, $gneg)
  Write-Host "     帧尾余量 = 本帧 H.264 结束到下一个帧头之间的字节数（零填充 + CRC16）。"
  Write-Host "     一帧完整时它 ≈ 298 - 内层长度（内层 250 -> 约 48）；"
  Write-Host "     明显更小 = 帧尾零填充被切（H.264 没缺）；负数 = 连 H.264 都缺了。"
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
  Write-Host "  ✅ 收到有效 0x0310 帧 —— 板子确实在往这根线上发视频。"
  Write-Host "     （「完整帧」= 帧头 + 内层头 + H.264 齐全；帧尾零填充/CRC16 被切掉也算，"
  Write-Host "       真正看下一行「帧尾余量」：40~65 正常、明显更小 = 帧尾被切、负数 = 编码数据缺了）"
} elseif ($headHits -gt 0) {
  Write-Host "  ⚠ 这根线上有 0x0310 帧头（CRC8 都对），但没有一帧凑满 309 字节。"
  Write-Host "     先看上面「帧间隔」和「每帧差多少」两行，再下结论："
  Write-Host "       每帧差多少 中位 ≈ 0   -> 线上本来就是完整的一帧（帧尾零填充少一点），"
  Write-Host "                                 只是本工具按 309 字节算「完整」，别当成丢 H.264"
  Write-Host "       每帧差多少 总是正的、多出来的都是 0 -> 帧尾零填充+CRC 被按 64 字节取整切掉"
  Write-Host "                                 （USB CDC / DMA 边界），H.264 没丢"
  Write-Host "       每帧差多少 忽正忽负、乱字节多 -> 没共地 / 线接触不良 / 波特率不对"
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
