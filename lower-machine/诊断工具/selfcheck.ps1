param(
  [string]$Port = "COM7",
  [int]$Baud = 921600,
  [double]$Seconds = 20.0
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936) } catch {}
. (Join-Path $PSScriptRoot 'crc0310.ps1')

# ===== 载入样本帧 =====
$fp = Join-Path $PSScriptRoot 'H264镜头样本.bin'
if (-not (Test-Path $fp)) { Write-Host "找不到 H264镜头样本.bin"; return }
$raw = [System.IO.File]::ReadAllBytes($fp)
$cnt = [int]$raw[0] -bor ([int]$raw[1] -shl 8)
$list = New-Object System.Collections.Generic.List[byte[]]
$p = 2
for ($i = 0; $i -lt $cnt; $i++) {
  $n = [int]$raw[$p] -bor ([int]$raw[$p + 1] -shl 8); $p += 2
  $tmp = New-Object byte[] $n
  [System.Array]::Copy($raw, $p, $tmp, 0, $n)
  $list.Add($tmp) | Out-Null
  $p += $n
}

function New-Frame309([int]$seq, [byte[]]$payload) {
  $f = New-Object byte[] 309
  $f[0] = 0xA5; $f[1] = 300 -band 0xFF; $f[2] = (300 -shr 8) -band 0xFF
  $f[3] = $seq -band 0xFF
  $f[4] = [byte](Get-Crc8 ([byte[]]$f[0..3]))
  $f[5] = 0x10; $f[6] = 0x03
  $plen = [Math]::Min($payload.Length, 296)
  $il = 2 + $plen
  $t = New-Object byte[] 300
  $t[0] = ($il -shr 8) -band 0xFF; $t[1] = $il -band 0xFF
  $t[2] = ($seq -shr 8) -band 0xFF; $t[3] = $seq -band 0xFF
  [System.Array]::Copy($payload, 0, $t, 4, $plen)
  [System.Array]::Copy($t, 0, $f, 7, 300)
  $c = Get-Crc16 ([byte[]]$f[0..306])
  $f[307] = $c -band 0xFF; $f[308] = ($c -shr 8) -band 0xFF
  return ,$f
}

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port; $sp.BaudRate = $Baud; $sp.Parity = "None"
$sp.DataBits = 8; $sp.StopBits = "One"; $sp.Handshake = "None"
$sp.ReadTimeout = 150; $sp.WriteTimeout = 2000
Write-Host "=== CH340 自检：TX 与 RX 短接，发真帧看能收回多少 ==="
Write-Host "接线：只把 CH340 自己的 TX 和 RX 两根针用一根线接起来。"
Write-Host "      ★ 不要接板子、不要接 VT03、不要碰 12V ★"
Write-Host ("串口 {0} @ {1}，{2} 秒，每 200ms 一帧（309 字节）" -f $Port, $Baud, $Seconds)
Write-Host ""
try { $sp.Open() } catch {
  Write-Host ("打不开 {0}：{1}" -f $Port, $_.Exception.Message)
  Write-Host "  先把串口助手 / RoboGo / Keil 关掉。"
  return
}

$buf = New-Object byte[] 8192
$acc = New-Object System.Collections.Generic.List[byte]
$accBase = 0
$lastHeadPos = -1
$headHits = 0; $good = 0; $short = 0; $total = 0
$sent = 0
$sw = [System.Diagnostics.Stopwatch]::StartNew()

