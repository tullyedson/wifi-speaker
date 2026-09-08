# Smart Speaker API v1

Base URL: the hub's configured public URL. `http://192.0.2.10:48490` below is a documentation-only example; replace it with your PC's reachable LAN address. Listen URL accepts an HTTP IP address and port, without a path. Callback and speaker paths below are fixed. Requests use JSON and UTF-8. Request IDs are UUIDs.

## Authentication and identity

Apps use `Authorization: Bearer <app-api-token>` for `/v1/speakers`, `/v1/responses`, and `/v1/speak`. Copy this token from Connections. Each speaker has a different token that grants access only to its own microphone WebSocket and generated audio. Tokens do not go in URLs.

The hub uses the optional destination token when sending prompts to your app. It does not put the callback token into outbound prompts. Configure that separately in your callback app.

Speaker IDs contain 1 to 64 letters, numbers, hyphens, or underscores. They are the stable routing key. Names are display text. Tags match exactly, with case sensitivity. Multiple speaker or tag selections form a union with duplicate targets removed.

## Prompt sent by the hub

The hub POSTs the following to the configured prompt destination in webhook mode:

```json
{
  "version": 1,
  "request_id": "d553e571-6e1f-4e76-9dd8-9dcab56c10c3",
  "conversation_id": "speaker-kitchen",
  "speaker": {
    "id": "speaker-kitchen",
    "name": "Kitchen",
    "tags": ["downstairs", "family"]
  },
  "text": "Please tell me the weather, Speaker.",
  "wake_name": "Speaker",
  "response_url": "http://192.0.2.10:48490/v1/responses"
}
```

The full recognized phrase is sent, including the wake name. `conversation_id` is the speaker ID; your backend decides how long its conversation history lasts. `X-Speaker-Id` and `X-Request-Id` headers also carry the identifiers.

Return HTTP 202 or 204 to acknowledge a deferred reply. Return HTTP 200 with `{"text":"Your reply"}` to speak immediately. An empty success body also waits for a callback. Outbound redirects are not followed. Non-success responses, malformed JSON, or network timeouts return the speaker to listening and appear in its status. There is no automatic outbound retry that could duplicate app work.

The default response deadline is 120 seconds, configurable from 10 to 600. A callback may arrive before the original HTTP acknowledgement. Once an accepted callback owns the response, a later outbound network failure does not discard it.

## POST /v1/responses

```json
{
  "request_id": "d553e571-6e1f-4e76-9dd8-9dcab56c10c3",
  "speaker_id": "speaker-kitchen",
  "text": "It is warm and sunny."
}
```

The request ID and speaker ID must match an outstanding prompt. The hub synthesizes speech and delivers it to that speaker. Text must contain 1 to 12,000 UTF-8 bytes; the generated audio must also fit the playback duration limit.

| Result | Meaning |
| --- | --- |
| 202 `{"status":"accepted"}` | Synthesis and delivery were accepted |
| 200 `{"status":"duplicate"}` | The same speaker, request ID, and text were already accepted |
| 401 | Missing or invalid app token |
| 409 `{"error":"…"}` | No matching pending request, expired request, or conflicting text |
| 400 / 422 | Invalid JSON or payload fields |
| 413 | Request body exceeds 32 KiB |

Acceptance is not a physical playback receipt. Check speaker status for synthesis, connectivity, or playback errors. Identical retries are suppressed while pending and for ten minutes after completion. Completed receipt history is bounded to 2,048 entries. Changing text under the same key is rejected. A disconnected/replaced speaker session invalidates its pending prompt and audio URLs; do not deliver an old answer to a newly connected session.

### Minimal Python callback

```python
import json
import urllib.request

def send_reply(prompt, text, app_api_token):
    body = json.dumps({
        "request_id": prompt["request_id"],
        "speaker_id": prompt["speaker"]["id"],
        "text": text,
    }).encode("utf-8")
    request = urllib.request.Request(
        prompt["response_url"], data=body, method="POST",
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + app_api_token},
    )
    with urllib.request.urlopen(request, timeout=15) as response:
        return json.load(response)
```

## POST /v1/speak

App-initiated speech does not need a preceding microphone prompt:

```json
{
  "request_id": "91717372-361c-46d2-bef1-f15c82a1d7e4",
  "speaker_ids": [],
  "tags": ["downstairs"],
  "text": "Dinner is ready."
}
```

Successful result: HTTP 202 `{"status":"accepted","speaker_ids":["speaker-kitchen","speaker-lounge"]}`.

Provide explicit IDs, tags, or both. There is no implicit broadcast. Every newly selected speaker must be enabled, online, unmuted, and free of another request; otherwise the group is rejected with HTTP 409. An unmatched tag alone is rejected. With a nonempty matching union, an additional unmatched tag contributes no targets. A listed unknown/disabled ID rejects the request.

