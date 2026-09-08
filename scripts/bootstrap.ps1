param([string]$Python = 'python.exe')
$ErrorActionPreference = 'Stop'
$speakerRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$speakerVenv = Join-Path $speakerRoot '.tools\venv'
if (-not (Test-Path -LiteralPath (Join-Path $speakerVenv 'Scripts\python.exe'))) {
    & $Python -m venv $speakerVenv
    if ($LASTEXITCODE -ne 0) { throw 'Python virtual environment creation failed' }
}
& (Join-Path $speakerVenv 'Scripts\python.exe') -m pip install platformio==6.2.0 libclang==18.1.1 esptool==5.4.0 websockets==15.0.1
if ($LASTEXITCODE -ne 0) { throw 'Helper installation failed' }
Write-Output 'Helpers installed. Rust stable, Visual Studio C++ build tools with CMake, and Windows WebView2 are also required to build the desktop app.'
