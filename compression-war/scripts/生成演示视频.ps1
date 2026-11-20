$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ffmpeg = Join-Path $root 'ffmpeg.exe'
if (-not (Test-Path -LiteralPath $ffmpeg)) { throw '请把带libx264编码器的ffmpeg.exe放到compression-war根目录，见README。' }
$source = Join-Path (Split-Path -Parent $root) 'luojiamaster\源码\tests\fixtures\测试画面.h265'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "找不到实车图传样片，请保留同级 luojiamaster 目录：$source"
}
$dir = Join-Path $root '素材'
[IO.Directory]::CreateDirectory($dir) | Out-Null
$output = Join-Path $dir '演示-实车图传.mp4'
$temporary = Join-Path $dir (([Guid]::NewGuid()).ToString() + '.mp4')
try {
    # 原录像有 120 帧；转成 30 fps 后循环两秒钟的画面，方便展示。
    $filter = 'fps=30,scale=960:540:flags=area,loop=loop=-1:size=60:start=0,setpts=N/(30*TB)'
    & $ffmpeg -hide_banner -loglevel error -nostdin -xerror -f hevc -i $source -an -vf $filter -t 15 -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -movflags +faststart -n $temporary
    if ($LASTEXITCODE) { throw '实车演示片生成失败，请查看上方 FFmpeg 原始错误。' }
    Move-Item -LiteralPath $temporary -Destination $output -Force
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
}
Write-Output "实车演示片：$output"
Write-Output '录像来自队内已有的官方图传实录。完整画面等比缩到 960×540，约 2 秒的内容循环播放 15 秒。'
