use async_trait::async_trait;
use axum::{
    extract::State,
    http::{HeaderMap, StatusCode},
    routing::post,
    Json, Router,
};
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use smart_speaker_hub::{
    audio,
    config::{BackendMode, ConfigStore, Settings, SpeakerConfig},
    engine::Hub,
    protocol::{PromptRequest, ServerMessage, SAMPLE_RATE},
    server::{self, Listener},
    speech::ISpeechEngine,
};
use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::{
    net::{TcpListener, TcpStream},
    sync::mpsc,
    task::JoinHandle,
    time::timeout,
};
use tokio_tungstenite::{
    connect_async,
    tungstenite::{client::IntoClientRequest, http::HeaderValue, Message},
    MaybeTlsStream, WebSocketStream,
};
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

type Socket = WebSocketStream<MaybeTlsStream<TcpStream>>;
struct FakeSpeech {
    transcripts: Mutex<VecDeque<String>>,
    spoken: Mutex<Vec<String>>,
    captured: Mutex<Vec<Vec<i16>>>,
    delay: Duration,
}
impl FakeSpeech {
    fn new(texts: &[&str], delay: Duration) -> Self {
        Self {
            transcripts: Mutex::new(texts.iter().map(|s| (*s).into()).collect()),
            spoken: Mutex::new(vec![]),
            captured: Mutex::new(vec![]),
            delay,
        }
    }
}
#[async_trait]
impl ISpeechEngine for FakeSpeech {
    async fn transcribe(
        &self,
        samples: Vec<i16>,
        _: Settings,
        cancel: CancellationToken,
    ) -> anyhow::Result<String> {
        self.captured.lock().unwrap().push(samples);
        let text = self
            .transcripts
            .lock()
            .unwrap()
            .pop_front()
            .unwrap_or_else(|| "ordinary conversation".into());
        tokio::select! { _ = cancel.cancelled() => anyhow::bail!("Cancelled"), _ = tokio::time::sleep(self.delay) => {} }
        Ok(text)
    }
    async fn synthesize(&self, text: String, _: Settings) -> anyhow::Result<Vec<u8>> {
        self.spoken.lock().unwrap().push(text);
        audio::encode_wav(&vec![4000; 1600])
    }
}
struct Fixture {
    hub: Arc<Hub>,
    listener: Listener,
    backend: JoinHandle<()>,
    requests: mpsc::Receiver<Value>,
    request_headers: mpsc::Receiver<HeaderMap>,
    speech: Arc<FakeSpeech>,
    url: String,
}
impl Fixture {
    async fn new(texts: &[&str], mode: BackendMode, direct: Option<&str>, delay: Duration) -> Self {
        Self::with_gate(texts, mode, direct, delay, None).await
    }
    async fn with_gate(
        texts: &[&str],
        mode: BackendMode,
        direct: Option<&str>,
        delay: Duration,
        gate: Option<Arc<tokio::sync::Notify>>,
    ) -> Self {
        let (sender, requests) = mpsc::channel(16);
        let (header_sender, request_headers) = mpsc::channel(16);
        let direct = direct.map(str::to_owned);
        let backend_listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = backend_listener.local_addr().unwrap();
        let model_mode = mode == BackendMode::OpenAi;
        let router = Router::new().route("/prompt", post(move |State(sender): State<mpsc::Sender<Value>>, headers: HeaderMap, Json(body): Json<Value>| {
            let direct = direct.clone(); let gate = gate.clone(); let header_sender = header_sender.clone();
            async move {
                // Strict compatible endpoints may reject optional legacy fields.
                let rejected = model_mode && body.get("user").is_some();
                header_sender.send(headers).await.unwrap();
                sender.send(body).await.unwrap();
                if rejected {
                    return (StatusCode::BAD_REQUEST, Json(json!({"error":{"message":"Unsupported field: user"}})));
                }
                if let Some(gate) = gate { gate.notified().await; }
                (if direct.is_some() { StatusCode::OK } else { StatusCode::ACCEPTED }, Json(direct.map_or(json!({}), |text| json!({"text":text,"choices":[{"message":{"content":text}}]}))))
            }
        })).with_state(sender);
        let backend = tokio::spawn(async move {
            axum::serve(backend_listener, router).await.unwrap();
        });
        let mut settings = Settings {
            listen_url: "http://127.0.0.1:0".into(),
            outbound_url: format!("http://{address}/prompt"),
            backend_mode: mode,
            model: "fixture-model".into(),
            silence_ms: 300,
            wake_names: vec!["Speaker".into(), "Computer".into()],
            ..Settings::default()
        };
        settings.speakers = vec![
            speaker("kitchen", "Kitchen", &["downstairs"]),
            speaker("office", "Office", &["upstairs"]),
            speaker("lounge", "Lounge", &["downstairs"]),
        ];
        let speech = Arc::new(FakeSpeech::new(texts, delay));
        let hub = Hub::new(settings, speech.clone()).unwrap();
        let listener = server::start(hub.clone()).await.unwrap();
        let url = format!("http://{}", listener.address);
        let mut config = hub.settings();
        config.public_url = url.clone();
        hub.replace_settings(config).await.unwrap();
        Self {
            hub,
            listener,
            backend,
            requests,
            request_headers,
            speech,
            url,
        }
    }
    async fn socket(&self, id: &str) -> Socket {
        let mut request =
            self.url.replace("http://", "ws://").to_owned() + &format!("/v1/speakers/{id}/audio");
        let mut request = std::mem::take(&mut request).into_client_request().unwrap();
        let token = self
            .hub
            .settings()
            .speakers
            .iter()
            .find(|s| s.id == id)
            .unwrap()
            .token
            .clone();
        request.headers_mut().insert(
            "Authorization",
            HeaderValue::from_str(&format!("Bearer {token}")).unwrap(),
        );
        let (mut socket, _) = connect_async(request).await.unwrap();
        socket.send(Message::Text(json!({"type":"hello","protocol":1,"speaker_id":id,"firmware":"test-simulator","sample_rate":SAMPLE_RATE,"channels":1,"sample_format":"s16le"}).to_string().into())).await.unwrap();
        let ServerMessage::Ready { volume, .. } = next(&mut socket).await else {
            panic!("Expected ready");
        };
        assert_eq!(
            volume,
            self.hub
                .settings()
                .speakers
                .iter()
                .find(|s| s.id == id)
                .unwrap()
                .volume
        );
        let _ = next(&mut socket).await;
        socket
    }
    async fn request(&mut self) -> PromptRequest {
        serde_json::from_value(
            timeout(Duration::from_secs(3), self.requests.recv())
                .await
                .unwrap()
                .unwrap(),
        )
        .unwrap()
    }
    async fn post(&self, route: &str, body: Value) -> reqwest::Response {
        reqwest::Client::new()
            .post(format!("{}{route}", self.url))
            .bearer_auth(self.hub.settings().api_token)
            .json(&body)
            .send()
            .await
            .unwrap()
    }
    async fn stop(self) {
        self.listener.stop(&self.hub).await;
        self.backend.abort();
        let _ = self.backend.await;
    }
}
fn speaker(id: &str, name: &str, tags: &[&str]) -> SpeakerConfig {
    SpeakerConfig {
        id: id.into(),
        name: name.into(),
        tags: tags.iter().map(|s| (*s).into()).collect(),
        token: format!("{}-token-{}", id, "x".repeat(48)),
        enabled: true,
        microphone_gain: 1.0,
        volume: 65,
    }
}
async fn next(socket: &mut Socket) -> ServerMessage {
    timeout(Duration::from_secs(4), async {
        loop {
            match socket.next().await.unwrap().unwrap() {
                Message::Text(text) => return serde_json::from_str(&text).unwrap(),
                Message::Ping(p) => socket.send(Message::Pong(p)).await.unwrap(),
                _ => {}
            }
        }
    })
    .await
    .unwrap()
}
async fn play(socket: &mut Socket) -> (Uuid, String) {
    loop {
        if let ServerMessage::Play {
            request_id,
            audio_url,
            sample_rate,
            channels,
            ..
        } = next(socket).await
        {
            assert_eq!(sample_rate, SAMPLE_RATE);
            assert_eq!(channels, 1);
            return (request_id, audio_url);
        }
    }
}
async fn utterance(socket: &mut Socket) {
    for amplitude in std::iter::repeat_n(6000_i16, 22).chain(std::iter::repeat_n(0_i16, 18)) {
        let bytes: Vec<u8> = std::iter::repeat_n(amplitude, 320)
            .flat_map(i16::to_le_bytes)
            .collect();
        socket.send(Message::Binary(bytes.into())).await.unwrap();
    }
}
async fn finished(socket: &mut Socket, id: Uuid) {
    socket
        .send(Message::Text(
            json!({"type":"playback_finished","request_id":id})
                .to_string()
                .into(),
        ))
        .await
        .unwrap();
}

