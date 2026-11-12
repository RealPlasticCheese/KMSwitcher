[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Installer, [ValidateSet('companion', 'setup')][string]$Product = 'companion')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path $repoRoot "build\installer-smoke-$Product"
$logDirectory = Join-Path $repoRoot 'build\installer-logs'
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$target = Join-Path $testRoot 'app'
$uninstaller = Join-Path $target 'unins000.exe'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{A507B117-4FDB-48DD-9E41-383D3D55214E}_is1'
if ($Product -eq 'setup') { $uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{81988AF7-DB48-43CC-A44E-E5BD20185213}_is1' }
if (Test-Path -LiteralPath $uninstallKey) { throw 'A real installation exists in this account; use a clean test account.' }
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$publisher = 'Frank Tzanabetis'
$website = 'https://plasticcheese.com/'
if ((Get-Item -LiteralPath $Installer).VersionInfo.CompanyName -ne $publisher) {
    throw 'Installer publisher metadata is missing.'
}
$originalLocalData = $env:LOCALAPPDATA
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$originalStartup = (Get-ItemProperty -Path $runKey -ErrorAction SilentlyContinue).KMSwitcher
$env:LOCALAPPDATA = Join-Path $testRoot 'profile'
$preferences = Join-Path $env:LOCALAPPDATA 'KMSwitcher\preferences.json'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $preferences) | Out-Null
$marker = '{"enabled":false,"installer_test":"preserve"}'
Set-Content -LiteralPath $preferences -Value $marker -NoNewline
try {
    # Install and repair/upgrade the same release; preserve user data in both passes.
    foreach ($pass in 1..2) {
        if ($pass -eq 2) {
            # Model files left behind by the former bundled-firmware installer.
            New-Item -ItemType Directory -Force -Path (Join-Path $target 'firmware') | Out-Null
            Set-Content -LiteralPath (Join-Path $target 'firmware/obsolete.bin') -Value 'legacy'
            Set-Content -LiteralPath (Join-Path $target 'KMSwitcher.html') -Value 'legacy'
        }
        $runningGui = $null
        if (($pass -eq 2) -and ($Product -eq 'companion')) {
            $runningGui = Start-Process -FilePath (Join-Path $target 'kmswitcher-gui.exe') -ArgumentList @('ui', '--tray') -WindowStyle Hidden -PassThru
            $ready = $false
            foreach ($attempt in 1..30) {
                Start-Sleep -Milliseconds 200
                & (Join-Path $target 'kmswitcher.exe') --json status | Out-Null
                if ($LASTEXITCODE -eq 0) { $ready = $true; break }
            }
            if (-not $ready) { throw 'Companion did not start before the upgrade test.' }
            $duplicate = Start-Process -FilePath (Join-Path $target 'kmswitcher-gui.exe') -ArgumentList @('ui', '--tray') -WindowStyle Hidden -PassThru
            if (-not $duplicate.WaitForExit(10000) -or $duplicate.ExitCode -ne 0) { throw 'A duplicate Companion did not exit successfully.' }
            if ($runningGui.HasExited) { throw 'The original Companion unexpectedly exited.' }
        }
        $process = Start-Process -FilePath (Resolve-Path -LiteralPath $Installer).Path -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/TASKS=', "/DIR=`"$target`"", "/LOG=`"$logDirectory\install-$pass.log`"") -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "Installer pass $pass failed." }
        $registered = Get-ItemProperty -LiteralPath $uninstallKey
        if ($registered.Publisher -ne $publisher -or $registered.URLInfoAbout -ne $website) {
            throw 'Installed publisher name or website is missing.'
        }
        if ($runningGui -and -not $runningGui.WaitForExit(10000)) { throw 'Upgrade left the old Companion running.' }
        if ((Get-Content -LiteralPath $preferences -Raw) -ne $marker) { throw 'Installation changed local preferences.' }
        & python (Join-Path $PSScriptRoot 'test-packaged.py') $target
        if ($LASTEXITCODE -ne 0) { throw 'Installed application checks failed.' }
    }
} finally {
    if (Test-Path -LiteralPath $uninstaller) {
        $process = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$logDirectory\uninstall.log`"") -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw 'Uninstall failed.' }
    }
    $env:LOCALAPPDATA = $originalLocalData
    if ($null -ne $originalStartup) {
        New-Item -Path $runKey -Force | Out-Null
        New-ItemProperty -Path $runKey -Name KMSwitcher -Value $originalStartup -PropertyType String -Force | Out-Null
    }
}
if (Test-Path -LiteralPath (Join-Path $target 'kmswitcher.exe')) { throw 'Uninstaller left the application executable.' }
if ((Get-Content -LiteralPath $preferences -Raw) -ne $marker) { throw 'Uninstall removed user preferences.' }
Write-Output 'PASS: install, reinstall/upgrade and uninstall preserve user preferences.'
