# Device control API, firmware 0.2.0

Each device serves its own HTTP API on port 80. This works independently of the reference Windows hub. Use the device's DHCP address, or `http://<speaker_id>.local` where mDNS is supported. All `/v1/` routes require `Authorization: Bearer <control_token>` and return `Cache-Control: no-store`. POST and PATCH bodies use `application/json`, with a maximum accepted size of 4,096 bytes. No cloud account is needed.

A private USB provisioning file can contain a separate `control_token`. For compatibility, omitting it uses that device's existing `speaker_token`. Tokens contain 32 to 256 ASCII letters, digits, hyphens or underscores. A controller can set a separate control token with an authenticated configuration update. Read endpoints never return either token or Wi-Fi credentials. Use the speaker token for the audio WebSocket, and the control token for these device routes. The hub's app token does not authorize the device API.

The transport is plain HTTP on a trusted LAN. Keep device and hub ports off the public internet. Supply credentials in headers, not URLs. The Python reference client refuses redirects.

## Quick start for an integrating application

`scripts/device_client.py` is a small standard-library Python reference. The existing Rust hub is the STT/TTS and audio-transport reference. Your application may implement the same contracts in another language.

```python
import os
from scripts.device_client import DeviceClient

speaker = DeviceClient(os.environ['SPEAKER_URL'],
                       os.environ['SPEAKER_CONTROL_TOKEN'])
print(speaker.status())
speaker.display(screen='eyes', awake=True, expression='happy', duration_ms=8000)
speaker.led(color='#77EEDD', effect='breathe', brightness=20, duration_ms=8000)
speaker.alarm('timer-42', duration_ms=5000, pattern='chime', volume=45)
speaker.stop_alarm('timer-42')
```

The CLI reads the token from `SPEAKER_CONTROL_TOKEN` or a private provisioning file. For example, replacing the documentation address with the device address:

```powershell
python scripts/device_client.py --url http://192.0.2.20 --provision ./my-provision.json status
python scripts/device_client.py --url http://192.0.2.20 --provision ./my-provision.json display --body ./my-display-command.json
```

Keep provisioning, tokens and command files containing private settings out of Git.

## GET /v1/device

Returns the permanent `speaker_id`, local `name` and `tags`, firmware version, `audio_ready`, `hub_connected`, microphone mute/gain/level/channel, current playback volume, `audio_uploading`, uptime, free heap and `settings_pending`. Nested objects describe the display, RGB indicator and current alarm. Microphone level is normalized RMS, not a percentage of recognition confidence.

`display.available` is false on headless boards. A round display additionally reports `awake`, `screen`, `expression`, `brightness`, `timeout_ms`, `look_x`, `look_y` and touch availability. The LED object reports the requested effect/color/brightness and physical pixel count; operational indicators may override it. Alarm states are `idle`, `queued`, `playing`, `finished`, `cancelled` and `error`. Audio readiness and playback completion are software observations; listen to the physical speaker to judge sound quality.

## GET and PATCH /v1/config

GET returns non-secret configuration. PATCH merges only the supplied fields, validates the complete result and saves it in ESP32 NVS flash before reporting success. Unknown fields, null values and incorrect types are rejected. `speaker_id` stays fixed through this API; change it through USB provisioning when moving the device to a different identity.

| Field | Meaning and range |
| --- | --- |
| `name`, `tags` | Name: 1..64 UTF-8 bytes. Up to 16 tags, each 1..64 bytes |
| `mic_gain` | Input PCM gain, 0.25..8, default 1 |
| `mic_channel` | Select I2S microphone slot 0 or 1 |
| `volume` | Playback volume, integer 0..100 |
| `muted` | Boolean, stops microphone upload |
| `brightness` | LCD backlight percentage, integer 1..100, default 30 |
| `screen_timeout_ms` | 0 for always on, or 5,000..3,600,000 ms |
| `default_screen` | `eyes` or `status`; default `eyes` |
| `hub_url` | Reachable HTTP audio-hub origin, hostname/IPv4 and optional port, with no path/query/credentials |
| `speaker_token` | Credential for the configured audio hub |
| `control_token` | Credential for this device's control API |
| `wifi_ssid`, `wifi_password` | Replacement 2.4 GHz network credentials |

```json
{"name":"Kitchen","tags":["downstairs"],"mic_gain":2.0,"volume":55,"screen_timeout_ms":0}
```

Ordinary controls apply immediately and return `{"saved":true,"restarting":false}`. Changing the network, audio destination or either token returns `{"saved":true,"restarting":true}` and restarts shortly afterward. Reconnect using the resulting device address and control token. Changes survive reset and power loss. Physical/touch adjustments are saved after two seconds without another adjustment; allow that interval before removing power.

Volume is initially taken from the reference hub's ready message. Once you set volume on the device, through a physical button, touch or PATCH, the device owns that saved value and restores it across reconnections and power cycles. GET config reports `volume:-1` while the initial hub default still owns it; PATCH only accepts 0..100. The reference desktop's saved volume is then only an initial default, and its device-volume telemetry shows what is actually playing.

Firmware `mic_gain` scales the raw network audio and therefore affects a receiving hub's speech threshold. The reference desktop's separate `microphone_gain` applies after phrase detection. Avoid accidentally multiplying both settings when integrating your own recognizer. The on-screen gain control adjusts the device's `mic_gain`.

The device name/tags and reference hub registration are separate local records joined by the permanent ID. Keep them aligned when using both APIs. The legacy audio handshake stays unchanged so older hubs continue working.