#[tokio::test]
async fn callback_routes_only_to_its_speaker_and_is_idempotent() {
    let mut f = Fixture::new(
        &["Please tell me the weather, Speaker."],
        BackendMode::Webhook,
        None,
        Duration::ZERO,
    )
    .await;
    let mut kitchen = f.socket("kitchen").await;
    let mut office = f.socket("office").await;
    utterance(&mut kitchen).await;
    let request = f.request().await;
    assert_eq!(request.speaker.id, "kitchen");
    assert_eq!(request.speaker.name, "Kitchen");
    assert_eq!(request.speaker.tags, ["downstairs"]);
    assert_eq!(request.conversation_id, "kitchen");
    assert_eq!(request.wake_name, "Speaker");
    let response = json!({"speaker_id":"kitchen","request_id":request.request_id,"text":"The weather is fine."});
    let mut wrong = response.clone();
    wrong["speaker_id"] = json!("office");
    assert_eq!(f.post("/v1/responses", wrong).await.status(), 409);
    assert_eq!(
        f.post("/v1/responses", response.clone()).await.status(),
        202
    );
    assert_eq!(
        f.post("/v1/responses", response.clone()).await.status(),
        200
    );
    let (id, audio_url) = play(&mut kitchen).await;
    assert_eq!(id, request.request_id);
    let client = reqwest::Client::new();
    let settings = f.hub.settings();
    assert_eq!(
        client
            .get(&audio_url)
            .bearer_auth(&settings.api_token)
            .send()
            .await
            .unwrap()
            .status(),
        401
    );
    assert_eq!(
        client
            .get(&audio_url)
            .bearer_auth(&settings.speakers[1].token)
            .send()
            .await
            .unwrap()
            .status(),
        404
    );
    let wav = client
        .get(&audio_url)
        .bearer_auth(&settings.speakers[0].token)
        .send()
        .await
        .unwrap()
        .bytes()
        .await
        .unwrap();
    assert_eq!(&wav[..4], b"RIFF");
    office
        .send(Message::Text(json!({"type":"ping"}).to_string().into()))
        .await
        .unwrap();
    assert!(matches!(next(&mut office).await, ServerMessage::Pong));
    finished(&mut kitchen, id).await;
    assert!(matches!(
        next(&mut kitchen).await,
        ServerMessage::State { .. }
    ));
    assert_eq!(
        f.post("/v1/responses", response.clone()).await.status(),
        200
    );
    let mut conflict = response;
    conflict["text"] = json!("Different answer");
    assert_eq!(f.post("/v1/responses", conflict).await.status(), 409);
    assert_eq!(f.speech.spoken.lock().unwrap().len(), 1);
    assert_eq!(
        client
            .get(audio_url)
            .bearer_auth(&settings.speakers[0].token)
            .send()
            .await
            .unwrap()
            .status(),
        404
    );
    f.stop().await;
}

