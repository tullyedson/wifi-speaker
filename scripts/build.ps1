param([switch]$Release, [switch]$Test)
$ErrorActionPreference = 'Stop'
$speakerRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$speakerVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $speakerVswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++ and CMake.' }
$speakerVs = & $speakerVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $speakerVs) { throw 'Visual Studio C++ build tools not found' }
$env:PATH = (Join-Path $env:USERPROFILE '.cargo\bin') + ';' + (Join-Path $speakerVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin') + ';' + $env:PATH
$env:LIBCLANG_PATH = Join-Path $speakerRoot '.tools\venv\Lib\site-packages\clang\native'
$env:CARGO_TARGET_DIR = Join-Path $speakerRoot 'target'
$env:CMAKE_BUILD_PARALLEL_LEVEL = '4'
Set-Location -LiteralPath $speakerRoot
if ($Test) { & cargo.exe test --workspace --locked -j 4 } elseif ($Release) { & cargo.exe build --workspace --release --locked -j 4 } else { & cargo.exe build --workspace --locked -j 4 }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
