# Verification

Run commands from the repository root after [bootstrapping the build helpers](../README.md#build-on-windows). The automated checks use generated test identities and loopback services. Credentials, hardware recordings, local reports and device backups are excluded from source control.

## Rust and firmware

```powershell
.\scripts\build.ps1 -Test
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
.\scripts\build.ps1 -Release
.\scripts\firmware.ps1 -Action Build -Board muse_luxe
.\scripts\firmware.ps1 -Action Build -Board spotpear_ball_v2
python .\scripts\check-public.py
```

Run Clippy in the same PowerShell session after the build helper so CMake and libclang are available. Tests cover phrase buffering, wake-name boundaries and aliases, malformed audio, mute/pause/disconnect, bounded recognition queues, speaker isolation, correlated replies, duplicate callbacks, announcements, stale generations, diagnostics, per-speaker gain/volume, clock context, generic OpenAI-compatible requests and configurable caller headers.

The Windows CI workflow runs the source privacy guard, Rust tests and Clippy, a release build, helper syntax checks, packaging and compilation of both firmware targets. Native UI and speech checks below require a Windows desktop with installed voices and are run separately. Firmware compilation does not connect to or write a USB device.

## Native settings interface

Close any running Smart Speaker tray app before this check, because the desktop enforces a single instance. Install the optional Playwright helper, then run:

```powershell
npm.cmd install --prefix .tools/ui-qa --no-save playwright@1.58.2
node .\scripts\ui-smoke.cjs
```

The script launches its own hidden test app with a temporary settings directory, loopback ports and WebView2 profile. It tests settings persistence/reload, independent speaker controls, names/tags, provisioning credential clearing, validation, occupied-port recovery, pause/resume and all UI sections. Reports and screenshots are written under `runtime/ui-*`. It terminates its own test process. Start your regular app again afterward.

## Local speech round trip

Download the model once, then run:

```powershell
.\target\release\speaker-hub.exe --download-model .\models
.\.tools\venv\Scripts\python.exe .\scripts\smoke-test.py
```

This synthesizes a neutral phrase, streams it through a simulated speaker to local Whisper, sends a matching prompt to a loopback callback app, and validates the returned speech WAV. It requires an installed Windows speech voice. It does not require an LLM endpoint or physical speaker.

## Optional model and hardware checks

After configuring your own OpenAI-compatible endpoint in Settings:

```powershell
.\.tools\venv\Scripts\python.exe .\scripts\live-model-test.py
```

This explicitly calls that endpoint using your local settings and a temporary simulated speaker. Provider usage may apply. It removes the temporary configuration containing the destination credential on completion. The test uses your configured wake name and validates a spoken test reply. A synthetic loopback success does not establish microphone range or physical sound quality.

For a connected physical device, use **Test voice**, then speak a phrase containing your wake name. Check the complete request and playback cycle and listen to the result. Test volume, microphone distance, mute, Wi-Fi recovery and power recovery in the intended room. Optional diagnostics are described in the [API guide](API.md#explicit-microphone-sample).

For SpotPear, also check the black display background, readable state text, touch volume/mute controls, and the 15-second backlight timeout. A first tap on a sleeping screen should wake it without changing mute or volume. Confirm that audio capture continues while the backlight is off. USB provisioning must acknowledge a complete message sent in one write and reconnect with the saved registration. An application update must preserve those settings and reject a different partition layout before writing.

## Public-source checks

`scripts/check-public.py` inspects tracked source for local network addresses, absolute workstation paths, credential-like content and forbidden runtime files. Third-party license notices retain their upstream attribution. A local pre-publication audit must also check Git history and actual private values without adding those values to a repository denylist. Passing a pattern scanner is one part of review, not proof that arbitrary new content is safe to publish.
