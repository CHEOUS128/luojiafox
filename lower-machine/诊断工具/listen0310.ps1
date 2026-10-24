param(
  [string]$Port = "",
  [int]$Baud = 921600,
  [double]$Seconds = 30.0,
  [switch]$NoCrc,                    # 不过滤 CRC（看原始帧率用）
  [switch]$Raw                       # 只统计字节/0xA5，不解析（怀疑波特率不对时用）
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936) } catch {}
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

Write-Host ("=== 监听 {0} @ {1} 8N1，{2} 秒（同时在听 PG14）===" -f $Port, $Baud, $Seconds)
Write-Host "接线（只接两根，接错会短路）："
Write-Host "  CH340 的 RX  ->  STM32 的 PG14（或 VT03 的 RX 焊盘）"
Write-Host "  CH340 的 GND ->  板子 GND（必须是板子的地，不能只接 USB 的地）"
Write-Host "  CH340 的 TX  ->  ★悬空不接★（STM32 正在驱动这根线）"
Write-Host ""

$buf      = New-Object byte[] 8192
$acc      = New-Object System.Collections.Generic.List[byte]
$all      = New-Object System.Collections.Generic.List[byte]
$seqs     = New-Object System.Collections.Generic.List[int]
$good     = 0
$badCrc   = 0
$badHead  = 0
$headOkCrcBad = 0
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
        $scan = 0
        while (($acc.Count - $scan) -ge 309) {
          if ($acc[$scan] -ne 0xA5) {
            $scan++
            $badHead++
            continue
          }
          # 结构不对就丢 1 个字节重新找；结构对但 CRC 不对就跳 309
          # （VT03 重传时可能就是同一个字节流原样再来一遍，跳 1 格会把它拆烂）
          $headOk = $true
          if ($acc[$scan + 1] -ne 0x2C -or $acc[$scan + 2] -ne 0x01) { $headOk = $false }
          if ($acc[$scan + 5] -ne 0x10 -or $acc[$scan + 6] -ne 0x03) { $headOk = $false }
          if (-not $headOk) {
            $scan++
            $badHead++
            continue
          }
          $frame = $acc.GetRange($scan, 309).ToArray()
          $ok = $true
          if (-not $NoCrc) {
            if ($frame[4] -ne [byte](Get-Crc8 ([byte[]]$frame[0..3]))) { $ok = $false }
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
            $recent.Enqueue(("seq={0,-3} 内层长度={1,-3} 内层序号={2}" -f $sq, $innerLen, $innerSeq))
            $scan += 309
          } else {
            $badCrc++
            $headOkCrcBad++
            $scan += 309
          }
        }
        if ($scan -gt 0) {
          $acc.RemoveRange(0, [Math]::Min($scan, $acc.Count))
        }
        if ($acc.Count -gt 8192) { $acc.RemoveRange(0, $acc.Count - 8192) }
      }
    }
    if ($sw.ElapsedMilliseconds -ge $nextReport) {
      $nextReport += 2000
      Write-Host ("  [{0,4:N0}s] 收到 {1,7:N0} 字节 | 有效帧 {2} 坏帧 {3} 乱字节 {4}" -f `
        $sw.Elapsed.TotalSeconds, $total, $good, $badCrc, $badHead)
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
Write-Host ("  有效 0x0310 {0} 帧（{1:N2} 帧/秒）" -f $good, ($good / [Math]::Max($elapsed, 0.001)))
Write-Host ("  CRC/帧头错  {0}     乱字节 {1}" -f $badCrc, $badHead)
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
  Write-Host "  ✅ STM32 确实在往 PG14 发 309 字节 0x0310，问题不在 C 板发送端。"
  Write-Host "     下一步查 VT03：TX 线是不是真接 PG14、VT03 串口配置、灯色。"
} elseif ($headOkCrcBad -gt 0) {
  Write-Host "  ⚠ 有帧头结构对的 309 字节块，但 CRC 全错。"
  Write-Host "     说明 PG14 上确实在吐 0x0310 形状的数据，但内容被改坏了："
  Write-Host "       - 波特率有偏差（确认 STM32 这边真是 921600）"
  Write-Host "       - 两个发送端并联在 PG14（SBUS 遥控接收机 + 视频），互相打乱"
  Write-Host "       - 线太长 / 没共地 / 接触不良"
} elseif ($total -gt 0) {
  Write-Host "  ⚠ 收到了字节，但一整帧都没凑齐（60 秒正常应是 9 万字节量级）。"
  Write-Host "     先看上面那份十六进制：以 A5 12 01 开头 -> 是视频帧，只是没发完/被截断；"
  Write-Host "     全是 00 或 FF -> 线悬空/没共地，收到的只是噪声；"
  Write-Host "     是规整重复的花纹 -> 波特率对不上。"
  Write-Host "     同时去 Jetson 上看一眼「成帧」还在不在涨（见下面的命令）。"
} else {
  Write-Host "  ❌ 一个字节都没收到。按下面顺序排查："
  Write-Host "     1) CH340 的 RX 是不是真接在 PG14（不是 VT03 的 TX），GND 是不是板子的 GND"
  Write-Host "     2) 先确认 Jetson 还在出帧：在 Jetson 上跑"
  Write-Host "        grep -a '1包/帧' ~/my_lf_INF_20261025/logs/autostart.log | tail -n 2"
  Write-Host "        两次间隔看「成帧」有没有涨；不涨说明 STM32 根本没收到视频（Jetson 卡住/线松）"
  Write-Host "     3) 换个 CH340 的口/换根线；万用表量 PG14 与 GND 之间是不是 3.3V 空闲电平"
}
