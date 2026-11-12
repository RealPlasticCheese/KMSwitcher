[CmdletBinding()]
param(
    [string]$SerialNumber
)

$ErrorActionPreference = 'Stop'

# Identify the tested S3's external CH343 UART bridge without opening any port.
# This identifies the USB bridge, not the firmware running behind it.
$candidates = @(Get-PnpDevice -PresentOnly | ForEach-Object {
    if ($_.Class -eq 'Ports' -and $_.Status -eq 'OK' -and
        $_.InstanceId -match '^USB\\VID_1A86&PID_55D3\\' -and
        $_.FriendlyName -match '\((COM[0-9]+)\)$') {
        $devicePort = $Matches[1]
        $deviceSerial = ($_.InstanceId -split '\\')[-1]
        if (-not $SerialNumber -or $deviceSerial -eq $SerialNumber) {
            [pscustomobject]@{ Port = $devicePort; SerialNumber = $deviceSerial }
        }
    }
})

if ($candidates.Count -eq 0) {
    throw 'No matching CH343 controller UART bridge found. Connect the S3 COM/UART connector and run scripts/list-devices.ps1. For another bridge type, pass -Port to scripts/watch-controller.ps1.'
}
if ($candidates.Count -gt 1) {
    $choices = ($candidates | ForEach-Object { "$($_.Port) (serial $($_.SerialNumber))" }) -join ', '
    throw "Multiple CH343 bridges found: $choices. Specify -SerialNumber to choose one."
}

$candidates[0].Port
