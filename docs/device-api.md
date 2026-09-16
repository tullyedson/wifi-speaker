# Device control API

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
print(speaker.sensors())
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

Returns the permanent `speaker_id`, local `name` and `tags`, firmware version, `audio_ready`, `hub_connected`, microphone mute/gain/level/channel, current playback volume, `audio_uploading`, uptime, free heap and `settings_pending`. Nested objects describe the display, RGB indicator, current alarm and sensors. Microphone level is normalized RMS, not a percentage of recognition confidence.

`display.available` is false on headless boards. A display additionally reports `awake`, `screen`, `expression`, `brightness`, `timeout_ms`, `presence_timeout_ms`, `presence_control_active`, `sleep_reason`, `look_x`, `look_y`, `width`, `height` and touch availability. `sleep_reason` is `null` while awake, or `manual`, `idle` or `absence`. The LED object reports the requested effect/color/brightness and physical pixel count; BOX-3 reports zero pixels. Operational indicators may override cosmetic LED commands. Alarm states are `idle`, `queued`, `playing`, `finished`, `cancelled` and `error`. Audio readiness and playback completion are software observations; listen to the physical speaker to judge sound quality.

## GET /v1/sensors

Available in BOX-3 firmware 0.2.1 and builds containing the sensor driver. Returns the device's `api_version`, permanent `speaker_id`, `name`, `tags`, `uptime_ms` and `sensors`. The same sensor object is included in `/v1/device`. This endpoint uses the device control token and works without the speech hub.

The **ESP32-S3-BOX-3-SENSOR base** contains an AHT30 ambient temperature and relative humidity sensor. The small DOCK stand does not contain it. Attach the appropriate base with power off, then power the device normally. Sampling runs approximately every ten seconds, including with the display asleep or microphone muted. HTTP reads return the latest snapshot without triggering additional conversions. No readings are written to flash or retained as a history.

Example response values are illustrative:

```json
{
  "api_version": 1,
  "speaker_id": "speaker-kitchen",
  "name": "Kitchen",
  "tags": ["downstairs"],
  "uptime_ms": 15000,
  "sensors": {
    "temperature_humidity": {
      "supported": true,
      "sensor": "aht30",
      "available": true,
      "state": "ready",
      "temperature_c": 25.0,
      "temperature_f": 77.0,
      "humidity_percent": 50.0,
      "sampled_at_uptime_ms": 12000,
      "sample_age_ms": 3000,
      "poll_interval_ms": 10000
    }
  }
}
```

`supported` means that the firmware supports this sensor, not that the base is attached. Check `available` before using a value. `state` is `starting`, `ready`, `not_detected`, `bus_error`, `read_error`, `timeout`, `invalid_data`, `stale` or `unsupported`. An absent sensor is retried on the normal interval. Transport errors, checksum failures and invalid measurements make readings unavailable immediately. Samples older than 30 seconds are also unavailable. All measurement and sample-time fields are `null` when unavailable, never invented zero values or old values presented as current. Uptime timestamps are milliseconds since boot, not wall-clock time.

Other board targets return `supported:false`, `available:false`, `sensor:null` and `state:"unsupported"`, with null readings. Ordinary absence is HTTP 200 so a controller can discover capabilities. Missing/invalid authentication still returns 401. Older firmware without this route returns 404.

```powershell
python scripts/device_client.py --url http://192.0.2.20 --provision ./my-provision.json sensors
```

Use `DeviceClient.sensors()` from Python or poll the same HTTP route from another application. Route observations using `speaker_id`; names and tags are labels. These are measurements at the base, which can differ from room temperature due to nearby electronics, airflow and enclosure heating. Celsius/Fahrenheit conversion does not imply a calibrated room thermometer.

### Radar presence and automatic screen control

BOX-3 firmware **0.2.2** adds `sensors.presence` alongside temperature/humidity. The SENSOR base's AT581x radar supplies an active-high motion/presence indication. It does not identify people or measure distance, and someone sitting completely still may stop producing detections.

```json
{
  "supported": true,
  "sensor": "at581x",
  "available": true,
  "state": "ready",
  "detected": true,
  "sampled_at_uptime_ms": 14950,
  "sample_age_ms": 50,
  "last_detected_uptime_ms": 14950,
  "last_detection_age_ms": 50,
  "poll_interval_ms": 50
}
```

Poll `/v1/sensors` to retrieve this object. `detected` is the sampled radar output, including its short hardware hold, not the screen's longer absence timer. The last-detection timestamp records the most recent positive sample; it is null until the first detection. Radar initialization and its two-second self-test run without blocking the main loop. The output is sampled every 50 ms, and the module is checked over I2C every second. After a failed check it retries in five seconds. States are `starting`, `initializing`, `warming_up`, `ready`, `not_detected`, `bus_error`, `write_error`, `stale` and `unsupported`. A sample older than 500 ms is unavailable. All observation/time fields are null when unavailable, including `detected`. Missing hardware must never be interpreted as `detected:false`. Other board targets report unsupported.

Enable automatic screen control with a one-minute absence timeout:

```python
speaker.configure(presence_timeout_ms=60000)
```

Equivalent `PATCH /v1/config` body: `{"presence_timeout_ms":60000}`. Use 0 to disable, or 5,000..3,600,000 ms. This setting survives power loss. The public default is 0, so updating firmware alone preserves the previous screen behavior.

