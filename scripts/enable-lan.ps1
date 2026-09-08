[CmdletBinding(SupportsShouldProcess)]
param([string]$Executable = '', [ValidateRange(1,65535)][int]$Port = 48490)
$ErrorActionPreference = 'Stop'
$speakerRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Executable) { $Executable = Join-Path $speakerRoot 'artifacts\Smart Speaker\Smart Speaker.exe' }
$speakerExecutable = (Resolve-Path -LiteralPath $Executable).Path
if (-not (Test-Path -LiteralPath $speakerExecutable -PathType Leaf)) { throw 'The Smart Speaker executable is missing.' }
$speakerHash = [Security.Cryptography.SHA256]::Create()
try { $speakerSuffix = ([BitConverter]::ToString($speakerHash.ComputeHash([Text.Encoding]::UTF8.GetBytes($speakerExecutable.ToLowerInvariant())))).Replace('-','').Substring(0,12) }
finally { $speakerHash.Dispose() }
$speakerRuleName = "SmartSpeaker-Private-LAN-$Port-$speakerSuffix"
if (-not $PSCmdlet.ShouldProcess($speakerExecutable, "Allow inbound TCP $Port from LocalSubnet on the Private network")) { return }
$speakerPrincipal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $speakerPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this helper in an administrator PowerShell to configure Windows Firewall.' }
$speakerRuntime = Join-Path $speakerRoot 'runtime'
New-Item -ItemType Directory -Path $speakerRuntime -Force | Out-Null
$speakerExisting = @(Get-NetFirewallApplicationFilter -Program $speakerExecutable -ErrorAction SilentlyContinue | Get-NetFirewallRule)
$speakerExisting | Select-Object Name,DisplayName,Enabled,Profile,Direction,Action | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $speakerRuntime ('firewall-before-' + [guid]::NewGuid().ToString('N') + '.json')) -Encoding UTF8
$speakerAllow = Get-NetFirewallRule -Name $speakerRuleName -ErrorAction SilentlyContinue
if ($speakerAllow) {
    $speakerFilter = $speakerAllow | Get-NetFirewallApplicationFilter
    if ($speakerFilter.Program -ne $speakerExecutable) { throw 'The named firewall rule belongs to another executable.' }
    Set-NetFirewallRule -Name $speakerRuleName -Enabled True -Direction Inbound -Action Allow -Profile Private -Protocol TCP -LocalPort $Port -RemoteAddress LocalSubnet -Program $speakerExecutable | Out-Null
} else {
    New-NetFirewallRule -Name $speakerRuleName -DisplayName "Smart Speaker (private LAN, TCP $Port)" -Description 'Allow registered Wi-Fi speakers to reach this app on the private local network.' -Enabled True -Direction Inbound -Action Allow -Profile Private -Protocol TCP -LocalPort $Port -RemoteAddress LocalSubnet -Program $speakerExecutable | Out-Null
}
$speakerVerified = Get-NetFirewallRule -Name $speakerRuleName
$speakerVerifiedPort = $speakerVerified | Get-NetFirewallPortFilter
if ($speakerVerified.Action -ne 'Allow' -or $speakerVerified.Profile -ne 'Private' -or $speakerVerifiedPort.LocalPort -ne [string]$Port) { throw 'Firewall rule verification failed.' }
[pscustomobject]@{success=$true;rule=$speakerRuleName;port=$Port;profile='Private';remote='LocalSubnet'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $speakerRuntime 'lan-firewall-result.json') -Encoding UTF8
Write-Output 'Private LAN rule configured. Existing block rules are unchanged; resolve any conflicting rule in Windows Firewall.'
