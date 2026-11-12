[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port,

    [ValidatePattern('^[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}$')]
    [string]$ExpectedMac,

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repoRoot 'build\receiver'
$registryPath = Join-Path $repoRoot '.tools\esp\tools\eim_idf.json'
if (-not (Test-Path -LiteralPath $registryPath)) {
    throw 'Run scripts/setup-toolchain.ps1 first.'
}
if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $Port) {
    throw "Port $Port is not present. Hold the S2 button while reconnecting, release it, then run scripts/list-devices.ps1."
}
$toolchain = Get-Content -LiteralPath (Join-Path $repoRoot 'toolchain.json') -Raw | ConvertFrom-Json
$registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
$setup = @($registry.idfInstalled | Where-Object { $_.name -eq $toolchain.espIdfVersion -and $_.status -eq 'finished' }) | Select-Object -First 1
if (-not $setup -or -not (Test-Path -LiteralPath $setup.python)) {
    throw 'The pinned ESP-IDF Python environment is incomplete. Run scripts/setup-toolchain.ps1.'
}
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'idf.ps1') -Project receiver -Action build
}
if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'flash_args'))) {
    throw 'Receiver build files are missing. Run without -SkipBuild.'
}
$flashConfig = Get-Content -LiteralPath (Join-Path $buildPath 'flasher_args.json') -Raw | ConvertFrom-Json
if ($flashConfig.extra_esptool_args.chip -ne 'esp32s2') {
    throw 'The receiver build does not target ESP32-S2; refusing to flash.'
}

# The KEY dongles work reliably with manual download mode and no serial resets.
# Keep image offsets, flash capacity, mode, and frequency in ESP-IDF's build files.
$resetArgs = @('--chip', 'esp32s2', '--port', $Port, '--before', 'no-reset', '--after', 'no-reset')
$identity = @(& $setup.python -m esptool @resetArgs read-mac)
$identityResult = $LASTEXITCODE
$identity | ForEach-Object { Write-Host $_ }
if ($identityResult -ne 0) { throw "S2 identification failed with exit code $identityResult; nothing was flashed." }
$macMatches = [regex]::Matches(($identity -join "`n"), '(?im)^MAC:\s*([0-9a-f:]{17})\s*$')
if (-not $macMatches.Count) { throw 'Could not identify the S2 MAC; refusing to flash.' }
$deviceMac = $macMatches[$macMatches.Count - 1].Groups[1].Value.ToLowerInvariant()
if ($ExpectedMac -and $deviceMac -ne $ExpectedMac.ToLowerInvariant()) {
    throw "S2 MAC $deviceMac differs from expected $ExpectedMac; nothing was flashed."
}
Write-Host "Flashing receiver $deviceMac on $Port"
Push-Location -LiteralPath $buildPath
try {
    & $setup.python -m esptool @resetArgs write-flash '@flash_args'
    if ($LASTEXITCODE -ne 0) { throw "Receiver flashing failed with exit code $LASTEXITCODE." }
} finally {
    Pop-Location
}
Write-Host "Receiver $deviceMac flashed and verified."
Write-Host 'Unplug and reconnect without holding the button to run USB HID firmware.'
Write-Host 'Assign its number through pairing; the same firmware is used for every S2.'