#[tokio::test]
async fn out_of_order_callbacks_keep_two_conversations_separate() {
    let mut f = Fixture::new(
        &["Speaker, kitchen question", "Speaker, office question"],
        BackendMode::Webhook,
        None,
        Duration::ZERO,
    )
    .await;
    let mut a = f.socket("kitchen").await;
    let mut b = f.socket("office").await;
    utterance(&mut a).await;
    let first = f.request().await;
    utterance(&mut b).await;
    let second = f.request().await;
    for r in [&second, &first] {
        assert_eq!(f.post("/v1/responses", json!({"speaker_id":r.speaker.id,"request_id":r.request_id,"text":format!("Answer for {}",r.speaker.name)})).await.status(), 202);
    }
    assert_eq!(play(&mut b).await.0, second.request_id);
    assert_eq!(play(&mut a).await.0, first.request_id);
    f.stop().await;
}

#[tokio::test]
async fn ordinary_speech_is_local_and_mute_blocks_a_transcription_in_progress() {
    let mut f = Fixture::new(
        &[
            "The loudspeakers are outside",
            "Speaker, do not send this muted request",
        ],
        BackendMode::Webhook,
        None,
        Duration::from_millis(150),
    )
    .await;
    let mut a = f.socket("kitchen").await;
    utterance(&mut a).await;
    tokio::time::sleep(Duration::from_millis(250)).await;
    assert!(f.requests.try_recv().is_err());
    utterance(&mut a).await;
    tokio::time::sleep(Duration::from_millis(25)).await;
    a.send(Message::Text(
        json!({"type":"muted","value":true}).to_string().into(),
    ))
    .await
    .unwrap();
    tokio::time::sleep(Duration::from_millis(250)).await;
    assert!(f.requests.try_recv().is_err());
    assert_eq!(
        f.hub.status().await.speakers[0].phase,
        smart_speaker_hub::protocol::Phase::Muted
    );
    f.stop().await;
}

#[tokio::test]
async fn tags_target_a_group_and_retries_do_not_repeat_playback() {
    let f = Fixture::new(&[], BackendMode::Webhook, None, Duration::ZERO).await;
    let mut a = f.socket("kitchen").await;
    let mut b = f.socket("lounge").await;
    let body = json!({"request_id":Uuid::new_v4(),"tags":["downstairs"],"text":"Dinner is ready."});
    assert_eq!(f.post("/v1/speak", body.clone()).await.status(), 202);
    let pa = play(&mut a).await;
    let pb = play(&mut b).await;
    assert_eq!(pa.0, pb.0);
    assert_eq!(f.post("/v1/speak", body).await.status(), 202);
    assert_eq!(f.speech.spoken.lock().unwrap().len(), 2);
    assert_eq!(
        f.post(
            "/v1/speak",
            json!({"request_id":Uuid::new_v4(),"text":"No implicit broadcast"})
        )
        .await
        .status(),
        409
    );
    assert_eq!(
        f.post(
            "/v1/speak",
            json!({"request_id":Uuid::new_v4(),"speaker_ids":["office"],"text":"Offline"})
        )
        .await
        .status(),
        409
    );
    f.stop().await;
}

#[tokio::test]
async fn reconnect_invalidates_pending_replies_and_bad_audio_closes_session() {
    let mut f = Fixture::new(
        &["Speaker, an old request"],
        BackendMode::Webhook,
        None,
        Duration::ZERO,
    )
    .await;
    let mut old = f.socket("kitchen").await;
    utterance(&mut old).await;
    let request = f.request().await;
    let mut replacement = f.socket("kitchen").await;
    assert_eq!(
        f.post(
            "/v1/responses",
            json!({"request_id":request.request_id,"speaker_id":"kitchen","text":"Stale reply"})
        )
        .await
        .status(),
        409
    );
    replacement
        .send(Message::Binary(vec![1, 2, 3].into()))
        .await
        .unwrap();
    tokio::time::sleep(Duration::from_millis(80)).await;
    assert!(!f.hub.status().await.speakers[0].connected);
    f.stop().await;
}

