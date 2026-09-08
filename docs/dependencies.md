# Dependencies and model

Project source is MIT licensed. Dependencies retain their own licenses. `Cargo.lock` records the exact Rust dependency graph for this build.

Main components: Rust, Tokio, Axum, Reqwest, Tauri 2, whisper-rs 0.16.0 with whisper.cpp, Microsoft's Windows API bindings, Arduino-ESP32, ArduinoJson, arduinoWebSockets and Adafruit NeoPixel. See the package metadata and upstream repositories for complete licenses. The firmware links LGPL components including Arduino-ESP32 and arduinoWebSockets; firmware source and its reproducible build configuration are supplied alongside the binaries so the program can be rebuilt.

Local STT uses the English Whisper base model from [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp), derived from [OpenAI Whisper](https://github.com/openai/whisper), whose code and model weights are MIT licensed. The bundled model file is `ggml-base.en.bin`, 147,964,211 bytes, SHA-256:

```
a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002
```

Windows TTS uses the voices already installed in Windows. Voices are not redistributed in this project. The desktop's model-download feature contacts Hugging Face only when requested; normal inference does not use a cloud speech API.

Native Whisper/GGML logging hooks are installed without Rust logging backends, because verbose native logs can otherwise contain recognized words. Explicit CLI transcription prints its result by design.
