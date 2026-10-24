param(
  [string]$Port = "",
  [int]$Baud = 921600,
  [double]$Seconds = 30.0,
  [double]$Hz = 0.0,                 # 0 = 自动：真视频应为 5Hz 左右，灌包按 1Hz 算
  [switch]$NoCrc,                    # 不过滤 CRC（看原始帧率用）
  [switch]$Raw                       # 只统计字节/0xA5，不解析（怀疑波特率不对时用）
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936) } catch {}
. (Join-Path $PSScriptRoot 'crc0310.ps1')

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
Write-Host "  CH340 的 GND ->  板子 GND"
Write-Host "  CH340 的 TX  ->  ★悬空不接★（STM32 正在驱动这根线）"
Write-Host ""
Write-Host "判读："
Write-Host "  有 0xA5 + 309B + CRC 全过  -> STM32 发了，断点在 VT03 这一侧的接线/配置"
Write-Host "  一个 0xA5 都没有           -> STM32 根本没往 PG14 发（USB 没收到 / 解析没收满 / 任务没跑）"
Write-Host ""

$buf      = New-Object byte[] 8192
$acc      = New-Object System.Collections.Generic.List[byte]
$seqs     = New-Object System.Collections.Generic.List[int]
$good     = 0
$badCrc   = 0
$badHead  = 0
$headOkCrcBad = 0
$total    = 0
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
    }
    if ($n -gt 0) {
      $total += $n
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
if ($good -gt 0) {
  $uniq = ($seqs | Sort-Object -Unique).Count
  Write-Host ("  外序号      {0} → {1}，不同值 {2} 个" -f $firstSeq, $lastSeq, $uniq)
  Write-Host "  最近 5 帧："
  foreach ($s in $recent) { Write-Host ("    " + $s) }
  Write-Host ""
  Write-Host "  ✅ STM32 确实在往 PG14 发 309 字节 0x0310，问题不在 C 板发送端。"
  Write-Host "     下一步查 VT03：TX 线是不是真接 PG14、VT03 串口波特率/协议配置、灯色。"
} elseif ($headOkCrcBad -gt 0) {
  Write-Host ""
  Write-Host "  ⚠ 有帧头结构对的 309 字节块，但 CRC 全错。"
  Write-Host "     说明 PG14 上确实在吐 0x0310 形状的数据，但内容被改坏了："
  Write-Host "       - 波特率有偏差（CH340 也能撑 921600，但要确认 STM32 这边真是 921600）"
  Write-Host "       - 两个发送端并联在 PG14（SBUS 遥控接收机 + 视频），互相打乱"
  Write-Host "       - 线太长/没共地/接触不良"
} else {
  Write-Host ""
  Write-Host "  ❌ 一个有效 0x0310 都没抓到。按下面顺序排查："
  Write-Host "     1) 先确认接线：CH340 RX 接 PG14，GND 共地，STM32 在跑（灯闪）"
  Write-Host "     2) 加 -Raw 重跑，看有没有任何字节/0xA5（区分『没发』和『波特率不对』）"
  Write-Host "     3) 若 -Raw 也全 0 -> 换个串口口/换根线，或 STM32 真没发"
  Write-Host "     4) 若 -Raw 有大量字节但没成帧 -> 波特率不是 921600，或 PG14 上有别的东西在发（SBUS）"
}