#[tokio::test]
async fn api_authentication_pause_and_listener_shutdown_are_enforced() {
    let f = Fixture::new(&[], BackendMode::Webhook, None, Duration::ZERO).await;
    let _a = f.socket("kitchen").await;
    let client = reqwest::Client::new();
    assert_eq!(
        client
            .get(format!("{}/v1/speakers", f.url))
            .send()
            .await
            .unwrap()
            .status(),
        401
    );
    assert_eq!(
        client
            .post(format!("{}/v1/speak", f.url))
            .json(&json!({"request_id":Uuid::new_v4(),"text":"unauthorized"}))
            .send()
            .await
            .unwrap()
            .status(),
        401
    );
    let device_token = &f.hub.settings().speakers[0].token;
    assert_eq!(
        client
            .get(format!("{}/v1/speakers", f.url))
            .bearer_auth(device_token)
            .send()
            .await
            .unwrap()
            .status(),
        401
    );
    f.hub.set_paused(true).await;
    assert!(!f.hub.status().await.speakers[0].connected);
    assert_eq!(
        f.post(
            "/v1/speak",
            json!({"request_id":Uuid::new_v4(),"tags":["downstairs"],"text":"Paused"})
        )
        .await
        .status(),
        409
    );
    let address = f.listener.address;
    f.stop().await;
    let bound_again = TcpListener::bind(address).await.unwrap();
    drop(bound_again);
}

#[tokio::test]
async fn direct_and_openai_compatible_replies_play_without_a_callback() {
    for mode in [BackendMode::Webhook, BackendMode::OpenAi] {
        let mut f = Fixture::new(
            &["Speaker, say hello"],
            mode.clone(),
            Some("Hello from the test app."),
            Duration::ZERO,
        )
        .await;
        let mut a = f.socket("kitchen").await;
        utterance(&mut a).await;
        let body = timeout(Duration::from_secs(3), f.requests.recv())
            .await
            .unwrap()
            .unwrap();
        if mode == BackendMode::OpenAi {
            assert_eq!(body["model"], "fixture-model");
            assert_eq!(body["max_tokens"], 1024);
            assert_eq!(body["messages"][1]["content"], "Speaker, say hello");
            let system = body["messages"][0]["content"].as_str().unwrap();
            let clock = system
                .split("PC: ")
                .nth(1)
                .unwrap()
                .split(". Use this clock")
                .next()
                .unwrap();
            let parsed = chrono::DateTime::parse_from_rfc3339(clock).unwrap();
            assert!((chrono::Local::now().timestamp() - parsed.timestamp()).abs() < 10);
        } else {
            assert_eq!(body["speaker"]["id"], "kitchen");
        }
        let headers = f.request_headers.recv().await.unwrap();
        assert!(!headers.contains_key("x-client-instance"));
        let _ = play(&mut a).await;
        assert_eq!(
            f.speech.spoken.lock().unwrap().as_slice(),
            ["Hello from the test app."]
        );
        f.stop().await;
    }
}

#[tokio::test]
async fn router_affinity_survives_reconnect_and_saved_settings_but_separates_callers() {
    let mut f = Fixture::new(
        &["Speaker, say hello"; 4],
        BackendMode::OpenAi,
        Some("Hello from the fixture."),
        Duration::ZERO,
    )
    .await;
    let directory = tempfile::tempdir().unwrap();
    let store = ConfigStore::new(directory.path().join("settings.json"));
    let mut config = f.hub.settings();
    config.router_instance_id = Uuid::new_v4().simple().to_string();
    config.model_max_tokens = 2048;
    config.router_header_name = "x-client-instance".into();
    store.save(&config).unwrap();
    let mut identities = vec![];
    let mut request_ids = std::collections::HashSet::new();
    for (index, id) in ["kitchen", "kitchen", "office", "kitchen"]
        .into_iter()
        .enumerate()
    {
        if index == 3 {
            config.router_instance_id = Uuid::new_v4().simple().to_string();
            store.save(&config).unwrap();
        }
        f.hub.replace_settings(store.load().unwrap()).await.unwrap();
        let mut socket = f.socket(id).await;
        utterance(&mut socket).await;
        let body = timeout(Duration::from_secs(3), f.requests.recv())
            .await
            .unwrap()
            .unwrap();
        let headers = f.request_headers.recv().await.unwrap();
        assert_eq!(body["max_tokens"], 2048);
        assert_eq!(headers["x-speaker-id"], id);
        request_ids.insert(headers["x-request-id"].to_str().unwrap().to_owned());
        let identity = headers["x-client-instance"].to_str().unwrap().to_owned();
        assert!(
            identity.len() <= 200
                && identity
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
        );
        assert!(!identity.contains(&config.api_token));
        identities.push(identity);
        let (request_id, _) = play(&mut socket).await;
        finished(&mut socket, request_id).await;
        loop {
            if matches!(
                next(&mut socket).await,
                ServerMessage::State {
                    state: smart_speaker_hub::protocol::Phase::Listening
                }
            ) {
                break;
            }
        }
        socket.close(None).await.unwrap();
    }
    assert_eq!(identities[0], identities[1]);
    assert_ne!(identities[0], identities[2]);
    assert_ne!(identities[0], identities[3]);
    assert_eq!(request_ids.len(), 4);
    f.stop().await;
}

