use crate::{
    audio,
    config::{BackendMode, Settings},
    protocol::*,
    speech::ISpeechEngine,
};
use anyhow::{bail, Context, Result};
use serde_json::json;
use sha2::{Digest, Sha256};
use std::{
    collections::{HashMap, VecDeque},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, RwLock,
    },
    time::{Duration, Instant},
};
use tokio::sync::{mpsc, oneshot, Mutex, Semaphore};
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

type RequestKey = (String, Uuid);
struct RecognitionObserver {
    id: Uuid,
    expires: Instant,
    sender: oneshot::Sender<RecognitionTestResult>,
}
struct MicrophoneCapture {
    id: Uuid,
    sender: mpsc::Sender<Vec<i16>>,
    cancel: CancellationToken,
}
struct CancelOnDrop(CancellationToken);
impl Drop for CancelOnDrop {
    fn drop(&mut self) {
        self.0.cancel();
    }
}
struct Session {
    generation: Uuid,
    sender: mpsc::Sender<ServerMessage>,
    cancel: CancellationToken,
    phase: Phase,
    muted: bool,
    level: f32,
    input_level: f32,
    microphone_gain: f32,
    device_volume: Option<u8>,
    frames: u64,
    requests: u64,
    firmware: String,
    last_error: Option<String>,
    active_request: Option<Uuid>,
    capture: Option<MicrophoneCapture>,
    observer: Option<RecognitionObserver>,
    utterance: audio::UtteranceBuffer,
    queued_utterances: VecDeque<Vec<i16>>,
    recognition_active: bool,
    listening_epoch: Uuid,
    listening_cancel: CancellationToken,
}
impl Session {
    fn reset_listening(&mut self) {
        self.listening_cancel.cancel();
        self.listening_cancel = self.cancel.child_token();
        self.listening_epoch = Uuid::new_v4();
        self.recognition_active = false;
        self.utterance.reset();
        self.queued_utterances.clear();
    }
}
struct Pending {
    generation: Uuid,
    deadline: Instant,
    fingerprint: Option<[u8; 32]>,
}
struct Receipt {
    fingerprint: [u8; 32],
    expires: Instant,
}
struct AudioAsset {
    speaker_id: String,
    generation: Uuid,
    request_id: Uuid,
    expires: Instant,
    bytes: Arc<Vec<u8>>,
}
#[derive(Default)]
struct Inner {
    sessions: HashMap<String, Session>,
    pending: HashMap<RequestKey, Pending>,
    receipts: HashMap<RequestKey, Receipt>,
    audio: HashMap<Uuid, AudioAsset>,
    listening_address: Option<String>,
}

pub struct Hub {
    settings: RwLock<Settings>,
    inner: Mutex<Inner>,
    speech: Arc<dyn ISpeechEngine>,
    workers: Arc<Semaphore>,
    paused: AtomicBool,
    http: reqwest::Client,
}

