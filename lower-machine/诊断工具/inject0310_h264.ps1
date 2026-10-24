param(
  [string]$Port = "",
  [double]$Hz = 5.0,
  [double]$Seconds = 60.0,
  [int]$StartSeq = 0,
  [int]$Baud = 921600,
  [string]$Frames = "",
  [switch]$ListPorts
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936) } catch {}
. (Join-Path $PSScriptRoot 'crc0310.ps1')

if ($ListPorts -or $Port -eq "" -or $Port -eq "ports") {
  Show-Ports0310
  Write-Host ""
  Write-Host "  下一步：inject_h264.bat COM7"
  return
}

if ($Frames -eq "") { $Frames = Join-Path $PSScriptRoot 'H264镜头样本.bin' }
if (-not (Test-Path $Frames)) {
  Write-Host ("找不到样本文件：{0}" -f $Frames)
  return
}

# ===== 读 H.264 样本（格式：[u16 帧数] 然后每帧 [u16 长度][数据]）=====
$raw = [System.IO.File]::ReadAllBytes($Frames)
$count = [int]$raw[0] -bor ([int]$raw[1] -shl 8)
$list = New-Object System.Collections.Generic.List[byte[]]
$p = 2
for ($i = 0; $i -lt $count; $i++) {
  if ($p + 2 -gt $raw.Length) { break }
  $n = [int]$raw[$p] -bor ([int]$raw[$p + 1] -shl 8)
  $p += 2
  if ($p + $n -gt $raw.Length) { break }
  $tmp = New-Object byte[] $n
  [System.Array]::Copy($raw, $p, $tmp, 0, $n)
  $list.Add($tmp) | Out-Null
  $p += $n
}
if ($list.Count -eq 0) { Write-Host "样本里没有帧。"; return }

# ===== 组 309 字节整帧，内层长度 = 2 + 真实帧长（这才是 STM32 在发的形状）=====
function New-Frame309([int]$seq, [byte[]]$payload) {
  $f = New-Object byte[] 309
  $f[0] = 0xA5
  $f[1] = 300 -band 0xFF
  $f[2] = (300 -shr 8) -band 0xFF
  $f[3] = $seq -band 0xFF
  $f[4] = [byte](Get-Crc8 ([byte[]]$f[0..3]))
  $f[5] = 0x10
  $f[6] = 0x03

  $plen = $payload.Length
  if ($plen -gt 296) { $plen = 296 }
  $innerLen = 2 + $plen

  $t = New-Object byte[] 300
  $t[0] = ($innerLen -shr 8) -band 0xFF
  $t[1] = $innerLen -band 0xFF
  $t[2] = ($seq -shr 8) -band 0xFF
  $t[3] = $seq -band 0xFF
  if ($plen -gt 0) { [System.Array]::Copy($payload, 0, $t, 4, $plen) }
  # t[4+plen .. 299] 保持 0 = 客户端认的「填充结束」标记

  [System.Array]::Copy($t, 0, $f, 7, 300)
  $c = Get-Crc16 ([byte[]]$f[0..306])
  $f[307] = $c -band 0xFF
  $f[308] = ($c -shr 8) -band 0xFF
  return ,$f
}

$periodMs = [int][Math]::Max(1, [Math]::Round(1000.0 / [Math]::Max($Hz, 0.1)))
$total = [int]($Hz * $Seconds)

Write-Host "=== 实验三：用真 H.264 + 变化的包长，灌进 VT03 ==="
Write-Host ("波特率 {0} 8N1，{1} 包/秒，{2} 秒，共约 {3} 包" -f $Baud, $Hz, $Seconds, $total)
Write-Host ("样本：{0}，{1} 帧，帧长 {2}~{3} 字节（内层长度 {4}~{5}）" -f `
  (Split-Path $Frames -Leaf), $list.Count, `
  (($list | ForEach-Object { $_.Length } | Measure-Object -Minimum).Minimum), `
  (($list | ForEach-Object { $_.Length } | Measure-Object -Maximum).Maximum), `
  (2 + ($list | ForEach-Object { $_.Length } | Measure-Object -Minimum).Minimum), `
  (2 + ($list | ForEach-Object { $_.Length } | Measure-Object -Maximum).Maximum))
Write-Host ""
Write-Host "接线（和 11:15 那次一模一样）："
Write-Host "  PC 的 TX  ->  VT03 的 RX"
Write-Host "  PC 的 GND ->  VT03 的 GND"
Write-Host "  STM32 的 PG14 先拔掉（两个发送端不能顶在一起）"
Write-Host "  VT03 用自己的 12V 供电"
Write-Host ""

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName     = $Port
$sp.BaudRate     = $Baud
$sp.Parity       = "None"
$sp.DataBits     = 8
$sp.StopBits     = "One"
$sp.Handshake    = "None"
$sp.ReadTimeout  = 2000
$sp.WriteTimeout = 2000

Write-Host ("正在打开 {0} ..." -f $Port)
try { $sp.Open() } catch {
  Write-Host ("打不开 {0}：{1}" -f $Port, $_.Exception.Message)
  Write-Host "  被占用 -> 关掉占用它的程序；串口号不对 -> 双击 bat 看列表。"
  return
}
Write-Host ("已打开 {0} @ {1}" -f $Port, $Baud)
try { $sp.DtrEnable = $true } catch {}
try { $sp.RtsEnable = $true } catch {}

$seq = $StartSeq -band 0xFFFF
$sent = 0
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$nextReport = 2000
try {
  try { $sp.DiscardInBuffer() } catch {}
  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $payload = $list[$sent % $list.Count]
    $f = New-Frame309 $seq $payload
    try { $sp.Write($f, 0, 309) } catch {
      Write-Host ("  写串口失败（第 {0} 包）：{1}" -f $sent, $_.Exception.Message)
      break
    }
    $sent++
    $seq = ($seq + 1) -band 0xFFFF
    Start-Sleep -Milliseconds $periodMs
    if ($sw.ElapsedMilliseconds -ge $nextReport) {
      $nextReport += 2000
      $rate = $sent / [Math]::Max($sw.Elapsed.TotalSeconds, 0.001)
      Write-Host ("  已发 {0} 包（{1:N1} 秒，实际 {2:N1} 包/秒）" -f $sent, $sw.Elapsed.TotalSeconds, $rate)
    }
  }
} finally {
  Start-Sleep -Milliseconds 300
  try { $sp.Close() } catch {}
  $sp.Dispose()
}

$elapsed = $sw.Elapsed.TotalSeconds
Write-Host ""
Write-Host ("=== 发完了：{0} 包，{1:N1} 秒，平均 {2:N1} 包/秒 ===" -f $sent, $elapsed, ($sent / [Math]::Max($elapsed, 0.001)))
Write-Host "现在关掉 RoboGo，dump 这一场的 network_capture.bin，看 CustomByteBlock 有几条。"
