param(
    [ValidateSet('Build','Backup','Flash','Update')][string]$Action = 'Build',
    [string]$Port = '',
    [ValidateSet('muse_luxe','spotpear_ball_v2')][string]$Board = 'muse_luxe',
    [string]$ToolsDirectory = ''
)
$ErrorActionPreference = 'Stop'
if ($Action -ne 'Build' -and $Port -notmatch '^COM[1-9][0-9]*$') { throw 'Specify the speaker USB port with -Port COM<number>. Check Windows Device Manager.' }
$speakerRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $ToolsDirectory) { $ToolsDirectory = Join-Path $speakerRoot '.tools' }
$speakerPython = Join-Path $ToolsDirectory 'venv\Scripts\python.exe'
$env:PLATFORMIO_CORE_DIR = Join-Path $ToolsDirectory 'platformio'
$speakerFirmware = Join-Path $speakerRoot 'firmware'
$speakerBuild = Join-Path $speakerFirmware ('.pio\build\' + $Board)
$speakerOutput = Join-Path $speakerRoot 'artifacts\firmware'
$profiles = @{
    muse_luxe = @{ Chip='esp32'; Name='muse-luxe'; FlashSize='4MB'; Bytes=0x400000; BootOffset='0x1000'; AppSize=0x1D0000; Reset='hard-reset' }
    spotpear_ball_v2 = @{ Chip='esp32s3'; Name='spotpear-ball-v2'; FlashSize='16MB'; Bytes=0x1000000; BootOffset='0x0'; AppSize=0x300000; Reset='watchdog-reset' }
}
$profile = $profiles[$Board]
if ($Action -in @('Build','Flash','Update')) {
    Push-Location -LiteralPath $speakerFirmware
    try { & $speakerPython -m platformio run -e $Board; if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed' } } finally { Pop-Location }
    New-Item -ItemType Directory -Path $speakerOutput -Force | Out-Null
    $speakerBootApp = Join-Path $env:PLATFORMIO_CORE_DIR 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
    & $speakerPython -m esptool --chip $profile.Chip merge-bin --output (Join-Path $speakerOutput ($profile.Name + '-factory.bin')) --flash-mode dio --flash-freq 40m --flash-size $profile.FlashSize $profile.BootOffset (Join-Path $speakerBuild 'bootloader.bin') 0x8000 (Join-Path $speakerBuild 'partitions.bin') 0xe000 $speakerBootApp 0x10000 (Join-Path $speakerBuild 'firmware.bin')
    if ($LASTEXITCODE -ne 0) { throw 'Firmware image assembly failed' }
    Copy-Item -LiteralPath (Join-Path $speakerBuild 'firmware.bin') -Destination (Join-Path $speakerOutput ($profile.Name + '-app.bin'))
    Get-FileHash -LiteralPath (Join-Path $speakerOutput ($profile.Name + '-factory.bin')) -Algorithm SHA256 | Format-List
}
if ($Action -in @('Backup','Flash','Update')) {
    $speakerBackupDirectory = Join-Path $speakerRoot 'backups'
    New-Item -ItemType Directory -Path $speakerBackupDirectory -Force | Out-Null
    $speakerBackup = Join-Path $speakerBackupDirectory ($profile.Name + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8) + '.bin')
    # Explicit chip type rejects accidental ESP32 / ESP32-S3 cross-flashing.
    & $speakerPython -m esptool --chip $profile.Chip --port $Port --baud 460800 --after $profile.Reset read-flash 0 $profile.Bytes $speakerBackup
    if ($LASTEXITCODE -ne 0 -or (Get-Item -LiteralPath $speakerBackup).Length -ne $profile.Bytes) { throw 'Full flash backup failed. No firmware was written.' }
    Get-FileHash -LiteralPath $speakerBackup -Algorithm SHA256 | Format-List
    Write-Output ('Original flash saved to ' + $speakerBackup)
}
if ($Action -eq 'Flash') {
    $speakerImage = Join-Path $speakerOutput ($profile.Name + '-factory.bin')
    & $speakerPython -m esptool --chip $profile.Chip --port $Port --baud 460800 --after $profile.Reset write-flash 0 $speakerImage
    if ($LASTEXITCODE -ne 0) { throw 'Flashing failed. Keep the original backup and retry over USB.' }
    & $speakerPython -m esptool --chip $profile.Chip --port $Port --baud 460800 --after $profile.Reset verify-flash 0 $speakerImage
    if ($LASTEXITCODE -ne 0) { throw 'Flash verification failed' }
    Write-Output 'Firmware written and verified. Switch the speaker on and complete Wi-Fi setup.'
}
if ($Action -eq 'Update') {
    $speakerImage = Join-Path $speakerOutput ($profile.Name + '-app.bin')
    if ((Get-Item -LiteralPath $speakerImage).Length -gt $profile.AppSize) { throw 'Application exceeds the installed app partition.' }
    $installedFlash = [IO.File]::ReadAllBytes($speakerBackup)
    $expectedPartitions = [IO.File]::ReadAllBytes((Join-Path $speakerBuild 'partitions.bin'))
    for ($index = 0; $index -lt $expectedPartitions.Length; $index++) {
        if ($installedFlash[0x8000 + $index] -ne $expectedPartitions[$index]) { throw 'Installed partition layout differs. Use Flash for first installation; no firmware was written.' }
    }
    & $speakerPython -m esptool --chip $profile.Chip --port $Port --baud 460800 --after $profile.Reset write-flash 0x10000 $speakerImage
    if ($LASTEXITCODE -ne 0) { throw 'Application update failed. Keep the full backup.' }
    & $speakerPython -m esptool --chip $profile.Chip --port $Port --baud 460800 --after $profile.Reset verify-flash 0x10000 $speakerImage
    if ($LASTEXITCODE -ne 0) { throw 'Application update verification failed' }
    Write-Output 'Application updated and verified. Existing Wi-Fi and speaker credentials were preserved.'
}
