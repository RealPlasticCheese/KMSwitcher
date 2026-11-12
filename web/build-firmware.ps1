[CmdletBinding()]
param(
    [ValidateSet('controller', 'receiver', 'all')]
    [string]$Target = 'all',

    [switch]$ResetConfig
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$projects = if ($Target -eq 'all') { @('receiver', 'controller') } else { @($Target) }
$failed = @()
Write-Host "Building sources in $repoRoot"
Write-Host 'This builds firmware only. No microcontroller is flashed.'
foreach ($project in $projects) {
    Write-Host "`n=== Building $project ==="
    try {
        $buildOptions = @{ Project = $project; Action = 'build'; ResetConfig = $ResetConfig }
        & (Join-Path $repoRoot 'scripts\idf.ps1') @buildOptions
        if (-not $?) { throw 'ESP-IDF build failed.' }
        $basePath = Join-Path $repoRoot "build\$project\kmswitcher_$project"
        if (-not (Test-Path -LiteralPath "$basePath.bin") -or -not (Test-Path -LiteralPath "$basePath.json")) {
            throw 'The build did not produce both firmware and manifest files.'
        }
        $manifest = Get-Content -LiteralPath "$basePath.json" -Raw | ConvertFrom-Json
        $image = Get-Item -LiteralPath "$basePath.bin"
        $algorithm = [Security.Cryptography.SHA256]::Create()
        $stream = [IO.File]::OpenRead($image.FullName)
        try {
            $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $stream.Dispose()
            $algorithm.Dispose()
        }
        if ($manifest.role -ne $project -or $manifest.size -ne $image.Length -or $manifest.sha256 -ne $hash) {
            throw 'The generated firmware does not match its build manifest.'
        }
        Write-Host "`nSUCCESS: $project"
        Write-Host "Image:    $basePath.bin"
        Write-Host "Manifest: $basePath.json"
    }
    catch {
        $failed += $project
        Write-Host "FAILED: $project - $($_.Exception.Message)" -ForegroundColor Red
    }
}
if ($failed.Count -gt 0) {
    Write-Host "`nBuild failed for: $($failed -join ', '). Review the output above; do not use old files from a failed build." -ForegroundColor Red
    exit 1
}
Write-Host "`nBuild complete. In Firmware Setup, choose the local application .bin with its matching .json beside it."
Write-Host 'For initial wired installation or recovery, use the repository flash-controller.cmd or flash-receiver.cmd wrapper.'