#[test]
fn config_roundtrip_is_atomic_and_invalid_edits_preserve_saved_settings() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("settings.json");
    let store = ConfigStore::new(path.clone());
    let mut settings = store.load().unwrap();
    settings
        .speakers
        .push(speaker("kitchen", "Kitchen", &["downstairs"]));
    store.save(&settings).unwrap();
    assert_eq!(store.load().unwrap().speakers[0].name, "Kitchen");
    let before = std::fs::read(&path).unwrap();
    settings.speakers.push(settings.speakers[0].clone());
    assert!(store.save(&settings).is_err());
    assert_eq!(std::fs::read(&path).unwrap(), before);
}

#[tokio::test]
async fn an_early_callback_keeps_ownership_when_the_http_body_later_disagrees() {
    let gate = Arc::new(tokio::sync::Notify::new());
    let mut f = Fixture::with_gate(
        &["Speaker, test an early callback"],
        BackendMode::Webhook,
        Some("A later conflicting answer"),
        Duration::ZERO,
        Some(gate.clone()),
    )
    .await;
    let mut a = f.socket("kitchen").await;
    utterance(&mut a).await;
    let prompt = f.request().await;
    let response = json!({"request_id":prompt.request_id,"speaker_id":"kitchen","text":"The accepted callback owns this answer."});
    assert_eq!(
        f.post("/v1/responses", response.clone()).await.status(),
        202
    );
    let (_, audio_url) = play(&mut a).await;
    gate.notify_one();
    tokio::time::sleep(Duration::from_millis(100)).await;
    assert_eq!(
        f.hub.status().await.speakers[0].phase,
        smart_speaker_hub::protocol::Phase::Speaking
    );
    let token = f.hub.settings().speakers[0].token.clone();
    assert_eq!(
        reqwest::Client::new()
            .get(audio_url)
            .bearer_auth(token)
            .send()
            .await
            .unwrap()
            .status(),
        200
    );
    assert_eq!(f.post("/v1/responses", response).await.status(), 200);
    assert_eq!(
        f.speech.spoken.lock().unwrap().as_slice(),
        ["The accepted callback owns this answer."]
    );
    f.stop().await;
}

