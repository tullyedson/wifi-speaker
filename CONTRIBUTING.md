# Contributing

Build from the repository root using the [README](README.md). Changes should keep the hub usable with unrelated OpenAI-compatible endpoints and custom webhook apps. Deployment-specific URLs, credentials, device identities, model selections and routing conventions belong in local settings.

Run the relevant checks in [docs/verification.md](docs/verification.md). Rust audio and routing changes need behavioral coverage through the public protocol. UI changes should be exercised in the native Tauri app. Firmware changes require the correct board and physical audio verification before claiming hardware support.

Use fictional test names and generated tokens. Keep reports, recordings, screenshots, private settings, flash backups and build outputs out of commits. Run `python scripts/check-public.py` and inspect `git diff --cached` before committing. Never weaken authentication to make a test pass.

Describe the problem, resulting behavior and validation in pull requests. Preserve third-party license and attribution files. New board support should document its pins, codec, flash layout, provisioning method and tested limitations.
