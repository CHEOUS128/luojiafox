param([string]$Path='')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Path) {
    $latest=Join-Path $root 'runs\strategies\latest-comparison.txt'
    if (-not (Test-Path -LiteralPath $latest)) { throw '还没有本地对比结果，请先运行“运行-策略对比.ps1”。' }
    $Path=[IO.File]::ReadAllText($latest,[Text.Encoding]::UTF8).Trim()
}
if (-not (Test-Path -LiteralPath $Path)) { throw "对比文件不存在：$Path" }
Start-Process -FilePath ([IO.Path]::GetFullPath($Path))
