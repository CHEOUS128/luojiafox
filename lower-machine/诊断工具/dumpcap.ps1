param(
  [string]$Path = "",
  [int]$MaxShow = 8
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding(936) } catch {}

if ($Path -eq "") {
  $root = 'C:\Users\CH\Desktop\robot记录\luojiafox\luojiamaster\程序\logs'
  if (-not (Test-Path $root)) { Write-Host ("找不到日志目录：{0}" -f $root); return }
  $dir = Get-ChildItem $root -Directory | Sort-Object Name -Descending | Select-Object -First 1
  if ($null -eq $dir) { Write-Host "日志目录是空的。客户端跑过吗？"; return }
  $Path = Join-Path $dir.FullName 'network_capture.bin'
  Write-Host ("最新会话：{0}" -f $dir.Name)
}
if (-not (Test-Path $Path)) { Write-Host ("找不到 {0}" -f $Path); return }

$b = [System.IO.File]::ReadAllBytes($Path)
Write-Host ("文件 {0}，{1:N0} 字节" -f (Split-Path $Path -Leaf), $b.Length)

$off = 16
$cbb = New-Object System.Collections.Generic.List[byte[]]
$recs = 0
$bad = 0
while ($off + 12 -le $b.Length) {
  if ($b[$off] -ne 0x52 -or $b[$off+1] -ne 0x45 -or $b[$off+2] -ne 0x43 -or $b[$off+3] -ne 0x32) { $bad++; break }
  $bs = [BitConverter]::ToUInt32($b, $off + 4)
  $off += 12
  if ($off + $bs -gt $b.Length) { break }
  # 头 18 字节：u64 t_us | u8 transport | u8 dir | u16 chlen | u16 peerlen | u32 plen
  $chlen = [BitConverter]::ToUInt16($b, $off + 10)
  $peerlen = [BitConverter]::ToUInt16($b, $off + 12)
  $plen = [BitConverter]::ToUInt32($b, $off + 14)
  $dirFlag = [int]$b[$off + 9]
  $p = $off + 18
  $ch = [System.Text.Encoding]::UTF8.GetString($b, $p, $chlen)
  $p += $chlen + $peerlen
  if ($ch -eq 'CustomByteBlock' -and $dirFlag -eq 1 -and ($p + $plen) -le $b.Length) {
    $payload = New-Object byte[] $plen
    [System.Array]::Copy($b, $p, $payload, 0, $plen)
    $cbb.Add($payload) | Out-Null
  }
  $recs++
  $off += $bs
}
if ($bad -gt 0) { Write-Host "  ! 记录在中间损坏，只解析了前面的部分" }

Write-Host ("总记录 {0} 条（其中 CustomByteBlock(rx) {1} 条）" -f $recs, $cbb.Count)
if ($cbb.Count -eq 0) {
  Write-Host ""
  Write-Host "  X 一条 CustomByteBlock 都没收到 —— 这一跳还是不通。"
  return
}

# 去掉 protobuf 头（0x0A + varint 长度），剩下才是 300 字节 data
$lens = @{}
$innerLens = @{}
$seqs = New-Object System.Collections.Generic.List[int]
$badParse = 0
$okPkt = 0
foreach ($pl in $cbb) {
  $lens[$pl.Length] = 1 + [int]$lens[$pl.Length]
  if ($pl.Length -lt 4 -or $pl[0] -ne 0x0A) { $badParse++; continue }
  $k = 1; $n = 0; $shift = 0
  while ($k -lt $pl.Length) {
    $v = [int]$pl[$k]
    $n = $n -bor (($v -band 0x7F) -shl $shift)
    $k++
    if (($v -band 0x80) -eq 0) { break }
    $shift += 7
    if ($shift -gt 14) { break }
  }
  $d = New-Object byte[] $n
  if ($k + $n -gt $pl.Length) { $badParse++; continue }
  [System.Array]::Copy($pl, $k, $d, 0, $n)
  $r = 0; $did = $false
  while ($r + 2 -le $d.Length) {
    $sz = ([int]$d[$r] -shl 8) -bor [int]$d[$r + 1]
    if ($sz -eq 0) { break }
    if ($r + 2 + $sz -gt $d.Length) { $badParse++; break }
    $innerLens[$sz] = 1 + [int]$innerLens[$sz]
    if ($sz -ge 3) { $seqs.Add((([int]$d[$r + 2] -shl 8) -bor [int]$d[$r + 3])) | Out-Null }
    $r += 2 + $sz
    $did = $true
  }
  if ($did) { $okPkt++ }
}

Write-Host ""
Write-Host "=== CustomByteBlock 负载长度 ==="
foreach ($k in ($lens.Keys | Sort-Object)) { Write-Host ("  {0,-4} 字节  x{1}" -f $k, $lens[$k]) }
Write-Host ("  能解出内层包的 {0}/{1}，解不开的 {2}" -f $okPkt, $cbb.Count, $badParse)

if ($innerLens.Count -gt 0) {
  Write-Host ""
  Write-Host "=== 内层包长度分布（298=定长，245~261=变化长度）==="
  $keys = $innerLens.Keys | Sort-Object
  $s = ""
  foreach ($k in $keys) { $s += ("{0}x{1}  " -f $k, $innerLens[$k]) }
  Write-Host ("  " + $s)
}

if ($seqs.Count -ge 2) {
  $gaps = @{}
  for ($i = 1; $i -lt $seqs.Count; $i++) {
    $g = $seqs[$i] - $seqs[$i - 1]
    $gaps[$g] = 1 + [int]$gaps[$g]
  }
  Write-Host ""
  $gs = ""
  foreach ($k in ($gaps.Keys | Sort-Object)) { $gs += ("+{0} x{1}  " -f $k, $gaps[$k]) }
  Write-Host ("  内层序号 {0} → {1}，共 {2} 个；步长分布：{3}" -f $seqs[0], $seqs[$seqs.Count - 1], $seqs.Count, $gs)
}

Write-Host ""
Write-Host "=== 前几条原始 payload ==="
$n = [Math]::Min($MaxShow, $cbb.Count)
for ($i = 0; $i -lt $n; $i++) {
  $h = ""
  $m = [Math]::Min(20, $cbb[$i].Length)
  for ($j = 0; $j -lt $m; $j++) { $h += ("{0:X2} " -f $cbb[$i][$j]) }
  Write-Host ("  [{0}] {1} ({2} 字节)" -f $i, $h, $cbb[$i].Length)
}
