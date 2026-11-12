[CmdletBinding()]
param(
    [string]$App,
    [string]$Identity,
    [string]$Output,
    [ValidateSet('companion', 'setup')][string]$Product = 'companion',
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $App) { $App = Join-Path $repoRoot $(if ($Product -eq 'setup') { 'build\release\app\kmswitcher-setup' } else { 'build\release\app\kmswitcher' }) }
if (-not $Identity) { $Identity = Join-Path $repoRoot 'build\release\version.json' }
if (-not $Output) { $Output = Join-Path $repoRoot 'build\release\installers' }
$toolSettings = Get-Content -LiteralPath "$repoRoot\packaging\tools.json" -Raw | ConvertFrom-Json
$toolDirectory = Join-Path $repoRoot 'build\tools'
New-Item -ItemType Directory -Force -Path $toolDirectory | Out-Null
$setupPath = Join-Path $toolDirectory "innosetup-$($toolSettings.innoVersion)-x64.exe"
if (-not (Test-Path -LiteralPath $setupPath -PathType Leaf) -or
    (Get-FileHash -LiteralPath $setupPath -Algorithm SHA256).Hash -ne $toolSettings.innoSha256) {
    $downloadPath = "$setupPath.$([Guid]::NewGuid()).tmp"
    try {
        for ($attempt = 1; ; $attempt++) {
            try {
                Invoke-WebRequest -UseBasicParsing -Uri $toolSettings.innoUrl -OutFile $downloadPath -TimeoutSec 120
                break
            } catch {
                if ($attempt -ge 3) { throw }
                Write-Warning "Inno Setup download attempt ${attempt} failed: $($_.Exception.Message). Retrying."
                Start-Sleep -Seconds (5 * $attempt)
            }
        }
        if ((Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash -ne $toolSettings.innoSha256) {
            throw 'Inno Setup checksum failed.'
        }
        Move-Item -LiteralPath $downloadPath -Destination $setupPath -Force
    } finally {
        if (Test-Path -LiteralPath $downloadPath) { Remove-Item -LiteralPath $downloadPath }
    }
}
$compilerDirectory = Join-Path $toolDirectory 'inno'
$compiler = Join-Path $compilerDirectory 'ISCC.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    $process = Start-Process -FilePath $setupPath -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/DIR=`"$compilerDirectory`"") -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw 'Inno Setup installation failed.' }
}
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'Inno Setup compiler is missing.' }
if ($env:KMS_SIGNING_PFX) {
    if (-not $env:KMS_SIGNING_PASSWORD -or $env:KMS_SIGNING_PASSWORD.StartsWith('$(')) {
        throw 'Set windowsSigningPassword before requesting signed packages.'
    }
    $password = ConvertTo-SecureString -String $env:KMS_SIGNING_PASSWORD -AsPlainText -Force
    $null = Get-PfxData -FilePath $env:KMS_SIGNING_PFX -Password $password
    $signTool = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | Sort-Object FullName | Select-Object -Last 1
    if (-not $signTool) { throw 'Windows SDK signing tool is missing.' }
}
if ($PrepareOnly) {
    Write-Output 'PASS: installer compiler and requested signing prerequisites'
    return
}
$release = Get-Content -LiteralPath $Identity -Raw | ConvertFrom-Json
$appDirectory = (Resolve-Path -LiteralPath $App).Path
$outputDirectory = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$arguments = @("/DAppVersion=$($release.version)", "/DNumericVersion=$($release.windows_version)",
    "/DAppSource=$appDirectory", "/DInstallerOutput=$outputDirectory")
$importedCertificate = $null
try {
    if ($env:KMS_SIGNING_PFX) {
        $existing = @(Get-ChildItem Cert:\CurrentUser\My | ForEach-Object Thumbprint)
        $password = ConvertTo-SecureString -String $env:KMS_SIGNING_PASSWORD -AsPlainText -Force
        $certificate = Import-PfxCertificate -FilePath $env:KMS_SIGNING_PFX -CertStoreLocation Cert:\CurrentUser\My -Password $password
        if ($certificate.Thumbprint -notin $existing) { $importedCertificate = $certificate.PSPath }
        $signArguments = @('sign', '/sha1', $certificate.Thumbprint, '/s', 'My', '/fd', 'SHA256', '/tr', 'http://timestamp.digicert.com', '/td', 'SHA256')
        foreach ($name in @(Get-ChildItem -LiteralPath $appDirectory -Filter '*.exe' | ForEach-Object Name)) {
            & $signTool.FullName @signArguments (Join-Path $appDirectory $name)
            if ($LASTEXITCODE -ne 0) { throw 'Application signing failed.' }
        }
        $signCommand = '"' + $signTool.FullName + '" ' + ($signArguments -join ' ') + ' "$f"'
        $arguments += @('/DSignInstaller=1', "/Srelease=$signCommand")
    }
    $definition = if ($Product -eq 'setup') { 'packaging\windows\setup.iss' } else { 'packaging\windows\installer.iss' }
    & $compiler @arguments (Join-Path $repoRoot $definition)
    if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
    $prefix = if ($Product -eq 'setup') { 'KMSwitcher-Firmware-Setup' } else { 'KMSwitcher-Companion' }
    $archivePath = Join-Path $outputDirectory "$prefix-$($release.version)-windows-x64-portable.zip"
    if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath }
    Compress-Archive -LiteralPath $appDirectory -DestinationPath $archivePath
    $release | Add-Member -NotePropertyName product -NotePropertyValue $Product -Force
    $release | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $outputDirectory 'release.json') -Encoding UTF8
} finally {
    if ($importedCertificate) { Remove-Item -LiteralPath $importedCertificate }
}