impl Hub {
    pub fn new(settings: Settings, speech: Arc<dyn ISpeechEngine>) -> Result<Arc<Self>> {
        settings.validate()?;
        let http = reqwest::Client::builder()
            .redirect(reqwest::redirect::Policy::none())
            .connect_timeout(Duration::from_secs(10))
            .build()?;
        Ok(Arc::new(Self {
            settings: RwLock::new(settings),
            inner: Mutex::new(Inner::default()),
            speech,
            workers: Arc::new(Semaphore::new(2)),
            paused: AtomicBool::new(false),
            http,
        }))
    }
    pub fn settings(&self) -> Settings {
        self.settings
            .read()
            .unwrap_or_else(|p| p.into_inner())
            .clone()
    }
    pub fn paused(&self) -> bool {
        self.paused.load(Ordering::Relaxed)
    }
    pub async fn replace_settings(&self, settings: Settings) -> Result<()> {
        settings.validate()?;
        *self.settings.write().unwrap_or_else(|p| p.into_inner()) = settings;
        self.disconnect_all().await;
        Ok(())
    }
    pub async fn disconnect_all(&self) {
        let mut inner = self.inner.lock().await;
        for session in inner.sessions.values() {
            session.cancel.cancel();
        }
        inner.sessions.clear();
        inner.pending.clear();
        inner.audio.clear();
    }
    pub async fn set_listener_address(&self, address: Option<String>) {
        self.inner.lock().await.listening_address = address;
    }
    pub async fn set_paused(&self, paused: bool) {
        self.paused.store(paused, Ordering::Relaxed);
        self.disconnect_all().await;
    }
    pub async fn status(&self) -> HubStatus {
        let settings = self.settings();
        let inner = self.inner.lock().await;
        HubStatus {
            paused: self.paused(),
            listening_address: inner.listening_address.clone(),
            model_ready: !settings.whisper_model.is_empty()
                && std::path::Path::new(&settings.whisper_model).is_file(),
            speakers: settings
                .speakers
                .iter()
                .map(|s| {
                    let live = inner.sessions.get(&s.id);
                    SpeakerStatus {
                        speaker: SpeakerIdentity {
                            id: s.id.clone(),
                            name: s.name.clone(),
                            tags: s.tags.clone(),
                        },
                        connected: live.is_some(),
                        phase: live.map_or(Phase::Offline, |l| l.phase),
                        level: live.map_or(0.0, |l| l.level),
                        input_level: live.map_or(0.0, |l| l.input_level),
                        microphone_gain: s.microphone_gain,
                        configured_volume: s.volume,
                        device_volume: live.and_then(|l| l.device_volume),
                        queued_utterances: live.map_or(0, |l| l.queued_utterances.len()),
                        audio_frames: live.map_or(0, |l| l.frames),
                        requests: live.map_or(0, |l| l.requests),
                        microphone_test_active: live.is_some_and(|l| l.capture.is_some()),
                        recognition_test_active: live.is_some_and(|l| l.observer.is_some()),
                        firmware: live.map_or(String::new(), |l| l.firmware.clone()),
                        last_error: live.and_then(|l| l.last_error.clone()),
                    }
                })
                .collect(),
        }
    }
    pub async fn connect(
        &self,
        id: &str,
        firmware: String,
    ) -> Result<(Uuid, mpsc::Receiver<ServerMessage>, CancellationToken)> {
        if !self
            .settings()
            .speakers
            .iter()
            .any(|s| s.id == id && s.enabled)
        {
            bail!("Speaker is not registered or enabled");
        }
        let registration = self
            .settings()
            .speakers
            .into_iter()
            .find(|s| s.id == id)
            .context("Speaker registration changed")?;
        let (sender, receiver) = mpsc::channel(16);
        let generation = Uuid::new_v4();
        let cancel = CancellationToken::new();
        let phase = if self.paused() {
            Phase::Paused
        } else {
            Phase::Listening
        };
        let mut inner = self.inner.lock().await;
        if let Some(old) = inner.sessions.remove(id) {
            old.cancel.cancel();
        }
        inner.pending.retain(|(speaker, _), _| speaker != id);
        inner.audio.retain(|_, asset| asset.speaker_id != id);
        sender.try_send(ServerMessage::Ready {
            protocol: PROTOCOL_VERSION,
            frame_samples: FRAME_SAMPLES,
            volume: registration.volume,
        })?;
        sender.try_send(ServerMessage::State { state: phase })?;
        inner.sessions.insert(
            id.to_owned(),
            Session {
                generation,
                sender,
                cancel: cancel.clone(),
                phase,
                muted: false,
                level: 0.0,
                input_level: 0.0,
                microphone_gain: registration.microphone_gain,
                device_volume: None,
                frames: 0,
                requests: 0,
                firmware,
                last_error: None,
                active_request: None,
                capture: None,
                observer: None,
                utterance: audio::UtteranceBuffer::new(&self.settings()),
                queued_utterances: VecDeque::new(),
                recognition_active: false,
                listening_epoch: Uuid::new_v4(),
                listening_cancel: cancel.child_token(),
            },
        );
        Ok((generation, receiver, cancel))
    }
    pub async fn disconnect(&self, id: &str, generation: Uuid) {
        let mut inner = self.inner.lock().await;
        if inner
            .sessions
            .get(id)
            .is_some_and(|s| s.generation == generation)
        {
            if let Some(session) = inner.sessions.remove(id) {
                session.cancel.cancel();
            }
            inner
                .pending
                .retain(|(speaker, _), p| speaker != id || p.generation != generation);
            inner
                .audio
                .retain(|_, a| a.speaker_id != id || a.generation != generation);
        }
    }
    pub async fn audio_frame(self: &Arc<Self>, id: &str, generation: Uuid, samples: &[i16]) {
        let listening_epoch = {
            let mut inner = self.inner.lock().await;
            let Some(session) = inner
                .sessions
                .get_mut(id)
                .filter(|s| s.generation == generation)
            else {
                return;
            };
            session.input_level = audio::level(samples);
            session.level = (session.input_level * session.microphone_gain).min(1.0);
            session.frames += 1;
            if let Some(capture) = &session.capture {
                if self.paused()
                    || session.muted
                    || capture.sender.try_send(samples.to_vec()).is_err()
                {
                    capture.cancel.cancel();
                }
                return;
            }
            if self.paused()
                || session.muted
                || !matches!(session.phase, Phase::Listening | Phase::Transcribing)
            {
                session.utterance.reset();
                return;
            }
            // Detect phrase boundaries from the raw input so gain does not turn room noise into speech.
            if let Some(mut utterance) = session.utterance.push(samples) {
                audio::apply_gain(&mut utterance, session.microphone_gain);
                // Retain recent speech while recognition runs, with at most two queued phrases.
                if session.queued_utterances.len() == 2 {
                    session.queued_utterances.pop_front();
                }
                session.queued_utterances.push_back(utterance);
            }
            if session.recognition_active || session.queued_utterances.is_empty() {
                return;
            }
            session.recognition_active = true;
            session.listening_epoch
        };
        let hub = self.clone();
        let id = id.to_owned();
        tokio::spawn(async move {
            hub.recognize_queued(id, generation, listening_epoch).await;
        });
    }
    async fn recognize_queued(
        self: Arc<Self>,
        id: String,
        generation: Uuid,
        listening_epoch: Uuid,
    ) {
        loop {
            let samples =
                {
                    let mut inner = self.inner.lock().await;
                    let Some(session) = inner.sessions.get_mut(&id).filter(|s| {
                        s.generation == generation && s.listening_epoch == listening_epoch
                    }) else {
                        return;
                    };
                    if self.paused()
                        || session.muted
                        || session.phase != Phase::Listening
                        || session.capture.is_some()
                    {
                        session.recognition_active = false;
                        return;
                    }
                    let Some(samples) = session.queued_utterances.pop_front() else {
                        session.recognition_active = false;
                        return;
                    };
                    samples
                };
            self.clone()
                .process_utterance(id.clone(), generation, listening_epoch, samples)
                .await;
        }
    }
    async fn recognition_phase(
        &self,
        id: &str,
        generation: Uuid,
        listening_epoch: Uuid,
        phase: Phase,
        error: Option<String>,
    ) {
        let mut inner = self.inner.lock().await;
        if let Some(session) = inner.sessions.get_mut(id).filter(|s| {
            s.generation == generation
                && s.listening_epoch == listening_epoch
                && !s.muted
                && !self.paused()
        }) {
            session.phase = phase;
            session.last_error = error;
            let _ = session
                .sender
                .try_send(ServerMessage::State { state: phase });
        }
    }
    pub async fn observe_recognition(
        &self,
        id: String,
        seconds: u32,
    ) -> Result<RecognitionTestResult> {
        if !(1..=60).contains(&seconds) {
            bail!("Choose between 1 and 60 seconds");
        }
        let (sender, receiver) = oneshot::channel();
        let observer_id = Uuid::new_v4();
        let generation = {
            let mut inner = self.inner.lock().await;
            let session = inner.sessions.get_mut(&id).context("Speaker is offline")?;
            if self.paused() || session.muted || session.observer.is_some() {
                bail!("Speaker is muted, paused, or already being observed");
            }
            session.observer = Some(RecognitionObserver {
                id: observer_id,
                expires: Instant::now() + Duration::from_secs(seconds as u64),
                sender,
            });
            session.generation
        };
        let result = tokio::time::timeout(Duration::from_secs(seconds as u64), receiver).await;
        let mut inner = self.inner.lock().await;
        if let Some(session) = inner
            .sessions
            .get_mut(&id)
            .filter(|s| s.generation == generation)
        {
            if session
                .observer
                .as_ref()
                .is_some_and(|o| o.id == observer_id)
            {
                session.observer = None;
            }
        }
        result
            .context("No phrase was recognized before the test timed out")?
            .context("Recognition test ended or the speaker disconnected")
    }
    /// Explicit app-authorized sample. Normal operation retains no microphone recording.
    pub async fn capture_microphone(self: &Arc<Self>, id: String, seconds: u32) -> Result<Vec<u8>> {
        if !(1..=15).contains(&seconds) {
            bail!("Choose between 1 and 15 seconds");
        }
        let (sender, mut receiver) = mpsc::channel::<Vec<i16>>(64);
        let capture_id = Uuid::new_v4();
        let (generation, cancel) = {
            let mut inner = self.inner.lock().await;
            let session = inner.sessions.get_mut(&id).context("Speaker is offline")?;
            if self.paused()
                || session.muted
                || session.phase != Phase::Listening
                || session.capture.is_some()
            {
                bail!("Speaker is busy, muted, paused, or already being tested");
            }
            session.reset_listening();
            let cancel = session.cancel.child_token();
            session.capture = Some(MicrophoneCapture {
                id: capture_id,
                sender,
                cancel: cancel.clone(),
            });
            (session.generation, cancel)
        };
        // HTTP cancellation stops the owned worker; cleanup still runs if this future is dropped.
        let _guard = CancelOnDrop(cancel.clone());
        let hub = self.clone();
        tokio::spawn(async move {
            let limit = seconds as usize * SAMPLE_RATE as usize;
            let mut samples = Vec::with_capacity(limit);
            let deadline = tokio::time::sleep(Duration::from_secs(seconds as u64 + 3));
            tokio::pin!(deadline);
            let result = loop {
                tokio::select! {
                    biased;
                    _ = cancel.cancelled() => break Err(anyhow::anyhow!("Microphone test cancelled or interrupted")),
                    _ = &mut deadline => break Err(anyhow::anyhow!("Microphone audio timed out")),
                    frame = receiver.recv() => {
                        let Some(frame) = frame else { break Err(anyhow::anyhow!("Speaker disconnected during microphone test")); };
                        let count = frame.len().min(limit - samples.len());
                        samples.extend_from_slice(&frame[..count]);
                        if samples.len() == limit { break audio::encode_wav(&samples); }
                    }
                }
            };
            let mut inner = hub.inner.lock().await;
            if let Some(session) = inner.sessions.get_mut(&id).filter(|s| s.generation == generation) {
                if session.capture.as_ref().is_some_and(|c| c.id == capture_id) {
                    session.capture = None;
                }
            }
            result
        }).await.context("Microphone test worker stopped")?
    }
    async fn phase(&self, id: &str, generation: Uuid, phase: Phase, error: Option<String>) {
        let mut inner = self.inner.lock().await;
        if let Some(session) = inner
            .sessions
            .get_mut(id)
            .filter(|s| s.generation == generation)
        {
            session.phase = if self.paused() {
                Phase::Paused
            } else if session.muted {
                Phase::Muted
            } else {
                phase
            };
            session.last_error = error;
            let _ = session.sender.try_send(ServerMessage::State {
                state: session.phase,
            });
        }
    }
    pub async fn client_event(&self, id: &str, generation: Uuid, event: ClientMessage) {
        match event {
            ClientMessage::Muted { value } => {
                let mut inner = self.inner.lock().await;
                if let Some(session) = inner
                    .sessions
                    .get_mut(id)
                    .filter(|s| s.generation == generation)
                {
                    if session.muted != value {
                        session.reset_listening();
                    }
                    session.muted = value;
                    if value {
                        session.observer = None;
                    }
                    if value {
                        if let Some(capture) = &session.capture {
                            capture.cancel.cancel();
                        }
                    }
                    if session.active_request.is_none() {
                        session.phase = if self.paused() {
                            Phase::Paused
                        } else if value {
                            Phase::Muted
                        } else {
                            Phase::Listening
                        };
                    }
                }
            }
            ClientMessage::PlaybackStarted { request_id } => {
                let mut inner = self.inner.lock().await;
                if let Some(session) = inner
                    .sessions
                    .get_mut(id)
                    .filter(|s| s.generation == generation && s.active_request == Some(request_id))
                {
                    session.phase = Phase::Speaking;
                }
            }
            ClientMessage::PlaybackFinished { request_id } => {
                self.finish(id, generation, request_id, None).await
            }
            ClientMessage::PlaybackError {
                request_id,
                code: _,
            } => {
                self.finish(
                    id,
                    generation,
                    request_id,
                    Some(
                        "Speaker could not play the response; check its connection and audio setup"
                            .into(),
                    ),
                )
                .await
            }
            ClientMessage::Volume { value } => {
                if value <= 100 {
                    if let Some(session) = self
                        .inner
                        .lock()
                        .await
                        .sessions
                        .get_mut(id)
                        .filter(|s| s.generation == generation)
                    {
                        session.device_volume = Some(value);
                    }
                }
            }
            ClientMessage::Ping => {
                if let Some(session) = self
                    .inner
                    .lock()
                    .await
                    .sessions
                    .get(id)
                    .filter(|s| s.generation == generation)
                {
                    let _ = session.sender.try_send(ServerMessage::Pong);
                }
            }
            ClientMessage::Hello { .. } => {}
        }
    }
    async fn process_utterance(
        self: Arc<Self>,
        id: String,
        generation: Uuid,
        listening_epoch: Uuid,
        samples: Vec<i16>,
    ) {
        let cancel = {
            let mut inner = self.inner.lock().await;
            let Some(session) = inner.sessions.get_mut(&id).filter(|s| {
                s.generation == generation
                    && s.listening_epoch == listening_epoch
                    && s.phase == Phase::Listening
                    && s.capture.is_none()
            }) else {
                return;
            };
            if self.paused() || session.muted {
                return;
            }
            session.phase = Phase::Transcribing;
            session.listening_cancel.child_token()
        };
        self.recognition_phase(&id, generation, listening_epoch, Phase::Transcribing, None)
            .await;
        let settings = self.settings();
        let utterance_seconds = samples.len() as f64 / SAMPLE_RATE as f64;
        let utterance_rms = audio::level(&samples);
        let operation = async {
            let _permit = self.workers.clone().acquire_owned().await?;
            if cancel.is_cancelled() {
                bail!("Cancelled");
            }
            self.speech
                .transcribe(samples, settings.clone(), cancel.clone())
                .await
        };
        let result = tokio::select! {
            _ = cancel.cancelled() => return,
            result = tokio::time::timeout(Duration::from_secs(120), operation) => result.unwrap_or_else(|_| Err(anyhow::anyhow!("Speech recognition timed out"))),
        };
        if cancel.is_cancelled() {
            return;
        }
        {
            let mut inner = self.inner.lock().await;
            if let Some(session) = inner
                .sessions
                .get_mut(&id)
                .filter(|s| s.generation == generation && s.listening_epoch == listening_epoch)
            {
                if let Some(observer) = session.observer.take() {
                    let text = result.as_ref().ok().cloned();
                    let wake_name = text
                        .as_deref()
                        .and_then(|t| audio::wake_name(t, &settings.wake_names));
                    let _ = observer.sender.send(RecognitionTestResult {
                        speaker_id: id.clone(),
                        text,
                        wake_name,
                        configured_wake_names: settings.wake_names.clone(),
                        utterance_seconds,
                        utterance_rms,
                        model_filename: std::path::Path::new(&settings.whisper_model)
                            .file_name()
                            .map(|p| p.to_string_lossy().into_owned())
                            .unwrap_or_default(),
                        error: result.as_ref().err().map(|e| e.to_string()),
                    });
                }
            }
        }
        match result {
            Ok(text) => {
                if let Some(name) = audio::wake_name(&text, &settings.wake_names) {
                    if let Err(error) = self
                        .dispatch(
                            id.clone(),
                            generation,
                            listening_epoch,
                            text,
                            name,
                            settings,
                        )
                        .await
                    {
                        self.recognition_phase(
                            &id,
                            generation,
                            listening_epoch,
                            Phase::Listening,
                            Some(error.to_string()),
                        )
                        .await;
                    }
                } else {
                    self.recognition_phase(
                        &id,
                        generation,
                        listening_epoch,
                        Phase::Listening,
                        None,
                    )
                    .await;
                }
            }
            Err(error) => {
                cancel.cancel();
                self.recognition_phase(
                    &id,
                    generation,
                    listening_epoch,
                    Phase::Listening,
                    Some(error.to_string()),
                )
                .await;
            }
        }
    }
    async fn dispatch(
        self: &Arc<Self>,
        id: String,
        generation: Uuid,
        listening_epoch: Uuid,
        text: String,
        wake_name: String,
        settings: Settings,
    ) -> Result<()> {
        if settings.outbound_url.is_empty() {
            bail!("Set a prompt destination URL in Settings");
        }
        let speaker = settings
            .speakers
            .iter()
            .find(|s| s.id == id)
            .context("Speaker registration changed")?;
        let request_id = Uuid::new_v4();
        let request = PromptRequest {
            version: PROTOCOL_VERSION,
            request_id,
            conversation_id: id.clone(),
            speaker: SpeakerIdentity {
                id: id.clone(),
                name: speaker.name.clone(),
                tags: speaker.tags.clone(),
            },
            text,
            wake_name,
            response_url: settings.callback_url(),
        };
        let cancel = {
            let mut inner = self.inner.lock().await;
            let session = inner
                .sessions
                .get_mut(&id)
                .filter(|s| {
                    s.generation == generation
                        && s.listening_epoch == listening_epoch
                        && !s.cancel.is_cancelled()
                        && !s.muted
                        && !self.paused()
                })
                .context("Speaker disconnected, paused, or muted")?;
            session.reset_listening();
            session.phase = Phase::Waiting;
            session.active_request = Some(request_id);
            session.requests += 1;
            let cancel = session.cancel.clone();
            inner.pending.insert(
                (id.clone(), request_id),
                Pending {
                    generation,
                    deadline: Instant::now()
                        + Duration::from_secs(settings.response_timeout_seconds as u64),
                    fingerprint: None,
                },
            );
            cancel
        };
        self.phase(&id, generation, Phase::Waiting, None).await;
        let outcome = self.send_request(&settings, &request);
        let result = tokio::select! {
            _ = cancel.cancelled() => return Ok(()),
            result = tokio::time::timeout(Duration::from_secs(settings.response_timeout_seconds as u64), outcome) => result.unwrap_or_else(|_| Err(anyhow::anyhow!("Prompt destination timed out"))),
        };
        match result {
            Ok(Some(text)) => {
                if let Err(error) = self
                    .respond(TextResponse {
                        request_id,
                        speaker_id: id.clone(),
                        text,
                    })
                    .await
                {
                    if !self.response_accepted(&id, request_id).await {
                        self.finish(&id, generation, request_id, Some(error.to_string()))
                            .await;
                    }
                }
            }
            Ok(None) => {}
            Err(error) => {
                // A callback can arrive before the outbound HTTP acknowledgement. It owns the result once accepted.
                let accepted = self.response_accepted(&id, request_id).await;
                if !accepted {
                    self.finish(&id, generation, request_id, Some(error.to_string()))
                        .await;
                }
            }
        }
        Ok(())
    }
    async fn response_accepted(&self, id: &str, request_id: Uuid) -> bool {
        let inner = self.inner.lock().await;
        let key = (id.to_owned(), request_id);
        inner
            .pending
            .get(&key)
            .is_some_and(|p| p.fingerprint.is_some())
            || inner.receipts.contains_key(&key)
    }
    async fn send_request(
        &self,
        settings: &Settings,
        request: &PromptRequest,
    ) -> Result<Option<String>> {
        let payload = if settings.backend_mode == BackendMode::Webhook {
            serde_json::to_value(request)?
        } else {
            let spoken_context = format!("{}\n\nCurrent local date and time on this speaker hub's PC: {}. Use this clock reading when asked for the current time.", settings.system_prompt, chrono::Local::now().to_rfc3339());
            json!({"model":settings.model,"max_tokens":settings.model_max_tokens,"stream":false,"messages":[{"role":"system","content":spoken_context},{"role":"user","content":request.text}]})
        };
        let mut builder = self
            .http
            .post(&settings.outbound_url)
            .header("X-Speaker-Id", &request.speaker.id)
            .header("X-Request-Id", request.request_id.to_string())
            .json(&payload);
        if !settings.outbound_token.is_empty() {
            builder = builder.bearer_auth(&settings.outbound_token);
        }
        if settings.backend_mode == BackendMode::OpenAi
            && !settings.router_instance_id.is_empty()
            && !settings.router_header_name.is_empty()
        {
            let mut identity = Sha256::new();
            identity.update(b"smart-speaker/router/v1\0");
            identity.update(settings.router_instance_id.as_bytes());
            identity.update(b"\0");
            identity.update(request.speaker.id.as_bytes());
            builder = builder.header(
                &settings.router_header_name,
                format!("speaker-{:x}", identity.finalize()),
            );
        }
        let mut response = builder
            .send()
            .await
            .map_err(|_| anyhow::anyhow!("Cannot reach the prompt destination"))?;
        if !response.status().is_success() {
            bail!(
                "Prompt destination returned HTTP {}",
                response.status().as_u16()
            );
        }
        if response.status() == reqwest::StatusCode::ACCEPTED
            || response.status() == reqwest::StatusCode::NO_CONTENT
        {
            return Ok(None);
        }
        let mut bytes = Vec::new();
        while let Some(chunk) = response.chunk().await? {
            if bytes.len() + chunk.len() > 1024 * 1024 {
                bail!("Prompt destination response is too large");
            }
            bytes.extend_from_slice(&chunk);
        }
        if bytes.is_empty() {
            return Ok(None);
        }
        let value: serde_json::Value =
            serde_json::from_slice(&bytes).context("Prompt destination did not return JSON")?;
        let text = if settings.backend_mode == BackendMode::OpenAi {
            value
                .pointer("/choices/0/message/content")
                .and_then(|v| v.as_str())
        } else {
            value.get("text").and_then(|v| v.as_str())
        };
        if settings.backend_mode == BackendMode::OpenAi && text.is_none() {
            bail!("Model returned no spoken text");
        }
        Ok(text.map(str::to_owned))
    }
    pub async fn respond(self: &Arc<Self>, response: TextResponse) -> Result<bool> {
        validate_text(&response.text)?;
        let key = (response.speaker_id.clone(), response.request_id);
        let fingerprint: [u8; 32] = Sha256::digest(response.text.as_bytes()).into();
        let generation = {
            let mut inner = self.inner.lock().await;
            let now = Instant::now();
            inner.receipts.retain(|_, receipt| receipt.expires > now);
            if let Some(receipt) = inner.receipts.get(&key) {
                if receipt.fingerprint == fingerprint {
                    return Ok(false);
                }
                bail!("Request ID was already used with different text");
            }
            let pending = inner
                .pending
                .get_mut(&key)
                .context("No pending request for this speaker and request ID")?;
            if pending.deadline <= now {
                bail!("Request expired");
            }
            if let Some(previous) = pending.fingerprint {
                if previous == fingerprint {
                    return Ok(false);
                }
                bail!("Request ID was already used with different text");
            }
            pending.fingerprint = Some(fingerprint);
            pending.deadline = now + Duration::from_secs(240);
            pending.generation
        };
        let hub = self.clone();
        tokio::spawn(async move {
            hub.speak(response, generation).await;
        });
        Ok(true)
    }
    pub async fn announce(self: &Arc<Self>, announcement: Announcement) -> Result<Vec<String>> {
        validate_text(&announcement.text)?;
        if announcement.speaker_ids.is_empty() && announcement.tags.is_empty() {
            bail!("Choose speaker IDs or tags");
        }
        if self.paused() {
            bail!("Hub is paused");
        }
        let settings = self.settings();
        let selected: Vec<String> = settings
            .speakers
            .iter()
            .filter(|s| {
                s.enabled
                    && (announcement.speaker_ids.contains(&s.id)
                        || s.tags.iter().any(|tag| announcement.tags.contains(tag)))
            })
            .map(|s| s.id.clone())
            .collect();
        if selected.is_empty()
            || announcement
                .speaker_ids
                .iter()
                .any(|id| !selected.contains(id))
        {
            bail!("One or more selected speakers are not registered and enabled");
        }
        let fingerprint: [u8; 32] = Sha256::digest(announcement.text.as_bytes()).into();
        {
            let mut inner = self.inner.lock().await;
            for id in &selected {
                let key = (id.clone(), announcement.request_id);
                if let Some(p) = inner.pending.get(&key) {
                    if p.fingerprint != Some(fingerprint) {
                        bail!("Request ID already in use");
                    }
                    continue;
                }
                if let Some(r) = inner.receipts.get(&key) {
                    if r.fingerprint != fingerprint {
                        bail!("Request ID already in use");
                    }
                    continue;
                }
                let session = inner
                    .sessions
                    .get(id)
                    .context("A selected speaker is offline")?;
                if session.phase != Phase::Listening || session.capture.is_some() {
                    bail!("A selected speaker is busy or muted");
                }
            }
            for id in &selected {
                let key = (id.clone(), announcement.request_id);
                if inner.pending.contains_key(&key) || inner.receipts.contains_key(&key) {
                    continue;
                }
                let session = inner.sessions.get_mut(id).context("Speaker disconnected")?;
                session.reset_listening();
                session.active_request = Some(announcement.request_id);
                session.phase = Phase::Waiting;
                let generation = session.generation;
                inner.pending.insert(
                    key,
                    Pending {
                        generation,
                        deadline: Instant::now() + Duration::from_secs(30),
                        fingerprint: None,
                    },
                );
            }
        }
        for id in &selected {
            self.respond(TextResponse {
                request_id: announcement.request_id,
                speaker_id: id.clone(),
                text: announcement.text.clone(),
            })
            .await?;
        }
        Ok(selected)
    }
    async fn speak(self: Arc<Self>, response: TextResponse, generation: Uuid) {
        let id = &response.speaker_id;
        let cancel = {
            let inner = self.inner.lock().await;
            let Some(session) = inner
                .sessions
                .get(id)
                .filter(|s| s.generation == generation)
            else {
                return;
            };
            session.cancel.clone()
        };
        self.phase(id, generation, Phase::Synthesizing, None).await;
        let operation = self.speech.synthesize(response.text, self.settings());
        let result = tokio::select! {
            _ = cancel.cancelled() => return,
            result = tokio::time::timeout(Duration::from_secs(60), operation) => result.unwrap_or_else(|_| Err(anyhow::anyhow!("Speech synthesis timed out"))),
        };
        match result {
            Ok(bytes) => {
                if bytes.len() > 6_000_000 {
                    self.finish(
                        id,
                        generation,
                        response.request_id,
                        Some("Spoken response exceeds the audio limit".into()),
                    )
                    .await;
                    return;
                }
                let audio_id = Uuid::new_v4();
                let mut inner = self.inner.lock().await;
                let Some(session) = inner.sessions.get_mut(id).filter(|s| {
                    s.generation == generation
                        && !s.cancel.is_cancelled()
                        && s.active_request == Some(response.request_id)
                }) else {
                    return;
                };
                let message = ServerMessage::Play {
                    request_id: response.request_id,
                    audio_url: format!(
                        "{}/v1/audio/{audio_id}",
                        self.settings().public_url.trim_end_matches('/')
                    ),
                    sample_rate: SAMPLE_RATE,
                    channels: 1,
                    bytes: bytes.len(),
                };
                if session.sender.try_send(message).is_err() {
                    session.cancel.cancel();
                    return;
                }
                session.phase = Phase::Speaking;
                inner.audio.insert(
                    audio_id,
                    AudioAsset {
                        speaker_id: id.clone(),
                        generation,
                        request_id: response.request_id,
                        expires: Instant::now() + Duration::from_secs(240),
                        bytes: Arc::new(bytes),
                    },
                );
            }
            Err(error) => {
                self.finish(id, generation, response.request_id, Some(error.to_string()))
                    .await
            }
        }
    }
    pub async fn audio_asset(&self, speaker_id: &str, audio_id: Uuid) -> Option<Arc<Vec<u8>>> {
        let inner = self.inner.lock().await;
        let asset = inner.audio.get(&audio_id)?;
        let session = inner.sessions.get(speaker_id)?;
        (asset.speaker_id == speaker_id
            && asset.generation == session.generation
            && asset.expires > Instant::now())
        .then(|| asset.bytes.clone())
    }
    async fn finish(&self, id: &str, generation: Uuid, request_id: Uuid, error: Option<String>) {
        let mut inner = self.inner.lock().await;
        if !inner
            .sessions
            .get(id)
            .is_some_and(|s| s.generation == generation && s.active_request == Some(request_id))
        {
            return;
        }
        if let Some(pending) = inner.pending.remove(&(id.to_owned(), request_id)) {
            if let Some(fingerprint) = pending.fingerprint {
                if inner.receipts.len() >= 2048 {
                    if let Some(key) = inner
                        .receipts
                        .iter()
                        .min_by_key(|(_, r)| r.expires)
                        .map(|(k, _)| k.clone())
                    {
                        inner.receipts.remove(&key);
                    }
                }
                inner.receipts.insert(
                    (id.to_owned(), request_id),
                    Receipt {
                        fingerprint,
                        expires: Instant::now() + Duration::from_secs(600),
                    },
                );
            }
        }
        inner.audio.retain(|_, a| {
            a.speaker_id != id || a.generation != generation || a.request_id != request_id
        });
        if let Some(session) = inner.sessions.get_mut(id) {
            session.active_request = None;
            session.last_error = error;
            session.phase = if self.paused() {
                Phase::Paused
            } else if session.muted {
                Phase::Muted
            } else {
                Phase::Listening
            };
            let _ = session.sender.try_send(ServerMessage::State {
                state: session.phase,
            });
        }
    }
    pub async fn maintenance(&self) {
        let expired = {
            let mut inner = self.inner.lock().await;
            let now = Instant::now();
            inner.receipts.retain(|_, r| r.expires > now);
            inner.audio.retain(|_, a| a.expires > now);
            for session in inner.sessions.values_mut() {
                if session
                    .observer
                    .as_ref()
                    .is_some_and(|o| o.expires <= now || o.sender.is_closed())
                {
                    session.observer = None;
                }
            }
            inner
                .pending
                .iter()
                .filter(|(_, p)| p.deadline <= now)
                .map(|((id, request), p)| (id.clone(), *request, p.generation))
                .collect::<Vec<_>>()
        };
        for (id, request_id, generation) in expired {
            self.finish(
                &id,
                generation,
                request_id,
                Some("Response or playback timed out".into()),
            )
            .await;
        }
    }
}

fn validate_text(text: &str) -> Result<()> {
    if text.trim().is_empty() || text.len() > 12_000 {
        bail!("Response text must contain 1 to 12000 bytes");
    }
    Ok(())
}
