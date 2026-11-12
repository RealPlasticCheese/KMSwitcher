[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port,

    [string]$SerialNumber,

    [ValidateRange(1, 86400)]
    [int]$Seconds = 3600,

    [ValidateSet('help', 'about', 'history', 'status', 'slaves')]
    [string]$Command = 'history',

    [ValidateRange(0, 86400)]
    [int]$StatusInterval = 0,

    [string]$OutputPath,

    [switch]$NonInteractive
)

$ErrorActionPreference = 'Stop'
if ($Port -and $SerialNumber) { throw 'Specify either -Port or -SerialNumber, not both.' }
if (-not $Port) {
    $Port = & (Join-Path $PSScriptRoot 'find-controller-port.ps1') -SerialNumber $SerialNumber
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$registryPath = Join-Path $repoRoot '.tools\esp\tools\eim_idf.json'
if (-not (Test-Path -LiteralPath $registryPath)) {
    throw 'Run scripts/setup-toolchain.ps1 first.'
}
$toolchain = Get-Content -LiteralPath (Join-Path $repoRoot 'toolchain.json') -Raw | ConvertFrom-Json
$registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
$setup = @($registry.idfInstalled | Where-Object { $_.name -eq $toolchain.espIdfVersion -and $_.status -eq 'finished' }) | Select-Object -First 1
if (-not $setup -or -not (Test-Path -LiteralPath $setup.python)) {
    throw 'The pinned ESP-IDF Python environment is missing. Run scripts/setup-toolchain.ps1.'
}

if (-not $OutputPath) {
    $OutputPath = Join-Path $repoRoot ('build\controller-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$OutputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
[System.IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
$captureArgs = @('--port', $Port, '--seconds', $Seconds, '--command', $Command, '--output', $OutputPath)
if (-not $NonInteractive) { $captureArgs += '--interactive' }
if ($StatusInterval -gt 0) { $captureArgs += @('--status-interval', $StatusInterval) }

Write-Host "Reading controller $Port for up to $Seconds seconds; initial command: $Command."
if (-not $NonInteractive) {
    Write-Host 'Type help for commands, about for firmware information; .quit closes the console and saves the log.'
}
Write-Host "Capture is saved when the console closes: $OutputPath"
# No DTR/RTS assertion or UART reset: leave the running controller alone.
& $setup.python -X utf8 -u (Join-Path $PSScriptRoot 'serial-capture.py') @captureArgs
if ($LASTEXITCODE -ne 0) {
    throw "Controller capture failed with exit code $LASTEXITCODE. Close any other monitor using $Port and retry."
}
