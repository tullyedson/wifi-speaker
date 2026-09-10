# Dependencies and model

Project source is MIT licensed. Dependencies retain their own licenses. `Cargo.lock` records the exact Rust dependency graph for this build.

Main components: Rust, Tokio, Axum, Reqwest, Tauri 2, whisper-rs 0.16.0 with whisper.cpp, Microsoft's Windows API bindings, Arduino-ESP32, ArduinoJson, arduinoWebSockets and Adafruit NeoPixel. See the package metadata and upstream repositories for complete licenses. The firmware links LGPL components including Arduino-ESP32 and arduinoWebSockets; firmware source and its reproducible build configuration are supplied alongside the binaries so the program can be rebuilt.

Local STT uses the English Whisper base model from [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp), derived from [OpenAI Whisper](https://github.com/openai/whisper), whose code and model weights are MIT licensed. The bundled model file is `ggml-base.en.bin`, 147,964,211 bytes, SHA-256:

```
a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002
```

Windows TTS uses the voices already installed in Windows. Voices are not redistributed in this project. The desktop's model-download feature contacts Hugging Face only when requested; normal inference does not use a cloud speech API.

Native Whisper/GGML logging hooks are installed without Rust logging backends, because verbose native logs can otherwise contain recognized words. Explicit CLI transcription prints its result by design.

SpotPear firmware additionally uses [Adafruit GC9A01A 1.1.1](https://github.com/adafruit/Adafruit_GC9A01A/tree/a03514ae042e990bc8d16f51aad7e87dabd989b1), pinned to its release commit, Adafruit GFX Library 1.12.6 and Adafruit BusIO 1.17.4. Included BSD notices cover [GC9A01A](third-party/adafruit-gc9a01a-1.1.1/NOTICE.md), [GFX](third-party/adafruit-gfx-1.12.6/LICENSE) and [BusIO](third-party/adafruit-busio-1.17.4/LICENSE). The ES8311 register initialization adapts Espressif's Apache-2.0 driver; its [attribution and license](third-party/es8311.md) are included.

The ES7210 ADC initialization is adapted from Espressif's Apache-2.0 driver. Its [attribution and license](third-party/es7210.md) are included. No microphone-array beamformer or acoustic echo canceller is bundled.