Retry the same UUID, text, and target selection for idempotent delivery within the receipt window. Tag membership is resolved at request time; keep targets unchanged for retries. The API does not guarantee simultaneous synchronized audio across rooms. Once accepted, individual devices may still disconnect or fail playback.

## GET /v1/speakers

Requires the app token. Returns:

```json
{
  "paused": false,
  "listening_address": "http://0.0.0.0:48490",
  "model_ready": true,
  "speakers": [{
    "speaker": {"id":"speaker-kitchen","name":"Kitchen","tags":["downstairs"]},
    "connected": true,
    "phase": "listening",
    "level": 0.013,
    "audio_frames": 1582,
    "requests": 3,
    "firmware": "muse-luxe/0.1.1",
    "last_error": null
  }]
}
```

Phases: `offline`, `listening`, `transcribing`, `waiting`, `synthesizing`, `speaking`, `muted`, `paused`. Level is normalized RMS, not decibels. Counters are per connection. `model_ready` means the selected file exists; loading/inference failures appear in the speaker's last error. No tokens or conversation text are exposed by this endpoint.

`GET /healthz` is public and returns only the service name and software version.

## Explicit microphone sample

`POST /v1/speakers/{id}/microphone-sample` uses the app bearer token and accepts `{"seconds":15}`, with a supported range of 1 to 15 seconds. It returns a 16 kHz mono signed 16-bit WAV with `Cache-Control: no-store`. Device tokens cannot use this endpoint. A selected speaker must be online, listening and unmuted; invalid duration returns HTTP 400 and offline/busy/interrupted tests return HTTP 409.

Only one test can run on each speaker. During the test, audio is collected in bounded memory for this request and ordinary prompt activation on that speaker is suspended. `/v1/speakers` reports `microphone_test_active`. Samples are discarded on cancellation, mute, pause, disconnect or timeout. A test cannot continue across a replacement connection. The hub never writes the recording to disk; an API caller receiving the WAV controls its own copy.

For local diagnosis, run `.tools/venv/Scripts/python.exe scripts/microphone-test.py --speaker <id>`. The helper collects one sample, reports level statistics and local transcription, then removes its temporary recording. `--play <test.wav>` plays a known phrase through the PC while capturing. `--output <private.wav>` explicitly retains the sample. Normal listening never enables this check automatically.

## Observe normal recognition

`POST /v1/speakers/{id}/recognition-test` uses the app bearer token and accepts `{"seconds":30}`, with a range of 1 to 60 seconds. It waits for one normally detected and transcribed phrase. The JSON response includes `text`, `wake_name`, `configured_wake_names`, `utterance_seconds`, `utterance_rms`, `model_filename` and `error`. It uses `Cache-Control: no-store`. Normal wake-name matching and prompt delivery continue throughout the observation.

Only one observation can be armed per speaker. It expires on timeout and ends on mute, pause or disconnect. The next connection cannot fulfill an older observation. No transcript history is retained, and ordinary status responses expose only the `recognition_test_active` flag. Invalid duration returns HTTP 400; offline, busy, expired or interrupted observations return HTTP 409.

Use `scripts/microphone-test.py --speaker <id> --guide --observe` for an audible cue followed by observation of the real recognition path. Without `--observe`, the helper uses the bounded WAV sample endpoint described above.

## Speaker WebSocket

Connect to `ws://PC:48490/v1/speakers/{speaker_id}/audio` with that speaker's bearer token in the upgrade request. Send this within five seconds:

```json
{"type":"hello","protocol":1,"speaker_id":"speaker-kitchen","firmware":"muse-luxe/0.1.1","sample_rate":16000,"channels":1,"sample_format":"s16le"}
```

The hub returns `{"type":"ready","protocol":1,"frame_samples":320,"volume":65}` and a `state` message. Upload raw signed little-endian 16-bit mono PCM binary messages at 16 kHz, normally 320 samples (640 bytes) every 20 ms. No WAV headers in microphone uploads. Empty, odd-sized, or oversized messages are rejected. The message limit is 6,400 bytes. Do not send prerecorded audio faster than real time except in isolated tests.

Client text messages:

```json
{"type":"muted","value":true}
{"type":"playback_started","request_id":"d553e571-6e1f-4e76-9dd8-9dcab56c10c3"}
{"type":"playback_finished","request_id":"d553e571-6e1f-4e76-9dd8-9dcab56c10c3"}
{"type":"playback_error","request_id":"d553e571-6e1f-4e76-9dd8-9dcab56c10c3","code":"audio_transfer_or_playback_failed"}
{"type":"ping"}
```

Hub text messages include `state`, `cancel`, `pong`, and:

