# WiFi Speaker

Turn a supported ESP32 speaker into a voice interface for any OpenAI-compatible chat endpoint or your own webhook app. A Rust Windows tray app handles local speech recognition, text routing and local text-to-speech. The ESP32 streams microphone audio over Wi-Fi and plays replies.

Choose your wake name, name and tag each speaker, and adjust each speaker's microphone gain and playback volume. Prompts and replies keep the originating speaker's identity so multiple rooms can share one hub.

## Requirements

- Windows 10/11 x64, Microsoft WebView2, and an installed Windows speech voice.
- An original **RASPIAUDIO Muse Luxe** with ESP32, ES8388 codec and 4 MB flash. Other hardware needs a board-specific firmware port; see [hardware support](docs/hardware.md).
- A 2.4 GHz Wi-Fi network whose devices can reach the Windows PC. The PC must stay awake while listening.
- An OpenAI-compatible chat-completions endpoint and model of your choice, or an app implementing the [webhook API](docs/API.md).

This repository contains source. Build the portable app and firmware using the steps below. There are no bundled accounts, model-server addresses, device registrations or credentials.

## Build on Windows

Install Git, the Rust stable **MSVC** toolchain, Python 3.11 or newer, and Visual Studio Build Tools with **Desktop development with C++** and CMake. Open PowerShell in the downloaded repository directory:

```powershell
.\scripts\bootstrap.ps1
.\scripts\build.ps1 -Test
.\scripts\build.ps1 -Release
.\target\release\speaker-hub.exe --download-model .\models
.\scripts\firmware.ps1 -Action Build
.\scripts\package.ps1
```

The bootstrap script installs pinned build helpers into `.tools`, inside this checkout. Cargo and PlatformIO download their dependencies. The explicit model-download command downloads the English Whisper model (148 MB) and verifies its size and SHA-256. You can also download it later from the app's settings.

Open `artifacts/Smart Speaker/Smart Speaker.exe`. Keep its `models` directory beside the executable if packaged with a model. Python and Rust are needed for building, not for running the portable desktop app. The output directory is self-contained, and `artifacts/desktop-sha256.json` lists relative file paths and hashes.

## Set up the hub

1. Open **Connections**. Choose **OpenAI-compatible chat**, enter your complete chat-completions URL, model ID and optional bearer token. A URL such as `https://llm.example.com/v1/chat/completions` is a placeholder, not a working service. For custom backends, choose **App webhook** instead.
2. Set **PC address reachable by speakers and apps** to your PC's LAN address with port 48490, or another configured port. The initial loopback address only works for tests on the PC itself. Leave the listen address at `http://0.0.0.0:48490` to accept LAN connections.
3. In **Speakers**, add a device, choose a name and tags, and save. Its **Setup details** contains the permanent ID and private device token.
4. In **Voice & listening**, choose your wake names and Windows voice. The initial wake name is **Speaker**. For example: “Speaker, tell me a short joke.” Any whole-word name anywhere in the phrase can activate it.
5. Allow the app through Windows Firewall on the Private network used by your speakers. The optional [LAN helper](docs/installation.md#lan-access) accepts the executable and port as parameters.

Closing Settings keeps the tray app running. Right-click its icon to pause listening or exit. **Start with Windows** is optional. Saving settings reconnects speakers and clears active requests.

Endpoint URLs, API keys, model selection, routing headers, wake names, audio adjustments and device registrations are saved in `%APPDATA%/com.smartspeaker.desktop/settings.json`. The app generates fresh API tokens locally. It does not encrypt this file. Keep it private. `SMART_SPEAKER_CONFIG_DIR` can select a different private settings directory.

## Install a Muse Luxe

Use a USB data cable, turn on the speaker and find its COM port in Windows Device Manager. In this example **COM7 is a placeholder**; replace it with your device's port:

```powershell
.\scripts\firmware.ps1 -Action Flash -Port COM7
```

The script makes and verifies a complete 4 MB backup before writing. **Flash replaces existing firmware and provisioning.** Keep the private backup for recovery. Only use this firmware with the supported board.

After flashing, join the device's `Speaker-setup-…` Wi-Fi network with password `speaker-setup`, open `http://192.168.4.1`, and enter your Wi-Fi details, hub URL and device ID/token. These settings stay in the device's flash, outside the source code. Setup closes after ten minutes; hold the middle button for five seconds to reopen it.

Once online, use **Test voice** and check the microphone meter. Adjust **Microphone gain** (0.25x to 8x) and **Playback volume** (0 to 100%) on that speaker's card, then save. A short middle-button press mutes the mic. The plus/minus buttons adjust volume for the current session; reconnecting restores the saved desktop volume.

For an existing installation using this project's partition map, use **Update** to preserve Wi-Fi and device credentials:

```powershell
.\scripts\firmware.ps1 -Action Update -Port COM7
```

See [installation and troubleshooting](docs/installation.md) for USB provisioning, indicators, backups and network setup.

## Integrate your application

The [API guide](docs/API.md) describes authenticated prompts, deferred replies, announcements to IDs or tags, live status, diagnostics and the speaker transport protocol.

- **OpenAI-compatible chat:** sends `model`, `max_tokens`, `stream: false`, and system/user messages to your configured URL; reads text from `choices[0].message.content`. The system context includes the PC's current date, time and UTC offset. This adapter is stateless.
- **App webhook:** sends a prompt with request ID, speaker ID/name/tags and callback URL. Return `{"text":"Your reply"}` immediately, or HTTP 202 and post a correlated response later. Your backend owns any conversation history.
- **Optional caller routing:** if your endpoint requires stable caller identity, configure a custom X- header and hub instance ID locally. This is disabled by default and has no dependency on a particular router.

## Behavior and limits

Speech recognition uses local Whisper; text-to-speech uses installed Windows voices. Normal operation keeps audio and transcripts in memory and sends only matching text prompts to your selected endpoint. See [privacy and security](SECURITY.md).

The hub transcribes phrases before matching a name. This allows the name at the end of a sentence but requires more CPU than a dedicated wake-word detector. Each speaker keeps at most two completed phrases waiting for recognition. Muting, starting a reply or reconnecting discards queued listening audio.

Up to 32 speakers may be registered; throughput depends on the PC and room activity. Each speaker has one reply in flight. Listening pauses during response playback, with no echo cancellation or barge-in. Playback is 16 kHz mono PCM, limited to three minutes. Speaker transport uses authenticated HTTP/WebSocket on a trusted LAN; do not expose the hub port to the internet.

## Development

See [contributing](CONTRIBUTING.md), [verification](docs/verification.md), and [dependency notices](docs/dependencies.md).

| Directory | Purpose |
| --- | --- |
| `crates/hub` | Rust audio processing, speech, HTTP/WebSocket API and CLI |
| `desktop`, `ui` | Tauri tray app and bundled settings interface |
| `firmware` | Muse Luxe audio, Wi-Fi, provisioning and transport |
| `scripts` | Build, package, firmware, simulator and diagnostic helpers |
| `docs` | Protocol, installation, hardware and third-party notices |

Project code is [MIT licensed](LICENSE). Dependencies retain their own licenses.
