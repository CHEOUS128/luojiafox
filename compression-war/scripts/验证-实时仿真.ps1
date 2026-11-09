param([switch]$Full)
# Compatibility entry: current acceptance is the two-packet realtime baseline.
& (Join-Path $PSScriptRoot '验证-双包实时仿真.ps1') -Full:$Full
