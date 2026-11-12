[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('controller', 'receiver', 'board-check', 'bringup', 'led-sweep', 'hid-generator', 'usb-isolation')]
    [string]$Project,

    [ValidateSet('build', 'reconfigure', 'menuconfig', 'size', 'flash', 'monitor')]
    [string]$Action = 'build',

    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port,

    [ValidateSet('esp32s2', 'esp32s3')]
    [string]$Target,

    [switch]$ResetConfig,
    [switch]$TestSupport,

    [switch]$Release
)

$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$repoRoot = Split-Path -Parent $PSScriptRoot
$registryDir = Join-Path $repoRoot '.tools\esp\tools'
$toolchain = Get-Content -LiteralPath (Join-Path $repoRoot 'toolchain.json') -Raw | ConvertFrom-Json
# Worktrees share the primary checkout's SDK installation, but build their own sources.
if (-not (Test-Path -LiteralPath (Join-Path $registryDir 'eim_idf.json'))) {
    $commonGitDir = & git -C $repoRoot rev-parse --path-format=absolute --git-common-dir
    if ($LASTEXITCODE -eq 0 -and $commonGitDir) {
        $sharedRoot = Split-Path -Parent $commonGitDir
        $registryDir = Join-Path $sharedRoot '.tools\esp\tools'
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $registryDir 'eim_idf.json'))) {
    throw 'Run scripts/setup-toolchain.ps1 first.'
}
if ($Action -in @('flash', 'monitor') -and -not $Port) {
    throw 'Specify -Port COMn explicitly. Use scripts/list-devices.ps1 to find the port.'
}

$registry = Get-Content -LiteralPath (Join-Path $registryDir 'eim_idf.json') -Raw | ConvertFrom-Json
$setup = @($registry.idfInstalled | Where-Object { $_.name -eq $toolchain.espIdfVersion -and $_.status -eq 'finished' }) | Select-Object -First 1
if (-not $setup -or -not (Test-Path -LiteralPath $setup.activationScript)) {
    throw 'The pinned ESP-IDF installation is incomplete. Run scripts/setup-toolchain.ps1.'
}
. $setup.activationScript

$arguments = @('--project', $Project, '--action', $Action)
if ($Port) { $arguments += @('--port', $Port) }
if ($Target) { $arguments += @('--target', $Target) }
if ($Release) { $arguments += '--release' }
if ($TestSupport) { $arguments += '--test-support' }
if ($ResetConfig) { $arguments += '--reset-config' }
& $setup.python (Join-Path $repoRoot 'scripts/idf.py') @arguments
if ($LASTEXITCODE -ne 0) { throw "ESP-IDF $Action failed with exit code $LASTEXITCODE." }
