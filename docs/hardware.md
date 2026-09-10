# Speaker hardware support

## Muse Luxe

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

## SpotPear Ball V2 / 1.28-inch BOX

PlatformIO target: `spotpear_ball_v2`. This is the newer ESP32-S3R8 board with 16 MB flash and optional battery/touch. The manufacturer's product names include `ESP32-S3-1.28inch-AI-legs-Bat-Touch` and `ESP32-S3-LCD-1.28-BOX`. The smaller Ball V1 has different wiring. The similarly named Waveshare ESP32-S3-AUDIO-Board is also a different device.

| Function | GPIO / device |
| --- | --- |
| Audio codec | ES8311, I2C address 0x18 |
| Audio I2C SDA / SCL | 15 / 14 |
| I2S master / bit / LR clock | 16 / 9 / 45 |
| I2S output / input | 8 / 10 |
| NS4150B amplifier enable | 46 |
| 240 x 240 round display | GC9A01A |
| Display clock / MOSI / CS / DC / reset | 4 / 2 / 5 / 47 / 38 |
| Display backlight | 42, active low |
| Touch controller | CST816 family, I2C address 0x15 |
| Touch SDA / SCL / reset / interrupt | 11 / 7 / 6 / 12 |
| BOOT button / RGB indicator | 0 / 48 |

The codec runs in slave mode with 4.096 MHz MCLK and 16 kHz, 16-bit I2S. Input uses the left microphone channel by default. Analog microphone gain is 24 dB, with desktop gain available separately. Playback gain is applied to PCM before duplicating it into both output slots. Echo cancellation and barge-in are not implemented.

The display shows listening, processing, playback, mute, connection and setup states. Touch is optional and probed on startup; the button still works without a touch controller. Touch controls change session volume and mute. The displayed meter uses device RMS with a 25% full-scale range; the desktop meter also accounts for its configured software gain. The SD slot and battery telemetry are not used by this firmware.

The background is black. PWM keeps the active-low backlight at approximately 14% duty while awake, then turns it off after 15 seconds without interaction or reply processing/playback. The first touch wakes the screen without activating a control. Listening continues while the screen is dark. The panel requires color inversion enabled; the SPI-pointer Adafruit constructor takes DC before CS.

The 16 MB layout contains two 3 MB application slots; only app0 is used. Native USB-Serial/JTAG stays enabled for provisioning. Initial installation uses the ESP32-S3 bootloader at offset 0, unlike the Muse ESP32 bootloader at 0x1000. Both applications start at 0x10000. Keep each board's image and flash backup separate.

The helper uses a watchdog reset after SpotPear USB operations because RTS reset can leave this board in the ROM downloader. If using esptool directly, use `--after watchdog-reset`. Native USB input has a 4 KiB buffer to accommodate provisioning messages arriving in USB bursts.

Sources: [SpotPear product](https://spotpear.com/shop/ESP32-S3-N16R8-AI-DeepSeek-XiaoZhi-XiaGe-Qwen-DouBao-1.28-inch-LCD/ESP32-S3-1.28inch-AI-legs-Bat-Touch.html), [manufacturer guide and pin table](https://spotpear.com/wiki/ESP32-S3-N16R8-AI-DeepSeek-XiaoZhi-XiaGe-Qwen-DouBao-1.28-inch-Round-LCD-BOX-TouchScreen.html), [manufacturer schematic](https://cdn.static.spotpear.com/uploads/picture/learn/ESP32/ESP32-S3-LCD-1.28-BOX-THIN/ESP32S3-1.28inch-BOX.pdf), [reference Ball V2 implementation](https://github.com/RealDeco/xiaozhi-esphome/blob/main/devices/Under_Development/Modular/HW/ball_v2_hw.yaml), [ES8311 attribution](third-party/es8311.md).

## Reference implementations

- [Manufacturer Muse WROVER source](https://github.com/RASPIAUDIO/Muse_library/blob/main/src/museWrover.cpp)
- [Manufacturer board definitions](https://github.com/RASPIAUDIO/Muse_library/blob/main/src/museWrover.h)
- [ESPHome Muse Luxe configuration](https://github.com/esphome/media-players/blob/main/raspiaudio/raspiaudio-muse-luxe.yaml)
- [Espressif esptool documentation](https://docs.espressif.com/projects/esptool/en/latest/esp32/)