### Send audio to another computer

Implement the [speaker WebSocket protocol](API.md#speaker-websocket) on the receiving computer and register the same speaker ID/token there. Then PATCH `hub_url` and, if needed, `speaker_token` on the device. It saves the configuration, restarts and connects to that receiver automatically. The receiver supplies the ready/state messages and consumes mono signed 16-bit little-endian PCM at 16 kHz, normally 320 samples every 20 ms.

This destination is an audio hub, not a text-only LLM URL. The receiver performs STT/wake-name recognition and calls its chosen AI. For speech output it serves a bounded mono PCM WAV through the authenticated audio path and sends a play command. The supplied Rust code provides a reference for the full cycle.

## POST /v1/display

Supported on the round SpotPear target. Headless boards return 409.

```json
{"screen":"eyes","awake":true,"expression":"curious","look_x":0.6,"look_y":-0.2,"duration_ms":10000}
```

All fields are optional, but send at least one. `screen` is `eyes` or `status`; `awake` and `blink` are booleans. Supported expressions: `neutral`, `happy`, `curious`, `sleepy`, `excited`, `love`, `sad`, `surprised`, `thinking`. Expressions are drawn and animated locally, so the caller need not stream animation frames.

Gaze coordinates are finite numbers from -1 to 1, with positive x looking right and positive y looking down. Supplying either coordinate fixes the gaze until the expression expires or another expression command releases it. Otherwise the eyes look around on their own. Natural blinking continues; `blink:true` also requests an immediate blink. `duration_ms` ranges from 0 to 600,000, with 0 holding the expression until replaced. Expiry returns to neutral wandering eyes. `duration_ms` applies to the expression/gaze, not the screen's awake state.

`{"awake":false}` turns off the backlight without stopping Wi-Fi or microphone capture. This manual sleep lasts until a touch, physical button, explicit wake command or alarm. An automatic idle timeout can also darken the display; reply processing wakes it unless it was manually put to sleep. This is display sleep, not ESP32 deep sleep. A device switched physically off cannot listen or receive a wake request.

The round BOOT button cycles the eyes/status screens and saves the choice. It leaves the separate physical power button alone. Holding BOOT for five seconds still opens setup. Touching the eyes gives a brief happy reaction. The status screen has microphone mute, playback volume and input-gain controls. First touch on a dark screen only wakes it.

The LCD uses an LED backlight, not OLED pixels. Always-on animated eyes are the default; brightness and timeout remain adjustable. No panel is promised to be immune to image retention.

## POST /v1/led

```json
{"color":"#FFB0CC","effect":"breathe","brightness":25,"duration_ms":15000}
```

`effect`: `solid`, `breathe`, `blink` or `status`. Color must be `#RRGGBB`; brightness is 0..100; duration is 0..600,000 ms, with 0 holding the choice. Defaults are pale teal, solid, 20% and no expiry. Expiry or `effect:"status"` restores operational colors. Audio initialization errors, provisioning, mute/pause and alarms take precedence so their indicators remain visible. Waveshare drives all seven LEDs together; the round board and Muse each have one indicator.

## POST /v1/alarms and /v1/alarms/stop

```json
{"id":"timer-42","duration_ms":5000,"pattern":"chime","volume":45}
```

Alarm IDs contain 1..64 letters, digits, hyphens or underscores. Duration is required and bounded to 100..60,000 ms. Patterns are `chime` (default), `beep` and `siren`; volume is 0..100, default 50. The device generates the tone locally and can play it with the speech hub disconnected. Acceptance returns HTTP 202. Observe `/v1/device` for queued/playing/finished state.

An alarm takes priority over a spoken reply, discards queued speech, temporarily suspends microphone upload, wakes the display and flashes the RGB indicator. The prior microphone mute setting is restored afterward. An in-progress HTTP transfer can delay alarm playback until its bounded network call returns, up to approximately five seconds. An audio-device failure can still prevent physical sound.

Only one alarm may be active. To stop it, POST `{"id":"timer-42"}` to `/v1/alarms/stop`. A short BOOT/middle-button press also stops the current alarm; Waveshare KEY2 can do the same. A stop for a different ID returns 404 and cannot cancel a newer alarm. Identical retries of an alarm ID do not replay it, even after finish/cancellation. Reusing an ID with different parameters returns 409. Receipts retain at most eight IDs for ten minutes in RAM. They do not survive reboot, and an active alarm is not replayed after reboot.

Schedule future alarms in the calling application, then trigger them at the desired time. This release does not implement a persistent on-device calendar or RTC scheduling.

## Responses and integration checks

| Status | Meaning |
| --- | --- |
| 200 | Read, applied control, saved config, cancellation or duplicate alarm |
| 202 | New alarm queued |
| 400 | Invalid JSON, unknown fields or invalid values |
| 401 | Missing or invalid device control token |
| 403 | Setup write attempted outside the physically opened setup access point |
| 404 | Unknown endpoint or wrong alarm ID |
| 409 | Unsupported display, active alarm or conflicting alarm ID |
| 413 | Accepted JSON size limit exceeded |
| 503 | Unprovisioned/setup/restarting device or unavailable audio hardware |

Verify authorization failures, malformed configuration, no-display handling, token rotation, persistence, route changes, duplicate/conflicting alarm IDs, cancellation and reconnection. For a display board, also verify physical screen cycling, touch gain/volume, expression changes, and continued microphone upload with the backlight off. Keep test recordings and installation-specific addresses out of public source.
