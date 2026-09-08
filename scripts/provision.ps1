param([Parameter(Mandatory=$true)][string]$Config, [Parameter(Mandatory=$true)][string]$Port)
$ErrorActionPreference = 'Stop'
$speakerRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
& (Join-Path $speakerRoot '.tools\venv\Scripts\python.exe') (Join-Path $PSScriptRoot 'provision.py') --port $Port --config $Config
if ($LASTEXITCODE -ne 0) { throw 'Speaker provisioning failed' }