async fn wait_for_microphone_test(f: &Fixture, active: bool) {
    timeout(Duration::from_secs(3), async {
        loop {
            let status: Value = reqwest::Client::new()
                .get(format!("{}/v1/speakers", f.url))
                .bearer_auth(f.hub.settings().api_token)
                .send()
                .await
                .unwrap()
                .json()
                .await
                .unwrap();
            if status["speakers"][0]["microphone_test_active"] == active {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .unwrap();
}

fn start_microphone_test(f: &Fixture, seconds: u32) -> JoinHandle<reqwest::Response> {
    let url = format!("{}/v1/speakers/kitchen/microphone-sample", f.url);
    let token = f.hub.settings().api_token;
    tokio::spawn(async move {
        reqwest::Client::new()
            .post(url)
            .bearer_auth(token)
            .json(&json!({"seconds":seconds}))
            .send()
            .await
            .unwrap()
    })
}

#[tokio::test]
async fn microphone_sample_is_explicit_authorized_bounded_and_speaker_specific() {
    let mut f = Fixture::new(&[], BackendMode::Webhook, None, Duration::ZERO).await;
    let mut kitchen = f.socket("kitchen").await;
    let mut office = f.socket("office").await;
    let route = "/v1/speakers/kitchen/microphone-sample";
    for token in ["".to_owned(), f.hub.settings().speakers[0].token.clone()] {
        let result = reqwest::Client::new()
            .post(format!("{}{route}", f.url))
            .bearer_auth(token)
            .json(&json!({"seconds":1}))
            .send()
            .await
            .unwrap();
        assert_eq!(result.status(), 401);
    }
    for seconds in [0, 16] {
        assert_eq!(
            f.post(route, json!({"seconds":seconds})).await.status(),
            400
        );
    }
    assert_eq!(
        f.post(
            "/v1/speakers/missing/microphone-sample",
            json!({"seconds":1})
        )
        .await
        .status(),
        409
    );
    let capture = start_microphone_test(&f, 1);
    wait_for_microphone_test(&f, true).await;
    assert_eq!(f.post(route, json!({"seconds":1})).await.status(), 409);
    assert_eq!(f.post("/v1/speak",json!({"request_id":Uuid::new_v4(),"speaker_ids":["kitchen"],"text":"Fixture announcement"})).await.status(),409);
    for _ in 0..50 {
        for (socket, amplitude) in [(&mut kitchen, 2000_i16), (&mut office, 300_i16)] {
            let bytes: Vec<u8> = std::iter::repeat_n(amplitude, 320)
                .flat_map(i16::to_le_bytes)
                .collect();
            socket.send(Message::Binary(bytes.into())).await.unwrap();
        }
        tokio::task::yield_now().await;
    }
    let response = timeout(Duration::from_secs(3), capture)
        .await
        .unwrap()
        .unwrap();
    assert_eq!(response.status(), 200);
    assert_eq!(response.headers()["cache-control"], "no-store");
    assert_eq!(response.headers()["content-type"], "audio/wav");
    let bytes = response.bytes().await.unwrap();
    let reader = hound::WavReader::new(std::io::Cursor::new(bytes)).unwrap();
    assert_eq!(reader.spec().sample_rate, 16000);
    assert_eq!(reader.spec().channels, 1);
    let samples = reader
        .into_samples::<i16>()
        .collect::<Result<Vec<_>, _>>()
        .unwrap();
    assert_eq!(samples, vec![2000; 16000]);
    wait_for_microphone_test(&f, false).await;
    assert!(f.requests.try_recv().is_err());
    assert!(f.speech.spoken.lock().unwrap().is_empty());
    f.stop().await;
}

#[tokio::test]
async fn microphone_test_cancels_on_mute_disconnect_pause_and_missing_frames() {
    let f = Fixture::new(&[], BackendMode::Webhook, None, Duration::ZERO).await;
    let mut socket = f.socket("kitchen").await;
    let capture = start_microphone_test(&f, 1);
    wait_for_microphone_test(&f, true).await;
    socket
        .send(Message::Text(
            json!({"type":"muted","value":true}).to_string().into(),
        ))
        .await
        .unwrap();
    assert_eq!(
        timeout(Duration::from_secs(2), capture)
            .await
            .unwrap()
            .unwrap()
            .status(),
        409
    );
    wait_for_microphone_test(&f, false).await;
    socket.close(None).await.unwrap();
    let mut socket = f.socket("kitchen").await;
    let capture = start_microphone_test(&f, 1);
    wait_for_microphone_test(&f, true).await;
    socket.close(None).await.unwrap();
    let _new_socket = f.socket("kitchen").await;
    assert_eq!(
        timeout(Duration::from_secs(2), capture)
            .await
            .unwrap()
            .unwrap()
            .status(),
        409
    );
    wait_for_microphone_test(&f, false).await;
    let capture = start_microphone_test(&f, 1);
    wait_for_microphone_test(&f, true).await;
    let response = timeout(Duration::from_secs(6), capture)
        .await
        .unwrap()
        .unwrap();
    assert_eq!(response.status(), 409);
    assert!(response.text().await.unwrap().contains("timed out"));
    wait_for_microphone_test(&f, false).await;
    let capture = start_microphone_test(&f, 1);
    wait_for_microphone_test(&f, true).await;
    f.hub.set_paused(true).await;
    assert_eq!(
        timeout(Duration::from_secs(2), capture)
            .await
            .unwrap()
            .unwrap()
            .status(),
        409
    );
    f.stop().await;
}

async fn wait_for_recognition_test(f: &Fixture, active: bool) {
    timeout(Duration::from_secs(3), async {
        loop {
            let status: Value = reqwest::Client::new()
                .get(format!("{}/v1/speakers", f.url))
                .bearer_auth(f.hub.settings().api_token)
                .send()
                .await
                .unwrap()
                .json()
                .await
                .unwrap();
            if status["speakers"][0]["recognition_test_active"] == active {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .unwrap();
}

fn start_recognition_test(f: &Fixture, seconds: u32) -> JoinHandle<reqwest::Response> {
    let url = format!("{}/v1/speakers/kitchen/recognition-test", f.url);
    let token = f.hub.settings().api_token;
    tokio::spawn(async move {
        reqwest::Client::new()
            .post(url)
            .bearer_auth(token)
            .json(&json!({"seconds":seconds}))
            .send()
            .await
            .unwrap()
    })
}

#[tokio::test]
async fn explicit_recognition_observation_preserves_normal_routing_and_expires() {
    let mut f = Fixture::new(
        &["Ordinary conversation.", "Computer, say hello"],
        BackendMode::Webhook,
        Some("Hello from the fixture."),
        Duration::ZERO,
    )
    .await;
    let mut socket = f.socket("kitchen").await;
    let route = "/v1/speakers/kitchen/recognition-test";
    let response = reqwest::Client::new()
        .post(format!("{}{route}", f.url))
        .bearer_auth(&f.hub.settings().speakers[0].token)
        .json(&json!({"seconds":1}))
        .send()
        .await
        .unwrap();
    assert_eq!(response.status(), 401);
    assert_eq!(f.post(route, json!({"seconds":61})).await.status(), 400);
    for expected_name in [None, Some("Computer")] {
        let probe = start_recognition_test(&f, 3);
        wait_for_recognition_test(&f, true).await;
        assert_eq!(f.post(route, json!({"seconds":1})).await.status(), 409);
        utterance(&mut socket).await;
        let response = probe.await.unwrap();
        assert_eq!(response.status(), 200);
        assert_eq!(response.headers()["cache-control"], "no-store");
        let result: Value = response.json().await.unwrap();
        assert_eq!(result["wake_name"].as_str(), expected_name);
        assert_eq!(
            result["configured_wake_names"],
            json!(["Speaker", "Computer"])
        );
        assert!(result["utterance_seconds"].as_f64().unwrap() > 0.5);
        wait_for_recognition_test(&f, false).await;
        if expected_name.is_some() {
            assert_eq!(f.request().await.text, "Computer, say hello");
            let (request_id, _) = play(&mut socket).await;
            finished(&mut socket, request_id).await;
        } else {
            assert!(f.requests.try_recv().is_err());
        }
        loop {
            if matches!(
                next(&mut socket).await,
                ServerMessage::State {
                    state: smart_speaker_hub::protocol::Phase::Listening
                }
            ) {
                break;
            }
        }
    }
    let probe = start_recognition_test(&f, 1);
    wait_for_recognition_test(&f, true).await;
    assert_eq!(
        timeout(Duration::from_secs(3), probe)
            .await
            .unwrap()
            .unwrap()
            .status(),
        409
    );
    wait_for_recognition_test(&f, false).await;
    let probe = start_recognition_test(&f, 3);
    wait_for_recognition_test(&f, true).await;
    f.hub.set_paused(true).await;
    assert_eq!(
        timeout(Duration::from_secs(2), probe)
            .await
            .unwrap()
            .unwrap()
            .status(),
        409
    );
    f.stop().await;
}

#[tokio::test]
async fn speech_arriving_during_local_recognition_is_not_lost() {
    let mut f = Fixture::new(
        &["ordinary conversation", "Speaker, the second phrase"],
        BackendMode::Webhook,
        Some("Second phrase received."),
        Duration::from_millis(350),
    )
    .await;
    let mut socket = f.socket("kitchen").await;
    utterance(&mut socket).await;
    timeout(Duration::from_secs(2), async {
        while f.hub.status().await.speakers[0].phase
            != smart_speaker_hub::protocol::Phase::Transcribing
        {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .unwrap();
    utterance(&mut socket).await;
    let result = timeout(Duration::from_secs(2), f.requests.recv()).await;
    let delivered = result.ok().flatten();
    f.stop().await;
    assert_eq!(
        delivered
            .as_ref()
            .and_then(|v| v.get("text"))
            .and_then(Value::as_str),
        Some("Speaker, the second phrase"),
        "A wake request spoken during earlier local transcription must remain buffered"
    );
}

async fn wait_phase(f: &Fixture, phase: smart_speaker_hub::protocol::Phase) {
    timeout(Duration::from_secs(2), async {
        while f.hub.status().await.speakers[0].phase != phase {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .unwrap();
}

#[tokio::test]
async fn a_reply_discards_speech_queued_before_that_reply() {
    let mut f = Fixture::new(
        &["Speaker, first request", "Speaker, stale queued request"],
        BackendMode::Webhook,
        Some("First reply."),
        Duration::from_millis(250),
    )
    .await;
    let mut socket = f.socket("kitchen").await;
    utterance(&mut socket).await;
    wait_phase(&f, smart_speaker_hub::protocol::Phase::Transcribing).await;
    utterance(&mut socket).await;
    let _ = f.request().await;
    let (id, _) = play(&mut socket).await;
    finished(&mut socket, id).await;
    wait_phase(&f, smart_speaker_hub::protocol::Phase::Listening).await;
    tokio::time::sleep(Duration::from_millis(400)).await;
    let extra = f.requests.try_recv().ok();
    let transcribed = f.speech.captured.lock().unwrap().len();
    f.stop().await;
    assert!(extra.is_none());
    assert_eq!(transcribed, 1);
}

#[tokio::test]
async fn mute_cancels_current_and_queued_recognition_even_after_unmute() {
    let mut f = Fixture::new(
        &["Speaker, cancelled current", "Speaker, cancelled queued"],
        BackendMode::Webhook,
        None,
        Duration::from_millis(300),
    )
    .await;
    let mut socket = f.socket("kitchen").await;
    utterance(&mut socket).await;
    wait_phase(&f, smart_speaker_hub::protocol::Phase::Transcribing).await;
    utterance(&mut socket).await;
    socket
        .send(Message::Text(
            json!({"type":"muted","value":true}).to_string().into(),
        ))
        .await
        .unwrap();
    wait_phase(&f, smart_speaker_hub::protocol::Phase::Muted).await;
    socket
        .send(Message::Text(
            json!({"type":"muted","value":false}).to_string().into(),
        ))
        .await
        .unwrap();
    wait_phase(&f, smart_speaker_hub::protocol::Phase::Listening).await;
    tokio::time::sleep(Duration::from_millis(450)).await;
    assert!(f.requests.try_recv().is_err());
    assert_eq!(f.speech.captured.lock().unwrap().len(), 1);
    *f.speech.transcripts.lock().unwrap() = VecDeque::from(["Speaker, a fresh request".to_owned()]);
    utterance(&mut socket).await;
    let request = f.request().await;
    f.stop().await;
    assert_eq!(request.text, "Speaker, a fresh request");
}

#[tokio::test]
async fn recognition_queue_has_a_per_speaker_bound() {
    let f = Fixture::new(&[], BackendMode::Webhook, None, Duration::from_millis(600)).await;
    let mut socket = f.socket("kitchen").await;
    utterance(&mut socket).await;
    wait_phase(&f, smart_speaker_hub::protocol::Phase::Transcribing).await;
    for _ in 0..5 {
        utterance(&mut socket).await;
    }
    timeout(Duration::from_secs(1), async {
        while f.hub.status().await.speakers[0].audio_frames < 240 {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .unwrap();
    let queued = f.hub.status().await.speakers[0].queued_utterances;
    f.stop().await;
    assert_eq!(queued, 2);
}

#[tokio::test]
async fn each_speaker_keeps_its_gain_volume_and_meter() {
    let f = Fixture::new(&[], BackendMode::Webhook, None, Duration::ZERO).await;
    let mut settings = f.hub.settings();
    settings.speakers[0].microphone_gain = 4.0;
    settings.speakers[0].volume = 80;
    settings.speakers[1].microphone_gain = 0.5;
    settings.speakers[1].volume = 20;
    let temporary = tempfile::tempdir().unwrap();
    let store = ConfigStore::new(temporary.path().join("settings.json"));
    store.save(&settings).unwrap();
    f.hub.replace_settings(store.load().unwrap()).await.unwrap();
    let mut kitchen = f.socket("kitchen").await;
    let mut office = f.socket("office").await;
    kitchen
        .send(Message::Text(
            json!({"type":"volume","value":80}).to_string().into(),
        ))
        .await
        .unwrap();
    office
        .send(Message::Text(
            json!({"type":"volume","value":20}).to_string().into(),
        ))
        .await
        .unwrap();
    utterance(&mut kitchen).await;
    timeout(Duration::from_secs(1), async {
        while f.speech.captured.lock().unwrap().is_empty() {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .unwrap();
    utterance(&mut office).await;
    timeout(Duration::from_secs(1), async {
        while f.speech.captured.lock().unwrap().len() < 2 {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .unwrap();
    let peaks: Vec<i16> = f
        .speech
        .captured
        .lock()
        .unwrap()
        .iter()
        .map(|samples| *samples.iter().max().unwrap())
        .collect();
    let bytes: Vec<u8> = std::iter::repeat_n(100_i16, 320)
        .flat_map(i16::to_le_bytes)
        .collect();
    kitchen
        .send(Message::Binary(bytes.clone().into()))
        .await
        .unwrap();
    office.send(Message::Binary(bytes.into())).await.unwrap();
    tokio::time::sleep(Duration::from_millis(30)).await;
    let status = f.hub.status().await;
    f.stop().await;
    assert_eq!(peaks, [24000, 3000]);
    assert_eq!(status.speakers[0].device_volume, Some(80));
    assert_eq!(status.speakers[1].device_volume, Some(20));
    assert!((status.speakers[0].level / status.speakers[1].level - 8.0).abs() < 0.01);
    assert_eq!(
        status.speakers[0].input_level,
        status.speakers[1].input_level
    );
}

#[test]
fn old_speaker_settings_get_defaults_and_invalid_audio_controls_are_rejected() {
    let mut settings = Settings::default();
    settings.speakers.push(speaker("fixture", "Fixture", &[]));
    let mut legacy = serde_json::to_value(&settings).unwrap();
    legacy["speakers"][0]
        .as_object_mut()
        .unwrap()
        .remove("microphone_gain");
    legacy["speakers"][0]
        .as_object_mut()
        .unwrap()
        .remove("volume");
    let loaded: Settings = serde_json::from_value(legacy).unwrap();
    assert_eq!(loaded.speakers[0].microphone_gain, 1.0);
    assert_eq!(loaded.speakers[0].volume, 65);
    for invalid in [0.0, 8.25, f32::NAN, f32::INFINITY] {
        settings.speakers[0].microphone_gain = invalid;
        assert!(settings.validate().is_err());
    }
    settings.speakers[0].microphone_gain = 1.0;
    settings.speakers[0].volume = 101;
    assert!(settings.validate().is_err());
    let mut samples = [10000, -10000, 0, 100];
    audio::apply_gain(&mut samples, 4.0);
    assert_eq!(samples, [32767, -32768, 0, 400]);
}

#[test]
fn clean_settings_have_no_destination_or_preconfigured_device() {
    let first = Settings::default();
    let second = Settings::default();
    first.validate().unwrap();
    assert!(first.outbound_url.is_empty());
    assert!(first.outbound_token.is_empty());
    assert!(first.model.is_empty());
    assert!(first.router_instance_id.is_empty());
    assert!(first.router_header_name.is_empty());
    assert!(first.speakers.is_empty());
    assert_eq!(first.wake_names, ["Speaker"]);
    assert_ne!(first.api_token, second.api_token);
}

#[test]
fn caller_header_validation_preserves_existing_local_settings() {
    let mut settings = Settings {
        router_instance_id: "local-hub".into(),
        ..Settings::default()
    };
    // Older configurations with only an instance ID still load, with routing disabled.
    let mut legacy = serde_json::to_value(&settings).unwrap();
    legacy.as_object_mut().unwrap().remove("router_header_name");
    let restored: Settings = serde_json::from_value(legacy).unwrap();
    restored.validate().unwrap();
    assert!(restored.router_header_name.is_empty());
    for name in [
        "Authorization",
        "Host",
        "X-Speaker-Id",
        "X-Request-Id",
        "X-Bad\r\nHeader",
        "X-",
        "X_With_Underscore",
    ] {
        settings.router_header_name = name.into();
        assert!(settings.validate().is_err(), "accepted invalid header");
    }
    settings.router_header_name = "X-Client-Instance".into();
    settings.validate().unwrap();
    settings.router_instance_id.clear();
    assert!(settings.validate().is_err());
}

#[tokio::test]
async fn caller_header_is_opt_in_and_never_sent_to_webhooks() {
    for (mode, header) in [
        (BackendMode::OpenAi, ""),
        (BackendMode::Webhook, "X-Client-Instance"),
    ] {
        let mut f = Fixture::new(
            &["Speaker, test routing"],
            mode,
            Some("Test complete."),
            Duration::ZERO,
        )
        .await;
        let mut config = f.hub.settings();
        config.router_instance_id = "local-hub".into();
        config.router_header_name = header.into();
        f.hub.replace_settings(config).await.unwrap();
        let mut socket = f.socket("kitchen").await;
        utterance(&mut socket).await;
        timeout(Duration::from_secs(3), f.requests.recv())
            .await
            .unwrap()
            .unwrap();
        let headers = f.request_headers.recv().await.unwrap();
        assert!(!headers.contains_key("x-client-instance"));
        let _ = play(&mut socket).await;
        f.stop().await;
    }
}
