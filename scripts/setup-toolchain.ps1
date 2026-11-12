[CmdletBinding()]
param([switch]$IfNeeded)

$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolchain = Get-Content -LiteralPath (Join-Path $repoRoot 'toolchain.json') -Raw | ConvertFrom-Json
$toolsRoot = Join-Path $repoRoot '.tools'
$espRoot = Join-Path $toolsRoot 'esp'
$sharedEspRoot = Join-Path $env:SystemDrive 'Espressif'
$installerPath = Join-Path $toolsRoot 'eim.exe'

New-Item -ItemType Directory -Path $toolsRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath $installerPath)) {
    $installerUrl = 'https://github.com/espressif/idf-im-ui/releases/download/{0}/eim-cli-windows-x64-{0}.exe' -f $toolchain.eimVersion
    Invoke-WebRequest -UseBasicParsing -Uri $installerUrl -OutFile $installerPath
}

$installerHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
if ($installerHash -ne $toolchain.eimWindowsX64Sha256) {
    throw "EIM checksum mismatch. Remove $installerPath and run setup again."
}

$registryPath = Join-Path $espRoot 'tools\eim_idf.json'
$installCommand = 'install'
$installPath = $espRoot
if (Test-Path -LiteralPath $registryPath) {
    $existingRegistry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
    $existingSetup = @($existingRegistry.idfInstalled | Where-Object { $_.name -eq $toolchain.espIdfVersion }) | Select-Object -First 1
    if ($existingSetup -and (Test-Path -LiteralPath (Join-Path $existingSetup.path 'tools\idf.py'))) {
        $compiler = Get-ChildItem -Path (Join-Path $sharedEspRoot 'tools\xtensa-esp-elf\*\xtensa-esp-elf\bin\xtensa-esp-elf-gcc.exe') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($IfNeeded -and $existingSetup.status -eq 'finished' -and $compiler -and (Test-Path -LiteralPath $existingSetup.python)) {
            & $existingSetup.python (Join-Path $existingSetup.path 'tools\idf.py') --version
            if ($LASTEXITCODE -eq 0) {
                Write-Output 'Pinned SDK, Python and compiler are available; keeping the validated cache.'
                return
            }
        }
        $installCommand = 'fix'
        $installPath = $existingSetup.path
    }
}

$installArgs = @(
    $installCommand,
    '--idf-versions', $toolchain.espIdfVersion,
    '--target', ($toolchain.targets -join ','),
    '--idf-features', 'ide',
    '--path', $installPath,
    '--tool-install-folder-name', (Join-Path $sharedEspRoot 'tools'),
    '--tool-download-folder-name', (Join-Path $sharedEspRoot 'dist'),
    '--esp-idf-json-path', (Join-Path $espRoot 'tools'),
    '--config-file-save-path', (Join-Path $toolsRoot 'eim-config.toml'),
    '--log-file', (Join-Path $toolsRoot 'eim-install.log'),
    '--do-not-track', 'true',
    '--non-interactive', 'true',
    '--skip-components-download', 'true'
)
& $installerPath @installArgs
if ($LASTEXITCODE -ne 0) {
    throw "ESP-IDF installation failed with exit code $LASTEXITCODE. See the installer output above."
}

$registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
$setup = @($registry.idfInstalled | Where-Object { $_.name -eq $toolchain.espIdfVersion -and $_.status -eq 'finished' }) | Select-Object -First 1
if (-not $setup -or -not (Test-Path -LiteralPath $setup.python)) {
    throw 'EIM did not finish creating the pinned ESP-IDF installation.'
}
& $installerPath --do-not-track true --esp-idf-json-path (Split-Path -Parent $registryPath) select $toolchain.espIdfVersion
if ($LASTEXITCODE -ne 0) {
    throw 'EIM could not select the installed ESP-IDF version.'
}
