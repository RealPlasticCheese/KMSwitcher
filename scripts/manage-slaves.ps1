[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$OperationArgs
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$registryPath = Join-Path $repoRoot '.tools\esp\tools\eim_idf.json'
if (-not (Test-Path -LiteralPath $registryPath)) { throw 'Run scripts/setup-toolchain.ps1 first.' }
$toolchain = Get-Content -LiteralPath (Join-Path $repoRoot 'toolchain.json') -Raw | ConvertFrom-Json
$registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
$setup = @($registry.idfInstalled | Where-Object {
    $_.name -eq $toolchain.espIdfVersion -and $_.status -eq 'finished'
}) | Select-Object -First 1
if (-not $setup -or -not (Test-Path -LiteralPath $setup.python)) {
    throw 'The pinned ESP-IDF Python environment is missing. Run scripts/setup-toolchain.ps1.'
}
& $setup.python -X utf8 -u (Join-Path $PSScriptRoot 'manage-slaves.py') @OperationArgs
exit $LASTEXITCODE