While a healthy radar is available, detection wakes the selected screen and keeps it on. No detection for the configured interval turns the backlight off. This policy overrides `screen_timeout_ms` while active. Touch, buttons, explicit API wake, reply processing and alarms provide a fresh timeout, so the screen stays readable during use even without a radar detection. The selected eyes/status page and expression remain unchanged. Manual API sleep continues to take priority until touch, a button, an alarm or an explicit wake. `display.presence_control_active` means a nonzero setting and a healthy radar, even if manual sleep currently overrides it.

If the radar becomes unavailable, a screen put to sleep by absence wakes and falls back to the ordinary `screen_timeout_ms` behavior. A new full grace period starts when the sensor recovers. Presence changes only the backlight; Wi-Fi, microphone capture, wake-name recognition at the connected hub and temperature sampling continue. This does not put the ESP32 into deep sleep. No sensor observations are written to flash.

BOX-3 has **no built-in camera**. Its SENSOR base's infrared emitter/receiver and radar are not image sensors. The separate DOCK can accept supported USB cameras with additional USB-host software; this firmware does not expose camera images, infrared, IMU, SD-card or battery telemetry.

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
| `presence_timeout_ms` | 0 disables radar screen control (default); 5,000..3,600,000 ms of absence turns off the backlight when a healthy radar is available |
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

Supported on SpotPear Ball V2 and ESP32-S3-BOX-3. Headless boards return 409.

```json
{"screen":"eyes","awake":true,"expression":"curious","look_x":0.6,"look_y":-0.2,"duration_ms":10000}
```

All fields are optional, but send at least one. `screen` is `eyes` or `status`; `awake` and `blink` are booleans. Supported expressions: `neutral`, `happy`, `curious`, `sleepy`, `excited`, `love`, `sad`, `surprised`, `thinking`. Expressions are drawn and animated locally, so the caller need not stream animation frames.

The eyes screen has no text. Eye color follows the device's current audio phase, independently of the selected expression: mint for listening, sky blue for recognition, violet while waiting for the AI, gold for speech preparation, green for playback, amber for mute/pause, coral red for alarm/audio error and slate blue for connection/setup. The audio hub's existing state messages drive these changes; expression commands do not override the phase color. Text labels remain on the status screen.

Physical RGB LEDs use the same palette in their normal `status` effect on all boards. Alarms blink coral red. Explicit cosmetic LED effects remain independent of eye color, with the operational overrides described below.

Gaze coordinates are finite numbers from -1 to 1, with positive x looking right and positive y looking down. Supplying either coordinate fixes the gaze until the expression expires or another expression command releases it. Otherwise the eyes look around on their own. Natural blinking continues; `blink:true` also requests an immediate blink. `duration_ms` ranges from 0 to 600,000, with 0 holding the expression until replaced. Expiry returns to neutral wandering eyes. `duration_ms` applies to the expression/gaze, not the screen's awake state.

`{"awake":false}` turns off the backlight without stopping Wi-Fi or microphone capture. This manual sleep lasts until a touch, physical button, explicit wake command or alarm. An automatic idle timeout can also darken the display; reply processing wakes it unless it was manually put to sleep. This is display sleep, not ESP32 deep sleep. A device switched physically off cannot listen or receive a wake request.

The display boards' BOOT button cycles the eyes/status screens and saves the choice. BOX-3's front capacitive button can also cycle screens. Separate hardware power/reset buttons retain their functions. Holding BOOT for five seconds still opens setup. Touching the eyes gives a brief happy reaction. The status screen has microphone mute, playback volume and input-gain controls. First touch on a dark screen only wakes it.

The LCD uses an LED backlight, not OLED pixels. Always-on animated eyes are the default; brightness and timeout remain adjustable. No panel is promised to be immune to image retention.

## POST /v1/led

Boards without a programmable RGB indicator, including BOX-3, return 409. Display phase colors remain available independently.

```json
{"color":"#FFB0CC","effect":"breathe","brightness":25,"duration_ms":15000}
```

`effect`: `solid`, `breathe`, `blink` or `status`. Color must be `#RRGGBB`; brightness is 0..100; duration is 0..600,000 ms, with 0 holding the choice. Defaults are pale teal, solid, 20% and no expiry. Expiry or `effect:"status"` restores operational colors. Audio initialization errors, provisioning, mute/pause and alarms take precedence so their indicators remain visible. Waveshare drives all seven LEDs together; the round board and Muse each have one indicator.

## POST /v1/alarms and /v1/alarms/stop

```json
{"id":"timer-42","duration_ms":5000,"pattern":"chime","volume":45}
```

Alarm IDs contain 1..64 letters, digits, hyphens or underscores. Duration is required and bounded to 100..60,000 ms. Patterns are `chime` (default), `beep` and `siren`; volume is 0..100, default 50. The device generates the tone locally and can play it with the speech hub disconnected. Acceptance returns HTTP 202. Observe `/v1/device` for queued/playing/finished state.

An alarm takes priority over a spoken reply, discards queued speech, temporarily suspends microphone upload, wakes the display and flashes the RGB indicator. It closes and automatically reconnects the audio WebSocket so protocol-1 hubs discard the interrupted request without mistaking cancellation for completed speech. The prior microphone mute setting is restored afterward; normal audio resumes when both the alarm and reconnection finish. An in-progress HTTP transfer can delay alarm playback until its bounded network call returns, up to approximately five seconds. An audio-device failure can still prevent physical sound.

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