```json
{"type":"play","request_id":"d553e571-6e1f-4e76-9dd8-9dcab56c10c3","audio_url":"http://PC:48490/v1/audio/383eb466-ddd9-40cf-bd64-b8f937663a61","sample_rate":16000,"channels":1,"bytes":64044}
```

Fetch `audio_url` using the speaker token. It contains a standard RIFF/WAV PCM stream. URLs are temporary, bound to the particular speaker connection, and expire after four minutes or earlier on completion/disconnect. Other speaker tokens cannot read them. Playback is limited to three minutes. The device reports started/finished/error; a missing report eventually times out the request. Respond to WebSocket ping frames even while not streaming audio. The hub disconnects peers with no incoming messages for 45 seconds.

For this firmware, public URL and configured device hub URL must refer to the same HTTP origin. HTTPS termination on a reverse proxy is not implemented in the speaker client. The speaker checks that playback URLs belong to its configured hub before sending its token.

## OpenAI-compatible mode

The URL is a complete chat-completions endpoint. The request contains `model`, `max_tokens` (the configured response budget, 1024 by default), `stream:false`, a system message and a user message containing the recognized phrase. `X-Speaker-Id` and `X-Request-Id` carry speaker and request identity. The optional legacy `user` body field is omitted for routers that reject it. The response comes from `choices[0].message.content`. This mode does not stream partial tokens or maintain history. The webhook mode provides the full names/tags contract for custom backends.

Endpoint URL, bearer token, model ID and response budget are local settings. No endpoint or model is bundled. For endpoints that require a stable caller header, expand **Optional caller routing** and set **Hub instance ID** (`router_instance_id`) and **Caller ID header** (`router_header_name`), for example `X-Client-Instance`. Both default to empty. The header must start with `X-`, contain only letters, numbers and hyphens, and cannot replace `X-Speaker-Id` or `X-Request-Id`. A header requires a hub instance ID. Leaving the header blank disables this feature even if an instance ID was previously saved.

The value is `speaker-` followed by the lowercase SHA-256 of `smart-speaker/router/v1`, a NUL byte, the hub instance ID, a NUL byte, and the permanent speaker ID. It is stable across requests, reconnects and restarts, with separate callers for different speakers or hubs. API tokens never enter this calculation. This optional header is sent only in OpenAI-compatible mode and does not provide conversation memory. Response budgets include any reasoning tokens used by the model.

## Buffering and lifecycle

The rolling prebuffer retains 500 ms before detected speech. A default 850 ms of silence ends a phrase. Speech activity uses RMS and a minimum voiced duration, so a short click does not activate a command. A matching name is checked using whole-word boundaries after local transcription. `loudspeakers` does not match `Speaker`.

Each connection has a unique generation. Old transcription, callbacks, or synthesized audio cannot publish to a replacement generation. Pause, exit, or saving settings disconnects devices and clears requests. Speakers reconnect automatically; a paused hub advertises paused state. The device stops uploading microphone frames when muted/paused and discards captured audio during playback plus a 350 ms tail. This is half-duplex conversational behavior, with no echo cancellation or barge-in.

## Per-speaker audio settings, 0.1.1

Saved speaker registrations additionally contain `microphone_gain` (finite number from 0.25 to 8, default 1) and `volume` (integer 0 to 100, default 65). Existing registrations without these fields load with those defaults. Names, tags, identity and tokens are unchanged.

The WebSocket ready message is now `{"type":"ready","protocol":1,"frame_samples":320,"volume":65}`. A device applies the saved level on connection and reports `{"type":"volume","value":65}` after applying it and after physical-button changes. The report is runtime telemetry; it does not overwrite saved settings. Values above 100 are ignored. Other devices may ignore the optional ready field, in which case remote volume is unconfirmed.

Authenticated speaker status includes `input_level` (raw RMS), `level` (gain-adjusted RMS, capped at 1), `microphone_gain`, `configured_volume`, `device_volume` (null until reported), and `queued_utterances` (0 to 2). The microphone sample API still returns raw device PCM; normal recognition and its observer use the speaker's configured gain. The desktop saves audio controls through its normal validated settings workflow.

Local transcription no longer discards newly arriving microphone frames. One owned worker per speaker drains a queue of at most two completed utterances; if it fills, the oldest waiting phrase is discarded. A separate in-progress phrase remains bounded by the configured maximum phrase length. A listening generation and cancellation token prevent queued or in-progress speech from being reused after mute, capture, playback or reconnection. Audio during a pending reply and playback is still ignored. Queue contents stay in memory.

In OpenAI-compatible mode, the system message now appends the PC's current local RFC3339 date/time and UTC offset at dispatch. The webhook request contract stays unchanged.
