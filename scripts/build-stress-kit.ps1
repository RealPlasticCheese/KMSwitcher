[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'idf.ps1') -Project hid-generator
    & (Join-Path $PSScriptRoot 'idf.ps1') -Project controller
    & (Join-Path $PSScriptRoot 'idf.ps1') -Project receiver
}
& python (Join-Path $PSScriptRoot 'package-stress-kit.py')
if ($LASTEXITCODE -ne 0) { throw 'Stress kit packaging failed.' }
