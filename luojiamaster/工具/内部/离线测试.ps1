param([ValidateSet('menu','one','two','choose')][string]$Mode='menu', [string]$Capture='',
      [ValidateRange(1,3600)][int]$Seconds=20, [switch]$NoLaunch, [switch]$CI)
$ErrorActionPreference='Stop'
$script:ClientRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$script:ProgramRoot=Join-Path $script:ClientRoot '程序'
$script:ClientExe=Join-Path $script:ProgramRoot 'luojiamaster.exe'

function Get-OfflineArguments {
    param([string]$CapturePath,[string]$ReportPath,[int]$Duration)
    if ($CapturePath.IndexOfAny(@([char]0,[char]10,[char]13,[char]34)) -ge 0 -or $ReportPath.IndexOfAny(@([char]0,[char]10,[char]13,[char]34)) -ge 0) { throw '路径含不支持的字符。' }
    @('--offline-video-test',('--capture='+$CapturePath),('--seconds='+$Duration),
      '--no-window-exercise',('--report-dir='+$ReportPath))
}

function Quote-ProcessArguments {
    param([string[]]$Values)
    # Values are fixed options or Windows file paths (no embedded quotes/trailing slash).
    ($Values | ForEach-Object { if ($_.Contains('"') -or $_.EndsWith('\')) { throw '参数格式无效。' }; '"'+$_+'"' }) -join ' '
}

if ($NoLaunch) { return }
try {
    Write-Host '离线测试只读取本机录包，不连接MQTT，不产生键鼠控制。'
    Write-Host '默认20秒，只用于冒烟检查，不代表完整稳定性或队友互联验收。'
    if ($CI -and $Mode -eq 'menu') { $Mode='one' }
    if ($Mode -eq 'menu') {
        Write-Host '1 回放内置双路样本（直接回车）  2 同时开两个独立回放窗口  3 选择其他录包  0 退出'
        $selection=(Read-Host '选择').Trim()
        if ($selection -eq '0') { exit 0 }
        if ($selection -eq '' -or $selection -eq '1') { $Mode='one' }
        elseif ($selection -eq '2') { $Mode='two' }
        elseif ($selection -eq '3') { $Mode='choose' }
        else { throw '无效选择，没有启动。' }
    }
    if (-not $Capture -and $Mode -ne 'choose') { $Capture=Join-Path $script:ClientRoot '源码\tests\fixtures\双路视频样本.bin' }
    if ($Mode -eq 'choose' -or -not (Test-Path -LiteralPath $Capture -PathType Leaf)) {
        if ($CI) { throw 'CI模式需要有效录包路径，不打开文件选择窗口。' }
        Add-Type -AssemblyName System.Windows.Forms
        $dialog=New-Object Windows.Forms.OpenFileDialog
        try {
            $dialog.Title='选择双路真实录包（不是普通MP4）';$dialog.Filter='RM26录包 (*.bin)|*.bin'
            if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 0 }
            $Capture=$dialog.FileName
        } finally { $dialog.Dispose() }
    }
    if (-not (Test-Path -LiteralPath $script:ClientExe -PathType Leaf)) { throw '缺少程序/luojiamaster.exe。' }
    $Capture=(Get-Item -LiteralPath $Capture -ErrorAction Stop).FullName
    $session=Join-Path ([IO.Path]::GetTempPath()) ('luojiamaster-offline-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
    [void][IO.Directory]::CreateDirectory($session)
    $processes=@();$count=if($Mode -eq 'two'){2}else{1}
    if ($count -eq 2) { Write-Host '两个窗口各自回放同一录包；这不是A/B互联测试。' }
    for ($i=1;$i -le $count;$i++) {
        $report=Join-Path $session ('window-'+$i)
        [void][IO.Directory]::CreateDirectory($report)
        $values=Get-OfflineArguments -CapturePath $Capture -ReportPath $report -Duration $Seconds
        $previousLogRoot=$env:ROBOGO_LOG_ROOT
        try {
            $env:ROBOGO_LOG_ROOT=Join-Path $report 'logs'
            $process=Start-Process -FilePath $script:ClientExe -ArgumentList (Quote-ProcessArguments $values) -WorkingDirectory $script:ProgramRoot -PassThru
            $processes+=,[pscustomobject]@{Process=$process;Report=$report}
        } finally { $env:ROBOGO_LOG_ROOT=$previousLogRoot }
    }
    $allPassed=$true
    foreach ($item in $processes) {
        if (-not $item.Process.WaitForExit(($Seconds+60)*1000)) { Write-Host ('[未结束] 请手动关闭测试窗口；未强制结束其他客户端。报告：'+$item.Report);$allPassed=$false;continue }
        $summaryPath=Join-Path $item.Report 'summary.json'
        if (-not (Test-Path -LiteralPath $summaryPath)) { Write-Host ('[失败] 没有测试摘要：'+$item.Report);$allPassed=$false;continue }
        $summary=[IO.File]::ReadAllText($summaryPath,[Text.Encoding]::UTF8) | ConvertFrom-Json
        $passed=$item.Process.ExitCode -eq 0 -and $summary.completed -eq $true -and $summary.dual_stream_smoke_ok -eq $true -and $summary.offline_no_network -eq $true
        Write-Host ('[冒烟结果] 完成={0} 双路出画={1} 新图={2}/{3} 退出码={4}' -f $summary.completed,$summary.dual_stream_smoke_ok,$summary.udp_new_presented,$summary.cbb_new_presented,$item.Process.ExitCode)
        if (-not $passed) { $allPassed=$false }
    }
    Write-Host ('本次记录：'+$session)
    Write-Host '这里只完成短时离线冒烟检查；详细失败请保留整个本次记录目录。'
    if (-not $CI) { [void](Read-Host '按回车关闭工具') }
    if (-not $allPassed) { exit 1 }
} catch {
    Write-Host ('[失败] '+$_.Exception.Message) -ForegroundColor Red
    if (-not $CI) { [void](Read-Host '按回车关闭工具') };exit 1
}
