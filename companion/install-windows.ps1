[CmdletBinding()]
param([switch]$EnableStartup)
$ErrorActionPreference = 'Stop'
$installDirectory = Join-Path $env:LOCALAPPDATA 'KMSwitcher\venv'
python -m venv $installDirectory
if ($LASTEXITCODE -ne 0) { throw 'Python 3.10 or later is required.' }
$pythonExecutable = Join-Path $installDirectory 'Scripts\python.exe'
& $pythonExecutable -m pip install "$PSScriptRoot[ui]"
if ($LASTEXITCODE -ne 0) { throw 'Companion installation failed.' }
$commandExecutable = Join-Path $installDirectory 'Scripts\kmswitcher.exe'
if ($EnableStartup) { & $commandExecutable startup enable }
Write-Output "Installed: $commandExecutable"
Write-Output "Run '$commandExecutable ui' for the tray and settings window."
