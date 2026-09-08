param([ValidateSet('Build','Backup','Flash','Update')][string]$Action = 'Build', [string]$Port = '')
$ErrorActionPreference = 'Stop'
if ($Action -ne 'Build' -and $Port -notmatch '^COM[1-9][0-9]*$') { throw 'Specify the speaker USB port with -Port COM<number>. Check Windows Device Manager.' }
$speakerRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$speakerPython = Join-Path $speakerRoot '.tools\venv\Scripts\python.exe'
$env:PLATFORMIO_CORE_DIR = Join-Path $speakerRoot '.tools\platformio'
$speakerFirmware = Join-Path $speakerRoot 'firmware'
$speakerBuild = Join-Path $speakerFirmware '.pio\build\muse_luxe'
$speakerOutput = Join-Path $speakerRoot 'artifacts\firmware'
if ($Action -in @('Build','Flash','Update')) {
    Push-Location -LiteralPath $speakerFirmware
    try { & $speakerPython -m platformio run; if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed' } } finally { Pop-Location }
    New-Item -ItemType Directory -Path $speakerOutput -Force | Out-Null
    $speakerBootApp = Join-Path $env:PLATFORMIO_CORE_DIR 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
    & $speakerPython -m esptool --chip esp32 merge-bin --output (Join-Path $speakerOutput 'muse-luxe-factory.bin') --flash-mode dio --flash-freq 40m --flash-size 4MB 0x1000 (Join-Path $speakerBuild 'bootloader.bin') 0x8000 (Join-Path $speakerBuild 'partitions.bin') 0xe000 $speakerBootApp 0x10000 (Join-Path $speakerBuild 'firmware.bin')
    if ($LASTEXITCODE -ne 0) { throw 'Firmware image assembly failed' }
    Copy-Item -LiteralPath (Join-Path $speakerBuild 'firmware.bin') -Destination (Join-Path $speakerOutput 'muse-luxe-app.bin')
    Get-FileHash -LiteralPath (Join-Path $speakerOutput 'muse-luxe-factory.bin') -Algorithm SHA256 | Format-List
}
if ($Action -in @('Backup','Flash','Update')) {
    $speakerBackupDirectory = Join-Path $speakerRoot 'backups'
    New-Item -ItemType Directory -Path $speakerBackupDirectory -Force | Out-Null
    $speakerBackup = Join-Path $speakerBackupDirectory ('muse-luxe-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8) + '.bin')
    & $speakerPython -m esptool --chip esp32 --port $Port --baud 460800 read-flash 0 0x400000 $speakerBackup
    if ($LASTEXITCODE -ne 0 -or (Get-Item -LiteralPath $speakerBackup).Length -ne 4194304) { throw 'Full flash backup failed. No firmware was written.' }
    Get-FileHash -LiteralPath $speakerBackup -Algorithm SHA256 | Format-List
    Write-Output ('Original flash saved to ' + $speakerBackup)
}
if ($Action -eq 'Flash') {
    $speakerImage = Join-Path $speakerOutput 'muse-luxe-factory.bin'
    & $speakerPython -m esptool --chip esp32 --port $Port --baud 460800 write-flash 0 $speakerImage
    if ($LASTEXITCODE -ne 0) { throw 'Flashing failed. Keep the original backup and retry over USB.' }
    & $speakerPython -m esptool --chip esp32 --port $Port --baud 460800 verify-flash 0 $speakerImage
    if ($LASTEXITCODE -ne 0) { throw 'Flash verification failed' }
    Write-Output 'Firmware written and verified. Switch the speaker on and complete Wi-Fi setup.'
}

if ($Action -eq 'Update') {
    $speakerImage = Join-Path $speakerOutput 'muse-luxe-app.bin'
    if ((Get-Item -LiteralPath $speakerImage).Length -gt 0x1D0000) { throw 'Application exceeds the installed app partition.' }
    & $speakerPython -m esptool --chip esp32 --port $Port --baud 460800 write-flash 0x10000 $speakerImage
    if ($LASTEXITCODE -ne 0) { throw 'Application update failed. Keep the full backup.' }
    & $speakerPython -m esptool --chip esp32 --port $Port --baud 460800 verify-flash 0x10000 $speakerImage
    if ($LASTEXITCODE -ne 0) { throw 'Application update verification failed' }
    Write-Output 'Application updated and verified. Existing Wi-Fi and speaker credentials were preserved.'
}
