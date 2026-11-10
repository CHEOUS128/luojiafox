param([string]$VehiclePath='', [string]$Python='')
$ErrorActionPreference='Stop'
if(-not $Python){
    $bundled=Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    $Python=if(Test-Path -LiteralPath $bundled){$bundled}else{'python'}
}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $VehiclePath){$VehiclePath=[IO.Path]::GetFullPath((Join-Path $root '..\autoaim\my_lf_INF_20261025'))}
$out=Join-Path $root 'build\vehicle-trial-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $Python (Join-Path $root 'tools\prepare_vehicle_test.py') --vehicle $VehiclePath --out $out
if($LASTEXITCODE){throw '源码提取失败'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=(& $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if(-not $vs){throw '需要VS2022 C++编译工具'}
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -Arch amd64 -HostArch amd64 | Out-Null
$exe=Join-Path $out 'vehicle_trial_test.exe'
& cl /nologo /std:c++17 /EHsc /utf-8 /W4 /O2 ("/I"+$VehiclePath) ("/I"+$out) (Join-Path $root 'tests\vehicle_trial_test.cpp') ("/Fo"+(Join-Path $out 'vehicle_trial_test.obj')) ("/Fe"+$exe)
if($LASTEXITCODE){throw '串口主机回归编译失败'}
& $exe
if($LASTEXITCODE){throw '串口主机回归失败'}
Write-Output '此测试替换硬件接口，不等于Jetson构建或USB线上验证。'
