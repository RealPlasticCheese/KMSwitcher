[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port,

    [ValidateRange(1, 86400)]
    [int]$Seconds = 240
)

$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolchain = Get-Content -LiteralPath (Join-Path $repoRoot 'toolchain.json') -Raw | ConvertFrom-Json
$registry = Get-Content -LiteralPath (Join-Path $repoRoot '.tools\esp\tools\eim_idf.json') -Raw | ConvertFrom-Json
$setup = @($registry.idfInstalled | Where-Object { $_.name -eq $toolchain.espIdfVersion -and $_.status -eq 'finished' }) | Select-Object -First 1
if (-not $setup -or -not (Test-Path -LiteralPath $setup.python)) {
    throw 'The pinned ESP-IDF Python environment is missing. Run scripts/setup-toolchain.ps1.'
}
$logDirectory = Join-Path $repoRoot 'build'
[System.IO.Directory]::CreateDirectory($logDirectory) | Out-Null
$logPath = Join-Path $logDirectory 'led-sweep-console.log'
Write-Host "Watching LED sweep on $Port for $Seconds seconds. DTR starts the sweep."
Write-Host "Watch the dongle's blue LED and note the GPIO and LOW/HIGH label if it changes."
Write-Host "Ctrl+C stops capture; the current GPIO is released when the port closes. Log: $logPath"
& $setup.python -u (Join-Path $PSScriptRoot 'serial-capture.py') --port $Port --dtr --seconds $Seconds |
    Tee-Object -FilePath $logPath
if ($LASTEXITCODE -ne 0) { throw "LED sweep capture failed with exit code $LASTEXITCODE." }
Write-Host 'Capture complete. Reopen this console command to restart the sweep at the first GPIO.'