function Scan-Acc {
  $script:scan = 0
  while (($script:acc.Count - $script:scan) -ge 9) {
    if ($script:acc[$script:scan] -ne 0xA5) { $script:scan++; continue }
    $s = $script:scan
    $ok = $true
    if ($script:acc[$s+1] -ne 0x2C -or $script:acc[$s+2] -ne 0x01) { $ok = $false }
    if ($script:acc[$s+5] -ne 0x10 -or $script:acc[$s+6] -ne 0x03) { $ok = $false }
    if ($ok) {
      $h4 = New-Object byte[] 4
      for ($k = 0; $k -lt 4; $k++) { $h4[$k] = $script:acc[$s + $k] }
      if ($script:acc[$s+4] -ne [byte](Get-Crc8 $h4)) { $ok = $false }
    }
    if (-not $ok) { $script:scan++; continue }

    $abs = $script:accBase + $s
    if ($abs -ne $script:lastHeadPos) {
      $script:headHits++
      $script:lastHeadPos = $abs
    }
    if (($script:acc.Count - $s) -lt 309) { break }

    $frame = $script:acc.GetRange($s, 309).ToArray()
    $crcRecv = [int]$frame[307] -bor ([int]$frame[308] -shl 8)
    if ($crcRecv -eq (Get-Crc16 ([byte[]]$frame[0..306]))) {
      $script:good++
      $script:scan += 309
    } else {
      $script:short++
      $script:scan++
    }
  }
  if ($script:scan -gt 0) {
    $d = [Math]::Min($script:scan, $script:acc.Count)
    $script:acc.RemoveRange(0, $d)
    $script:accBase += $d
  }
}

try {
  $sp.DiscardInBuffer()
  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $payload = $list[$sent % $list.Count]
    $f = New-Frame309 $sent $payload
    try { $sp.Write($f, 0, 309) } catch {
      Write-Host ("写失败：{0}" -f $_.Exception.Message); break
    }
    $sent++
    Start-Sleep -Milliseconds 200
    for ($k = 0; $k -lt 8; $k++) {
      $n = 0
      try { $n = $sp.Read($buf, 0, $buf.Length) } catch [System.TimeoutException] { $n = 0 } catch { $n = 0 }
      if ($n -gt 0) {
        $total += $n
        for ($i = 0; $i -lt $n; $i++) { $acc.Add($buf[$i]) }
        $accBase += [Math]::Max(0, $acc.Count - $n)
      }
      Scan-Acc
      if ($sp.BytesToRead -le 0) { break }
    }
  }
} finally {
  Start-Sleep -Milliseconds 500
  for ($k = 0; $k -lt 10; $k++) {
    $n = 0
    try { $n = $sp.Read($buf, 0, $buf.Length) } catch { $n = 0 }
    if ($n -gt 0) {
      $total += $n
      for ($i = 0; $i -lt $n; $i++) { $acc.Add($buf[$i]) }
      $accBase += [Math]::Max(0, $acc.Count - $n)
      Scan-Acc
    } else { break }
  }
  try { $sp.Close() } catch {}
  $sp.Dispose()
}

Write-Host ""
Write-Host "=== 结果 ==="
Write-Host ("  发出       {0} 帧（{1} 字节）" -f $sent, ($sent * 309))
Write-Host ("  收回字节   {0}（{1:P1}）" -f $total, ($total / [Math]::Max(1, $sent * 309)))
Write-Host ("  收全帧     {0} 帧" -f $good)
Write-Host ("  帧头       {0} 个   残帧 {1}" -f $headHits, $short)
Write-Host ""
if ($sent -gt 0) {
  $r = $good / [double]$sent
  if ($r -ge 0.95) {
    Write-Host ("  OK：收回 {0:P1}。CH340 的 tap 没问题 —— 12:38 抓包里每帧缺字节就是板子线上在丢。" -f $r)
  } elseif ($r -ge 0.5) {
    Write-Host ("  ~ 收回 {0:P1}：CH340 有点跟不上，但没到 7.9% 那么惨。" -f $r)
    Write-Host "     12:38 那份抓包的 7.9% 不能直接归给 CH340，也不能全归给板子。"
  } else {
    Write-Host ("  X 只收回 {0:P1}：CH340 + 这个驱动在 921600 下自己就严重丢包。" -f $r)
    Write-Host "     12:38 那份抓包「21/266 帧」极可能就是 CH340 自己丢的，不代表板子。"
  }
  Write-Host ""
  Write-Host "  判读：收全帧接近发出帧数 -> 这个 tap 可信；差很多 -> 以后 CH340 的抓包只能当『有帧』的证据，"
  Write-Host "        不能当『丢了多少帧』的证据。"
}
