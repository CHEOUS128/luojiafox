# 建一个目录 junction（C:\rm26cw -> 仓库里的 compression-war）。
#
# 为什么需要：ninja / MSVC / CMake 的中间文件路径里如果带中文，历史上踩过多个坑
# （链接器的响应文件、obj 的相对路径等）。客户端工程也用了同样的办法（C:\rm26build）。
#
# 用法：powershell -ExecutionPolicy Bypass -File 建短路径.ps1 <junction 路径> <目标目录>
param(
    [Parameter(Mandatory = $true)][string]$Link,
    [Parameter(Mandatory = $true)][string]$Target
)

$ErrorActionPreference = 'Stop'

# 已经存在就不动（可能是上次建的，也可能是用户自己建的）
if (Test-Path -LiteralPath $Link) {
    exit 0
}

$parent = Split-Path -Parent $Link
if ($parent -and -not (Test-Path -LiteralPath $parent)) {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
}

try {
    # 优先用 junction（不需要管理员权限、不受开发者模式限制）
    New-Item -ItemType Junction -Path $Link -Target $Target -ErrorAction Stop | Out-Null
    exit 0
} catch {
    # junction 失败（极少数文件系统不支持），退回符号链接
    try {
        New-Item -ItemType SymbolicLink -Path $Link -Target $Target -ErrorAction Stop | Out-Null
        exit 0
    } catch {
        [Console]::Error.WriteLine($_.Exception.Message)
        exit 1
    }
}
