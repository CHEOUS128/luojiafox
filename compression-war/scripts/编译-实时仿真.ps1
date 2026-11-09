$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\')
$portalPath = 'C:\rm26cw'
$portal = Get-Item -LiteralPath $portalPath -Force -ErrorAction SilentlyContinue
if (-not $portal) {
    New-Item -ItemType Junction -Path $portalPath -Target $repo -ErrorAction Stop | Out-Null
    $portal = Get-Item -LiteralPath $portalPath -Force
}
$target = if ($portal.Target) { [IO.Path]::GetFullPath(@($portal.Target)[0]).TrimEnd('\') } else { '' }
if ($portal.LinkType -ne 'Junction' -or $target -ine $repo) { throw "C:\rm26cw 指向错误：$target；应为 $repo。请先核对，不会自动覆盖。" }
Write-Output "构建门户：$portalPath -> $target"
$vs = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $found = @(& $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if ($found.Count) { $vs = $found[0].Trim() }
}
if (-not $vs) {
    foreach ($edition in @('Community','Professional','Enterprise','BuildTools')) {
        $candidate = Join-Path $env:ProgramFiles ('Microsoft Visual Studio\2022\'+$edition)
        if (Test-Path -LiteralPath (Join-Path $candidate 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')) { $vs=$candidate;break }
    }
}
if (-not $vs) { throw '找不到VS2022 C++工具。请安装“使用C++的桌面开发”、Windows SDK、CMake和Ninja。' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -Arch amd64 -HostArch amd64 | Out-Null
foreach ($tool in @('cmake','ninja')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        $bundled = if ($tool -eq 'cmake') { 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin' } else { 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja' }
        $toolDir = Join-Path $vs $bundled
        if (Test-Path -LiteralPath (Join-Path $toolDir ($tool+'.exe'))) { $env:PATH=$toolDir+';'+$env:PATH }
    }
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "找不到$tool，请安装VS的C++ CMake工具组件。" }
}
$work = Join-Path $portalPath 'compression-war'
Push-Location $work
try {
    & cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE) { throw 'CMake配置失败' }
    & cmake --build build --target cw_live cw_wire_verify cw_decoder_reference cw_decoder_observed
    if ($LASTEXITCODE) { throw '编译失败' }
    & .\build\cw_live.exe --self-test
    if ($LASTEXITCODE) { throw '协议自检失败' }
    foreach ($name in @('cw_live.exe','cw_wire_verify.exe')) {
        Copy-Item -LiteralPath (Join-Path 'build' $name) -Destination $name -Force
        Get-Item -LiteralPath $name | Select-Object FullName,Length,LastWriteTime | Format-List
        Get-FileHash -LiteralPath $name -Algorithm MD5 | Format-List
    }
} finally { Pop-Location }
