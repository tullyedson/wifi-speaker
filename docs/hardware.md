# Muse Luxe hardware support

The supplied firmware targets the original **RASPIAUDIO Muse Luxe** using an ESP32, ES8388 codec and 4 MB flash. PSRAM is not required. Similar product names and later board revisions may use different audio hardware. Check the board before flashing; this firmware is not a universal ESP32 speaker image.

## Board routing

| Function | GPIO / device |
| --- | --- |
| Codec | ES8388, I2C address 0x10 |
| I2C SDA / SCL | 18 / 23 |
| I2S master clock | 0 |
| I2S bit clock / LR clock | 5 / 25 |
| I2S output to codec / input from codec | 26 / 35 |
| Speaker amplifier enable | 21 |
| RGB indicator | 22 |
| Middle button | 12 |
| Volume up / down | 19 / 32 |

The codec and ESP32 share I2S0 clocks at 16 kHz, 16-bit stereo. Firmware extracts one input channel as mono PCM and duplicates mono playback into both output channels. The microphone PGA is +21 dB; codec ALC is off. Internal microphone selection uses ADC control register 12 value `0x4C`, following the manufacturer's implementation. Firmware gain defaults to 1. Use the desktop per-speaker gain control for routine adjustment.

## Memory and transport

One audio task owns I2S. Fixed DMA buffers and queues avoid whole-recording allocations. The upload queue holds six 20 ms frames and drops its oldest frame if networking falls behind. Playback streams over HTTP in 320-sample blocks. Connection generations reject stale capture and playback work.

The partition map has two app slots of `0x1D0000` bytes each. The second is reserved; this release has no network OTA endpoint. Initial USB installation uses the factory image at offset 0. Updates to this layout write only the app at `0x10000`, preserving NVS. See [installation](installation.md).

## Audio controls and other devices

Firmware 0.1.1 accepts a saved volume from 0 to 100 in the hub's ready message and reports the applied value. Buttons change current volume in five-percent steps. Reconnection applies the saved desktop value again.

Other speaker types need their own board driver and provisioning implementation. They can share the [speaker WebSocket protocol](API.md#speaker-websocket): authenticated 16 kHz mono signed little-endian PCM upload, state/mute handling, authenticated WAV download and playback receipts. Implement the ready volume field and volume telemetry to support desktop volume control. Desktop microphone gain works with any compatible PCM source. Validate physical microphone pickup, codec routing, playback and reconnect behavior on each new board.

## Reference implementations

- [Manufacturer Muse WROVER source](https://github.com/RASPIAUDIO/Muse_library/blob/main/src/museWrover.cpp)
- [Manufacturer board definitions](https://github.com/RASPIAUDIO/Muse_library/blob/main/src/museWrover.h)
- [ESPHome Muse Luxe configuration](https://github.com/esphome/media-players/blob/main/raspiaudio/raspiaudio-muse-luxe.yaml)
- [Espressif esptool documentation](https://docs.espressif.com/projects/esptool/en/latest/esp32/)
