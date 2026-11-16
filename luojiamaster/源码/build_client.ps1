param([switch]$CI, [switch]$SkipConfigure, [switch]$Verify, [switch]$SyncDelivery,
      [ValidateRange(1,32)][int]$Parallel=6, [string[]]$Targets=@('luojiamaster'))
$ErrorActionPreference='Stop'
$taskExit=1
$script:BuildFailureCode=1
$originalLocation=Get-Location

function Get-FileMd5 {
    param([string]$LiteralPath)
    $stream=$null; $algorithm=$null
    try {
        $stream=[IO.File]::Open($LiteralPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        $algorithm=[Security.Cryptography.MD5]::Create()
        [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','')
    } finally {
        if ($algorithm) { $algorithm.Dispose() }
        if ($stream) { $stream.Dispose() }
    }
}

function Assert-SourceJunction {
    param([string]$SourceRoot,[string]$Portal)
    $link=Get-Item -LiteralPath $Portal -ErrorAction Stop
    if ($link.LinkType -ne 'Junction' -or @($link.Target).Count -ne 1) { throw 'C:\rm26build不是唯一目标的junction，停止编译。' }
    $target=[IO.Path]::GetFullPath([string]@($link.Target)[0]).TrimEnd('\')
    $expected=[IO.Path]::GetFullPath($SourceRoot).TrimEnd('\')
    if (-not $target.Equals($expected,[StringComparison]::OrdinalIgnoreCase)) { throw ('C:\rm26build指错目录。实际：'+$target+'；应该：'+$expected) }
}

function Invoke-BuildCommand {
    param([string]$Executable,[string[]]$Arguments,[string]$LogFile)
    $previousPreference=$ErrorActionPreference
    try {
        # Native warnings on stderr are not terminating PowerShell errors.
        $ErrorActionPreference='Continue'
        & $Executable @Arguments 2>&1 | Tee-Object -FilePath $LogFile -Append
        $nativeExit=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previousPreference }
    if ($nativeExit -ne 0) { $script:BuildFailureCode=$nativeExit;throw ('命令失败，退出码'+$nativeExit+'：'+$Executable) }
}

try {
    $sourceRoot=[IO.Path]::GetFullPath($PSScriptRoot)
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt'))) { throw '请保留本脚本在源码目录。' }
    $portal='C:\rm26build'
    if (-not (Test-Path -LiteralPath $portal)) { throw '缺少C:\rm26build。请按客户端README建立指向当前源码目录的junction，脚本不会猜旧目录。' }
    Assert-SourceJunction -SourceRoot $sourceRoot -Portal $portal
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot '3rdparty_downloads\boost\.unpacked'))) {
        throw '第三方依赖尚未按客户端README准备。先处理3rdparty_downloads/解压依赖.bat；本入口不下载或修改依赖。'
    }
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw '找不到VS Installer的vswhere.exe，请安装Visual Studio 2022 C++桌面开发环境。' }
    $vsInstall=@(& $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if ($LASTEXITCODE -ne 0 -or $vsInstall.Count -ne 1 -or -not $vsInstall[0]) { throw '未找到带C++工具链的Visual Studio 2022。' }
    $devShell=Join-Path $vsInstall[0] 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vsInstall[0] -SkipAutomaticLocation -Arch amd64 -HostArch amd64
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'VS开发环境初始化后仍未找到cl.exe，停止。' }
    $cmake=(Get-Command cmake.exe -ErrorAction Stop).Source
    $ctest=(Get-Command ctest.exe -ErrorAction Stop).Source
    Set-Location -LiteralPath $portal
    $buildDir=Join-Path $portal 'build\release'
    $logFile=Join-Path $sourceRoot 'cmake_build_log.txt'
    [IO.File]::WriteAllText($logFile,('RM26 build '+[DateTime]::Now.ToString('o')+"`r`n"),(New-Object Text.UTF8Encoding($false)))
    if (-not $SkipConfigure) { Invoke-BuildCommand -Executable $cmake -Arguments @('--preset','release','-DFETCHCONTENT_FULLY_DISCONNECTED=ON') -LogFile $logFile }
    foreach ($target in $Targets) { if ($target -notmatch '^[A-Za-z0-9_.+-]+$') { throw '构建目标名称无效。' } }
    $buildTargets=@($Targets)
    if ($Verify -or $SyncDelivery) {
        # CTest's current registration is authoritative; zero tests is not success.
        $previousPreference=$ErrorActionPreference
        try { $ErrorActionPreference='Continue';$registration=& $ctest --test-dir $buildDir --show-only=json-v1;$registrationExit=$LASTEXITCODE }
        finally { $ErrorActionPreference=$previousPreference }
        if ($registrationExit -ne 0) { $script:BuildFailureCode=$registrationExit;throw '无法读取本次CTest注册，停止验证和同步。' }
        $registered=($registration -join "`n") | ConvertFrom-Json
        $testNames=@($registered.tests | ForEach-Object { $_.name })
        if (-not $testNames.Count) { throw '当前配置没有CTest测试；不能把0项测试当作通过。请先启用BUILD_TESTING。' }
        foreach ($testName in $testNames) { if ($testName -notmatch '^[A-Za-z0-9_.+-]+$') { throw '注册测试名称不能作为当前项目构建目标，请维护人员核对。' } }
        $buildTargets=@(@('luojiamaster')+$buildTargets+$testNames | Select-Object -Unique)
    }
    Assert-SourceJunction -SourceRoot $sourceRoot -Portal $portal
    Invoke-BuildCommand -Executable $cmake -Arguments (@('--build','--preset','release','--parallel',[string]$Parallel,'--target')+$buildTargets) -LogFile $logFile
    if ($Verify -or $SyncDelivery) {
        $testXml=Join-Path $buildDir 'Testing\rm26-last-tests.xml'
        if (Test-Path -LiteralPath $testXml) { Remove-Item -LiteralPath $testXml }
        Invoke-BuildCommand -Executable $ctest -Arguments @('--test-dir',$buildDir,'--output-on-failure','--no-tests=error','--output-junit',$testXml) -LogFile $logFile
        if (-not (Test-Path -LiteralPath $testXml -PathType Leaf)) { throw 'CTest没有留下本次测试结果，禁止同步。' }
        [xml]$junit=[IO.File]::ReadAllText($testXml,[Text.Encoding]::UTF8)
        $executed=@($junit.testsuite.testcase | Where-Object { -not $_.SelectSingleNode('skipped') }).Count
        if ($executed -le 0) { throw '实际执行的CTest测试为0，禁止同步。' }
        Write-Host ('[已验证] 本次实际执行的CTest测试数：'+$executed)
    }
    $builtExe=Join-Path $buildDir 'luojiamaster.exe'
    if ($buildTargets -contains 'luojiamaster') {
        if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) { throw '构建结束但没有找到本次EXE。' }
        Get-Item -LiteralPath $builtExe | Select-Object FullName,Length,LastWriteTime | Format-List
        $builtMd5=(Get-FileMd5 -LiteralPath $builtExe)
        Write-Host ('编译EXE MD5：'+$builtMd5)
        if ($SyncDelivery) {
            if (Get-Process -Name luojiamaster -ErrorAction SilentlyContinue) { throw '客户端仍在运行，交付EXE未覆盖。先退出客户端再同步。' }
            $delivery=Join-Path ([IO.Path]::GetFullPath((Join-Path $sourceRoot '..\程序'))) 'luojiamaster.exe'
            Copy-Item -LiteralPath $builtExe -Destination $delivery -Force
            if ((Get-FileMd5 -LiteralPath $delivery) -ne $builtMd5) { throw '交付EXE的MD5不一致，同步未完成。' }
            Write-Host '[已同步] 程序/luojiamaster.exe与本次构建字节一致。'
        } else { Write-Host '[待同步] 未覆盖程序目录EXE。交付前运行本入口 -SyncDelivery，或按客户端README复制后核对MD5。' }
    }
    Write-Host ('构建日志：'+$logFile)
    $taskExit=0
} catch {
    Write-Host ('[失败] '+$_.Exception.Message) -ForegroundColor Red
    $taskExit=$script:BuildFailureCode
} finally {
    Set-Location -LiteralPath $originalLocation
    if (-not $CI -and $env:RM26_CI -ne '1') { [void](Read-Host '按回车关闭') }
}
exit $taskExit
