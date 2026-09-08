param([switch]$Build, [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$speakerRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($Build) { & (Join-Path $PSScriptRoot 'build.ps1') -Release; if ($LASTEXITCODE -ne 0) { throw 'Release build failed' } }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $speakerRoot 'artifacts\Smart Speaker' }
$speakerOutput = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $speakerOutput) -and @(Get-ChildItem -LiteralPath $speakerOutput -Force).Count -ne 0) { throw 'Output directory must be empty. Choose another -OutputDirectory to preserve the existing package.' }
foreach ($speakerBinary in @('smart-speaker.exe','speaker-hub.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $speakerRoot ('target\release\' + $speakerBinary)))) { throw 'Build the release executables before packaging.' }
}
$speakerModel = Join-Path $speakerRoot 'models\ggml-base.en.bin'
if (Test-Path -LiteralPath $speakerModel) {
    if ((Get-Item -LiteralPath $speakerModel).Length -ne 147964211 -or (Get-FileHash -LiteralPath $speakerModel -Algorithm SHA256).Hash -ne 'a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002') { throw 'The optional speech model failed verification.' }
}
New-Item -ItemType Directory -Path $speakerOutput -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $speakerRoot 'target\release\smart-speaker.exe') -Destination (Join-Path $speakerOutput 'Smart Speaker.exe')
Copy-Item -LiteralPath (Join-Path $speakerRoot 'target\release\speaker-hub.exe') -Destination (Join-Path $speakerOutput 'speaker-hub.exe')
foreach ($speakerDocument in @('README.md','LICENSE','SECURITY.md','CONTRIBUTING.md','docs')) {
    Copy-Item -LiteralPath (Join-Path $speakerRoot $speakerDocument) -Destination $speakerOutput -Recurse
}
if (Test-Path -LiteralPath $speakerModel) {
    New-Item -ItemType Directory -Path (Join-Path $speakerOutput 'models') -Force | Out-Null
    Copy-Item -LiteralPath $speakerModel -Destination (Join-Path $speakerOutput 'models\ggml-base.en.bin')
}
$speakerHashes = @(Get-ChildItem -LiteralPath $speakerOutput -Recurse -File | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{Hash=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash;Path=$_.FullName.Substring($speakerOutput.TrimEnd('\').Length + 1).Replace('\','/')}
})
New-Item -ItemType Directory -Path (Join-Path $speakerRoot 'artifacts') -Force | Out-Null
$speakerHashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $speakerRoot 'artifacts\desktop-sha256.json') -Encoding UTF8
Write-Output ('Portable app: ' + (Join-Path $speakerOutput 'Smart Speaker.exe'))
