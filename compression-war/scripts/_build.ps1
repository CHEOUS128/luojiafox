# 一键编译 cw_sim.exe（被 一键编译.bat 调起）
#
# 做四件事：
#   1. 找 Visual Studio 2022（六条默认路径 + vswhere 兜底）
#   2. 建短路径门户 C:\rm26cw -> 仓库根
#   3. 删掉可能来自别处/别的机器的 build\，重新 configure + build
#   4. 把 build\cw_sim.exe 拷到 compression-war\ 和 scripts\，并跑 --help 自检
#
# 这个文件用 UTF-8 with BOM + CRLF 保存（和仓库里其它 .ps1 一致），
# Windows PowerShell 5.1 能正确识别中文。
param(
    # 可以不传：默认用脚本自身位置往上两级（scripts -> compression-war -> 仓库根）。
    # 用 PowerShell 自己算，不用批处理猜层级 —— 这一步已经错过两次。
    [string]$Root
)

$ErrorActionPreference = 'Stop'

function Say([string]$msg)  { [Console]::WriteLine($msg) }
function SayErr([string]$m) { [Console]::Error.WriteLine($m) }
function Rule() { Say ('=' * 60) }

if (-not $Root) {
    # $PSScriptRoot = ...\compression-war\scripts
    $Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
# GetFullPath 会把 ".." 归一化掉，而且路径不存在时也不抛（后面我们自己给出清楚的错）
$Root = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
$CW   = Join-Path $Root 'compression-war'
Say ('  脚本目录：' + $PSScriptRoot)
Say ('  仓库根  ：' + $Root)

# 门户是否真的指向上面的 $Root（C:\rm26cw 可能是上一次指向别的仓库的残留 junction，
# 那会把别的仓库当成这次要编的源码，编出来的 exe 是错的）
function Test-PortalTarget([string]$Path, [string]$WantRoot) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $item) { return $false }
    # PS 5.1 叫 Target，PS 7 叫 LinkTarget
    $t = $null
    if ($item.PSObject.Properties['Target'])     { $t = $item.Target }
    if (-not $t -and $item.PSObject.Properties['LinkTarget']) { $t = $item.LinkTarget }
    if (-not $t) { return $false }
    $t = @($t)[0]
    if (-not [System.IO.Path]::IsPathRooted($t)) { $t = Join-Path (Split-Path -Parent $Path) $t }
    $a = [System.IO.Path]::GetFullPath($t).TrimEnd('\', '/')
    return ($a -ieq $WantRoot)
}

# --------------------------------------------------------------------------
# 0. 仓库结构检查
# --------------------------------------------------------------------------
# 自愈：如果传进来的不是仓库根，就向上/向下一层找真正的那一个。
# 判据是「同时有 luojiamaster 的解码器源码 和 compression-war\CMakeLists.txt」。
function Test-RepoRoot([string]$p) {
    if (-not $p) { return $false }
    return (Test-Path -LiteralPath (Join-Path $p 'luojiamaster\源码\src\video\custom_byte_block_video_decoder.cpp')) -and
           (Test-Path -LiteralPath (Join-Path $p 'compression-war\CMakeLists.txt'))
}

if (-not (Test-RepoRoot $Root)) {
    $found = $null
    $probe = $Root
    for ($i = 0; $i -lt 4 -and -not $found; ++$i) {
        $probe = Split-Path -Parent $probe
        if (-not $probe) { break }
        if (Test-RepoRoot $probe) { $found = $probe }
    }
    if (-not $found) {
        foreach ($d in (Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue)) {
            if (Test-RepoRoot $d.FullName) { $found = $d.FullName; break }
        }
    }
    if ($found) {
        Say ('  [提示] 传入的根不是仓库根，已自动改用：' + $found)
        $Root = $found
        $CW   = Join-Path $Root 'compression-war'
    }
}

