# Privacy and security

## Data flow

Speakers stream microphone PCM to the Windows hub. The hub detects phrases, transcribes them locally, and sends only text matching a configured wake name to the selected endpoint. Replies are synthesized locally using Windows voices and fetched by the originating device. Normal operation does not save recordings, transcripts or conversation history.

The endpoint you configure controls its own data retention. Explicit CLI transcription, microphone diagnostics and test helpers can print text or save test artifacts. Treat those outputs as private.

## Local secrets

The hub stores API tokens, endpoint credentials and device registrations in its user settings directory. These values are not encrypted by the app. Each installation generates new app/device tokens. Wi-Fi credentials are provisioned at runtime and stored in the speaker's NVS. Full flash backups can contain them.

Do not commit settings, provisioning files, `.env` files, flash backups, recordings, logs, runtime reports or generated packages. Default locations and common filenames are ignored by Git, but renamed files still require review. Third-party endpoint addresses, model IDs and custom routing headers belong in local settings.

## Network boundary

The speaker transport uses bearer-authenticated HTTP and WebSocket on a trusted LAN, without TLS. Do not forward the hub port to the internet. Keep the app API token separate from each device token. The settings editor is local to the desktop app and is not served over the network. Provision only when needed; the setup access point has a shared bootstrap password and expires after ten minutes.

The device control API runs on port 80 and uses a separate optional control token, falling back to its device audio token for existing provisioning files. It can change microphone routing and trigger sound, so give it only to intended device controllers. Configuration reads omit Wi-Fi credentials and both tokens. Setup writes require a connection through the physically opened setup access point. Direct device controls are documented in [the device API guide](docs/device-api.md).

## Reporting

For reproducible non-sensitive bugs, open a repository issue with sanitized details. Do not post credentials, recordings, endpoint addresses or private configuration. For a security issue containing sensitive information, use GitHub's private vulnerability reporting option if it is enabled for the repository. If unavailable, first request a private contact channel without disclosing the sensitive details publicly.
