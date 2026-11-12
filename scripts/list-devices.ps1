[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ports = @([System.IO.Ports.SerialPort]::GetPortNames())
Write-Host ('Serial ports: ' + $(if ($ports.Count) { $ports -join ', ' } else { '(none)' }))
Get-PnpDevice -PresentOnly |
    Where-Object { $_.Class -eq 'Ports' -or $_.InstanceId -like 'USB\VID_303A*' -or ($_.InstanceId -like 'USB\*' -and $_.Status -ne 'OK') } |
    Select-Object Status, Class, FriendlyName, InstanceId |
    Format-List