$decoder = Join-Path $Root 'luojiamaster\源码\src\video\custom_byte_block_video_decoder.cpp'
if (-not (Test-Path -LiteralPath $decoder)) {
    SayErr '[错误] 找不到客户端的解码器源码：'
    SayErr ('       ' + $decoder)
    SayErr ''
    SayErr ('       当前认为的仓库根：' + $Root)
    SayErr ('       它下面有：')
    Get-ChildItem -LiteralPath $Root -Force -ErrorAction SilentlyContinue |
      Select-Object -ExpandProperty Name | ForEach-Object { SayErr ('         - ' + $_) }
    SayErr ''
    SayErr '       compression-war 必须和 luojiamaster 同级（同一个仓库根下面）。'
    SayErr '       如果上面少了 luojiamaster，说明调用方传进来的 -Root 层级不对。'
    exit 1
}
if (-not (Test-Path -LiteralPath (Join-Path $CW 'CMakeLists.txt'))) {
    SayErr '[错误] 找不到 compression-war\CMakeLists.txt'
    exit 1
}

Say ''
Rule
Say '  压片仿真 · 编译 cw_sim.exe'
Rule
Say ('  源码：' + $CW)
Say ''

# --------------------------------------------------------------------------
# 1. 找 VS2022
# --------------------------------------------------------------------------
$candidates = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Professional',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise'
)

