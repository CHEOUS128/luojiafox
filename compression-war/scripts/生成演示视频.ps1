$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ffmpeg = Join-Path $root 'ffmpeg.exe'
if (-not (Test-Path -LiteralPath $ffmpeg)) { throw '请把带libx264编码器的ffmpeg.exe放到compression-war根目录，见README。' }
$dir = Join-Path $root '素材'
[IO.Directory]::CreateDirectory($dir) | Out-Null
$output = Join-Path $dir '演示-四角运动.mp4'
if (Test-Path -LiteralPath $output) { Write-Output "已存在演示视频，直接使用：$output";exit 0 }
$marks = 'drawbox=x=0:y=0:w=48:h=48:color=red:t=fill,drawbox=x=iw-48:y=0:w=48:h=48:color=green:t=fill,drawbox=x=0:y=ih-48:w=48:h=48:color=blue:t=fill,drawbox=x=iw-48:y=ih-48:w=48:h=48:color=yellow:t=fill'
& $ffmpeg -hide_banner -loglevel error -f lavfi -i 'testsrc2=s=640x360:r=30:d=15' -vf $marks -c:v libx264 -preset ultrafast -pix_fmt yuv420p -n $output
if ($LASTEXITCODE) { throw '演示视频生成失败，请查看上方FFmpeg原始错误。' }
Write-Output "演示视频：$output"
Write-Output '15秒、640x360、30fps；仅作合成运动演示，不替代真实相机验收素材。'
