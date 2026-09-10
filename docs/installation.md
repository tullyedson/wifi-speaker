# Installation and troubleshooting

Start with the [build and setup steps](../README.md). All addresses, device IDs, credentials and model choices belong to your installation. No existing installation is included in the repository.

## Desktop settings

Run `Smart Speaker.exe` from the portable output directory. It needs Windows 10/11 x64, WebView2 and a Windows speech voice. Closing the window leaves the tray hub running. Exit from the tray menu to stop it.

Settings are created in `%APPDATA%/com.smartspeaker.desktop/settings.json`. A new installation has no speakers, destination URL, model ID or destination token. It generates a fresh app API token. Adding a speaker generates that device's separate token. Set `SMART_SPEAKER_CONFIG_DIR` before launching to use a different private settings directory.

The local speech model can be placed in the portable app's `models` directory or selected in **Voice & listening**. **Download base.en** verifies both the file size and hash. Choose an installed Windows voice or leave the Windows default.

In **Connections**, choose a complete OpenAI-compatible chat-completions URL and model ID. Supply a bearer token only if your endpoint requires one. The optional caller-header settings default to empty. If an endpoint documents a custom caller header, enter that header and a unique hub instance ID under **Optional caller routing**. No specific router is required. For webhook backends, configure the separate app API token in the application that will send callbacks.

## LAN access

Find your PC's LAN IPv4 address with `ipconfig`. Enter `http://<your-PC-IP>:48490` as the public address in Settings. The wildcard listen URL `http://0.0.0.0:48490` means listen on this PC's interfaces. Loopback `127.0.0.1` is only reachable from the PC itself. The documentation-only address `192.0.2.10` is a placeholder and must be replaced.

The speakers and PC need network reachability, even if Wi-Fi and Ethernet are used separately. Guest isolation, IoT VLAN rules and blocked TCP ports can prevent connections. Use a DHCP reservation for the PC if possible; otherwise a changed PC address also requires updated speaker provisioning.

Allow the portable executable on the selected TCP port in Windows Firewall's **Private** profile. An optional administrator PowerShell helper creates a rule restricted to the local subnet:

```powershell
.\scripts\enable-lan.ps1 -Port 48490
```

It finds the portable app relative to the repository. If you moved it, pass `-Executable` with your installed executable path. It records the rule and existing application rules under `runtime`. It does not disable existing firewall block rules; resolve those explicitly in Windows Firewall if one conflicts.

## USB and firmware

The original Muse Luxe ES8388 and SpotPear Ball V2 / 1.28-inch BOX ES8311 boards are supported. See [hardware.md](hardware.md) before flashing. Use a USB data cable, turn on the device and locate its COM port in Device Manager. Muse Luxe uses a Silicon Labs CP210x USB bridge; SpotPear uses the ESP32-S3 native USB JTAG/serial interface. Close any serial terminal using that port.

From the repository directory, replacing COM7 with the actual port:

```powershell
.\scripts\firmware.ps1 -Action Backup -Port COM7
.\scripts\firmware.ps1 -Action Flash -Port COM7
```

These commands default to Muse Luxe. Add `-Board spotpear_ball_v2` to every firmware command for the SpotPear device. **Backup** reads all flash: 4 MB for Muse Luxe or 16 MB for SpotPear. **Flash** builds the selected factory image, creates another complete backup, writes the bootloader, partitions and app at offset 0, and independently verifies the written image. It replaces existing firmware and provisioning. Never interrupt power during writing.

For later updates using this project's partition layout:

```powershell
.\scripts\firmware.ps1 -Action Update -Port COM7
```

**Update** backs up all flash, checks that the installed partition table matches the selected target, writes only the application at `0x10000`, and verifies it. It preserves NVS, which contains Wi-Fi credentials and device identity. Use **Flash** for the initial installation. **Build** only creates images and requires no USB port. `-ToolsDirectory` optionally reuses a previously bootstrapped `.tools` directory when building in a separate checkout.

Private flash backups live under `backups/` and may contain credentials. Keep them outside shared files and public releases. To restore a verified full backup on the same device, use the bundled esptool to write it at offset 0 and run `verify-flash` against the same file. Restoration replaces all device state with that backup.

## Provisioning

Add and save a speaker in the desktop first. Its setup dialog contains the hub URL, permanent ID and device token. After flashing, join `Speaker-setup-…` (password `speaker-setup`) and open `http://192.168.4.1`. Enter your 2.4 GHz Wi-Fi credentials and the saved hub/device values. The device restarts after saving.

The setup access point closes after ten minutes. Hold the Muse Luxe middle button or SpotPear BOOT button for five seconds to reopen it. Provision near the device on a trusted network, since setup has a shared bootstrap password.

Alternatively, use **Provision over USB** in the desktop setup dialog. Enter Wi-Fi credentials, copy the JSON into a private `my-provision.json` file, and run:

```powershell
.\scripts\provision.ps1 -Port COM7 -Config .\my-provision.json
```

Delete that temporary file after successful setup. Its Wi-Fi password and device token are not needed in the checkout. Provisioning filenames and local settings are ignored by Git.

## Check sound and listening

Wait for the device to appear online. Select **Test voice**, then say a phrase containing your configured wake name. The initial wake name is **Speaker**. A request should pass through transcribing, waiting, speaking and back to listening. A playback acknowledgement means the device finished its audio stream; judge volume and clarity by listening.

Each speaker has saved microphone gain and playback volume. Increase gain gradually and avoid a continuously red mic meter. Gain affects recognition audio after raw phrase segmentation. Lower the global speech threshold if quiet speech does not start a phrase; raise it if room noise keeps starting phrases. Higher gain does not itself lower the raw speech threshold.

The buttons mute/unmute, adjust session volume and reopen setup. Reconnection restores the desktop's saved volume. LED states:

| Color | Meaning |
| --- | --- |
| Green | Listening |
| Yellow | Processing |
| Purple | Playback |
| Amber | Muted or paused |
| Blue | Setup mode |
| Red | Disconnected or audio initialization failure |

The SpotPear screen uses a black background and a dim backlight. After 15 seconds idle, the backlight turns off while the microphone keeps listening. Tap once to wake it; subsequent center taps mute/unmute, and minus/plus taps change volume. Reply processing/playback also wakes the screen. The BOOT button works without touch support.

If the mic is silent, check USB startup diagnostics (`audio=ready`), device mute and channel selection. If you see phrases but no response, check wake names, the endpoint URL, model ID, bearer token and the device's error status. Long transcription times depend on model size, CPU and concurrent room activity. Use [explicit microphone diagnostics](API.md#explicit-microphone-sample) when needed. Diagnostics can contain private speech; keep their output local.