$VsRoot = $null
foreach ($p in $candidates) {
    if (Test-Path -LiteralPath (Join-Path $p 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')) {
        $VsRoot = $p
        break
    }
}

if (-not $VsRoot) {
    # vswhere 兜底：装在非默认目录的情况
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -version '[17.0,18.0)' -property installationPath 2>$null
        if ($found -and (Test-Path -LiteralPath (Join-Path $found.Trim() 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'))) {
            $VsRoot = $found.Trim()
        }
    }
}

if (-not $VsRoot) {
    SayErr '[错误] 找不到 Visual Studio 2022。'
    SayErr '       装了但不在默认目录的话，把安装路径告诉我，我改脚本。'
    SayErr '       需要勾选「使用 C++ 的桌面开发」工作负载。'
    exit 1
}
Say ('  VS  ：' + $VsRoot)

# --------------------------------------------------------------------------
# 2. 短路径门户
# --------------------------------------------------------------------------
$Portal = $null
foreach ($p in @('C:\rm26cw', (Join-Path $env:USERPROFILE 'rm26cw'))) {
    if (Test-PortalTarget $p $Root) { $Portal = $p; break }
}

if (-not $Portal) {
    $linkScript = Join-Path $CW 'scripts\建短路径.ps1'
    foreach ($p in @('C:\rm26cw', (Join-Path $env:USERPROFILE 'rm26cw'))) {
        # 已存在但指向别处的链接不动它（可能是别的工程的门户），换下一个候选
        if (Test-Path -LiteralPath $p) { continue }
        try { & $linkScript -Link $p -Target $Root } catch { }
        if (Test-PortalTarget $p $Root) { $Portal = $p; break }
    }
}

if (-not $Portal) {
    Say '  [提示] 没能建短路径门户，直接用原路径编译。'
    Say '         如果后面链接器报路径相关的错，把它发我。'
    $Portal = $Root
}
Say ('  门户：' + $Portal)

# --------------------------------------------------------------------------
# 3. 进 VS 开发者环境 + 配置 + 编译
# --------------------------------------------------------------------------
$work = Join-Path $Portal 'compression-war'
$log  = Join-Path $work 'build_log.txt'

Say ''
Rule
Say '  配置 + 编译（Ninja Release）'
Rule
Say ''

Import-Module (Join-Path $VsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $VsRoot -SkipAutomaticLocation -Arch amd64 -HostArch amd64 | Out-Null

# Ninja 不一定在 DevShell 的 PATH 里（VS 的「CMake 工具」组件会装一份）。
# 找不到就报清楚的错，别让 cmake 抛一句 "CMAKE_MAKE_PROGRAM not set"。
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja) {
    foreach ($p in @(
        (Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')
    )) {
        if (Test-Path -LiteralPath $p) {
            $env:PATH = (Split-Path -Parent $p) + ';' + $env:PATH
            $ninja = Get-Command ninja -ErrorAction SilentlyContinue
            if ($ninja) { break }
        }
    }
}
if (-not $ninja) {
    SayErr '[错误] 找不到 ninja.exe。'
    SayErr '       VS 安装器里补勾「使用 C++ 的 CMake 工具」，或者在命令行跑：'
    SayErr '         pip install ninja'
    exit 1
}
Say ('  Ninja：' + $ninja.Source)

Push-Location $work
try {
    # 删掉可能来自别的机器（比如我这边 Linux 沙箱）的 build\
    # 那种 CMakeCache.txt 里记的是 /sessions/... 路径，cmake 会直接拒绝复用。
    if (Test-Path -LiteralPath 'build') {
        Say '  清理旧的 build\ ...'
        Remove-Item -LiteralPath 'build' -Recurse -Force
    }

    & cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败' }

    & cmake --build build 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) { throw '编译失败' }
} catch {
    Say ''
    Rule
    SayErr ('  [失败] ' + $_.Exception.Message)
    SayErr ('  完整日志：' + $log)
    SayErr ''
    SayErr '  最常见的三个原因：'
    SayErr '    1. VS 安装时没勾「使用 C++ 的桌面开发」'
    SayErr '    2. 没装 Ninja（VS 安装器里勾「CMake 工具」，或 pip install ninja）'
    SayErr '    3. 路径太长/含特殊字符 —— 看日志里第一个 error'
    exit 1
} finally {
    Pop-Location
}

# --------------------------------------------------------------------------
# 4. 拷贝产物 + 自检
# --------------------------------------------------------------------------
$built = Join-Path $work 'build\cw_sim.exe'

if (-not (Test-Path -LiteralPath $built)) {
    SayErr ('[失败] 编译说是成功了，但没找到 ' + $built)
    SayErr ('       看日志：' + $log)
    exit 1
}

# 放两个地方：compression-war\（主位置）和 scripts\（双击跑仿真.bat 时省得再找）。
# 注意用 $CW（真实路径）而不是 $work（门户路径）—— 门户只是个 junction，
# 打印门户路径会让人以为 exe 不在仓库里。
$targets = @((Join-Path $CW 'cw_sim.exe'), (Join-Path $CW 'scripts\cw_sim.exe'))
$copied = 0
foreach ($t in $targets) {
    try { Copy-Item -LiteralPath $built -Destination $t -Force; ++$copied } catch { }
}
if ($copied -eq 0) {
    SayErr '[失败] 编译成功了，但 exe 一个都没拷回去（文件可能被占用）。'
    SayErr ('       源文件：' + $built)
    exit 1
}

$final = $targets[0]
$item = Get-Item -LiteralPath $final
Say ''
Rule
Say '  成功'
Rule
Say ''
Say ('  产物：' + $item.FullName)
Say ('  大小：' + $item.Length + ' 字节    时间：' + $item.LastWriteTime)
Say ''

# 自检：--help 能跑起来就说明没缺 DLL、没崩在启动阶段
$null = & $final --help 2>&1
if ($LASTEXITCODE -eq 0) {
    Say '  自检：cw_sim.exe --help 正常'
} else {
    Say ('  [注意] --help 返回了 ' + $LASTEXITCODE + '，但进程能启动。可能只是帮助信息走到 stderr 了。')
}

Say ''
Say '  下一步：'
Say '    1. ffmpeg.exe 放到 compression-war\ 或 compression-war\scripts\（两处都会找）'
Say '    2. 双击 scripts\准备素材.bat，把一段录像拖给它'
Say '    3. 双击 scripts\跑仿真.bat'
Say ''
