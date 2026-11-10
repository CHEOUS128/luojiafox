param(
    [string]$InputPath = '',
    [string[]]$Profiles = @('baseline','one-160','one-320','one-320-aq0','one-320-gop20','gray','chroma4','temporal16','sharp'),
    [double]$Seconds = 15,
    [string]$OutputPath = '',
    [string]$Python = ''
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Python) {
    $bundled=Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if (Test-Path -LiteralPath $bundled) { $Python=$bundled }
    elseif (Get-Command python.exe -ErrorAction SilentlyContinue) { $Python=(Get-Command python.exe).Source }
    else { throw '需要Python 3和numpy、Pillow；可用-Python指定python.exe。' }
}
& $Python -c 'import numpy; from PIL import Image'
if ($LASTEXITCODE) { throw '此Python缺少依赖，请用它执行：python -m pip install numpy pillow' }
if (-not $InputPath) { $InputPath=Join-Path $root '素材\car_raw.bgr' }
if (-not (Test-Path -LiteralPath $InputPath)) { throw '原片不存在，请用-InputPath指定已有视频。' }
if (-not $OutputPath) { $OutputPath=Join-Path $root ('runs\strategies\'+(Get-Date -Format 'yyyyMMdd_HHmmss')) }
$arguments=@((Join-Path $root 'tools\run_strategies.py'),'--root',$root,'--input',([IO.Path]::GetFullPath($InputPath)),'--out',([IO.Path]::GetFullPath($OutputPath)),'--duration',"$Seconds",'--profiles')+$Profiles
& $Python @arguments
if ($LASTEXITCODE) { throw '策略运行或独立验证失败，请查看会话目录中的原始日志。' }
$html=Join-Path ([IO.Path]::GetFullPath($OutputPath)) 'index.html'
$latest=Join-Path $root 'runs\strategies\latest-comparison.txt'
[IO.File]::WriteAllText($latest,$html,[Text.UTF8Encoding]::new($false))
Write-Output "对比入口：$html"
Start-Process -FilePath $html
