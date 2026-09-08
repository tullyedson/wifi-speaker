use crate::{audio::pcm_from_bytes, engine::Hub, protocol::*};
use anyhow::Result;
use axum::{
    body::Body,
    extract::{
        ws::{Message, WebSocket},
        DefaultBodyLimit, Path, State, WebSocketUpgrade,
    },
    http::{header, HeaderMap, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use futures_util::{SinkExt, StreamExt};
use serde_json::json;
use std::{
    net::SocketAddr,
    sync::Arc,
    time::{Duration, Instant},
};
use subtle::ConstantTimeEq;
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

#[derive(Clone)]
struct ApiState {
    hub: Arc<Hub>,
    cancel: CancellationToken,
}
pub struct Listener {
    pub address: SocketAddr,
    cancel: CancellationToken,
    task: tokio::task::JoinHandle<()>,
}
impl Listener {
    pub async fn stop(mut self, hub: &Hub) {
        self.cancel.cancel();
        hub.disconnect_all().await;
        if tokio::time::timeout(Duration::from_secs(5), &mut self.task)
            .await
            .is_err()
        {
            self.task.abort();
            let _ = self.task.await;
        }
        hub.set_listener_address(None).await;
    }
}

pub async fn start(hub: Arc<Hub>) -> Result<Listener> {
    start_at(hub.clone(), hub.settings().bind_address()?).await
}
pub async fn start_at(hub: Arc<Hub>, bind: SocketAddr) -> Result<Listener> {
    let listener = tokio::net::TcpListener::bind(bind).await?;
    let address = listener.local_addr()?;
    let cancel = CancellationToken::new();
    let state = ApiState {
        hub: hub.clone(),
        cancel: cancel.clone(),
    };
    let router = Router::new()
        .route(
            "/healthz",
            get(|| async {
                Json(json!({"service":"smart-speaker","version":env!("CARGO_PKG_VERSION")}))
            }),
        )
        .route("/v1/speakers", get(speakers))
        .route("/v1/speakers/{id}/audio", get(speaker_socket))
        .route(
            "/v1/speakers/{id}/microphone-sample",
            post(microphone_sample),
        )
        .route("/v1/speakers/{id}/recognition-test", post(recognition_test))
        .route("/v1/responses", post(respond))
        .route("/v1/speak", post(announce))
        .route("/v1/audio/{id}", get(audio_asset))
        .layer(DefaultBodyLimit::max(32 * 1024))
        .with_state(state);
    hub.set_listener_address(Some(format!("http://{address}")))
        .await;
    let stop = cancel.clone();
    let maintenance_hub = hub.clone();
    let maintenance_stop = cancel.clone();
    let task = tokio::spawn(async move {
        let maintenance = tokio::spawn(async move {
            let mut ticker = tokio::time::interval(Duration::from_secs(1));
            loop {
                tokio::select! { _ = maintenance_stop.cancelled() => break, _ = ticker.tick() => maintenance_hub.maintenance().await }
            }
        });
        let _ = axum::serve(listener, router)
            .with_graceful_shutdown(stop.clone().cancelled_owned())
            .await;
        stop.cancel();
        let _ = maintenance.await;
    });
    Ok(Listener {
        address,
        cancel,
        task,
    })
}

fn token(headers: &HeaderMap) -> Option<&str> {
    headers
        .get(header::AUTHORIZATION)?
        .to_str()
        .ok()?
        .strip_prefix("Bearer ")
}
pub fn same_token(supplied: &str, expected: &str) -> bool {
    supplied.as_bytes().ct_eq(expected.as_bytes()).into()
}
fn app_authorized(state: &ApiState, headers: &HeaderMap) -> bool {
    token(headers).is_some_and(|t| same_token(t, &state.hub.settings().api_token))
}
fn unauthorized() -> Response {
    (
        StatusCode::UNAUTHORIZED,
        Json(json!({"error":"Valid bearer token required"})),
    )
        .into_response()
}
fn error(status: StatusCode, message: String) -> Response {
    (status, Json(json!({"error":message}))).into_response()
}

async fn speakers(State(state): State<ApiState>, headers: HeaderMap) -> Response {
    if !app_authorized(&state, &headers) {
        return unauthorized();
    }
    Json(state.hub.status().await).into_response()
}
async fn recognition_test(
    State(state): State<ApiState>,
    Path(id): Path<String>,
    headers: HeaderMap,
    Json(request): Json<MicrophoneSampleRequest>,
) -> Response {
    if !app_authorized(&state, &headers) {
        return unauthorized();
    }
    if !(1..=60).contains(&request.seconds) {
        return error(
            StatusCode::BAD_REQUEST,
            "Choose between 1 and 60 seconds".into(),
        );
    }
    match state.hub.observe_recognition(id, request.seconds).await {
        Ok(result) => ([(header::CACHE_CONTROL, "no-store")], Json(result)).into_response(),
        Err(err) => error(StatusCode::CONFLICT, err.to_string()),
    }
}
async fn microphone_sample(
    State(state): State<ApiState>,
    Path(id): Path<String>,
    headers: HeaderMap,
    Json(request): Json<MicrophoneSampleRequest>,
) -> Response {
    if !app_authorized(&state, &headers) {
        return unauthorized();
    }
    if !(1..=15).contains(&request.seconds) {
        return error(
            StatusCode::BAD_REQUEST,
            "Choose between 1 and 15 seconds".into(),
        );
    }
    match state.hub.capture_microphone(id, request.seconds).await {
        Ok(bytes) => Response::builder()
            .status(StatusCode::OK)
            .header(header::CONTENT_TYPE, "audio/wav")
            .header(header::CONTENT_LENGTH, bytes.len())
            .header(header::CACHE_CONTROL, "no-store")
            .body(Body::from(bytes))
            .unwrap(),
        Err(err) => error(StatusCode::CONFLICT, err.to_string()),
    }
}
async fn respond(
    State(state): State<ApiState>,
    headers: HeaderMap,
    Json(response): Json<TextResponse>,
) -> Response {
    if !app_authorized(&state, &headers) {
        return unauthorized();
    }
    match state.hub.respond(response).await {
        Ok(accepted) => (
            if accepted {
                StatusCode::ACCEPTED
            } else {
                StatusCode::OK
            },
            Json(json!({"status": if accepted { "accepted" } else { "duplicate" }})),
        )
            .into_response(),
        Err(err) => error(StatusCode::CONFLICT, err.to_string()),
    }
}
async fn announce(
    State(state): State<ApiState>,
    headers: HeaderMap,
    Json(announcement): Json<Announcement>,
) -> Response {
    if !app_authorized(&state, &headers) {
        return unauthorized();
    }
    match state.hub.announce(announcement).await {
        Ok(speakers) => (
            StatusCode::ACCEPTED,
            Json(json!({"status":"accepted","speaker_ids":speakers})),
        )
            .into_response(),
        Err(err) => error(StatusCode::CONFLICT, err.to_string()),
    }
}
async fn audio_asset(
    State(state): State<ApiState>,
    Path(id): Path<Uuid>,
    headers: HeaderMap,
) -> Response {
    let Some(supplied) = token(&headers) else {
        return unauthorized();
    };
    let Some(speaker) = state
        .hub
        .settings()
        .speakers
        .into_iter()
        .find(|s| s.enabled && same_token(supplied, &s.token))
    else {
        return unauthorized();
    };
    match state.hub.audio_asset(&speaker.id, id).await {
        Some(bytes) => Response::builder()
            .status(StatusCode::OK)
            .header(header::CONTENT_TYPE, "audio/wav")
            .header(header::CONTENT_LENGTH, bytes.len())
            .header(header::CACHE_CONTROL, "no-store")
            .body(Body::from(bytes.as_ref().clone()))
            .unwrap(),
        None => error(
            StatusCode::NOT_FOUND,
            "Audio is unavailable for this speaker or has expired".into(),
        ),
    }
}
async fn speaker_socket(
    State(state): State<ApiState>,
    Path(id): Path<String>,
    headers: HeaderMap,
    upgrade: WebSocketUpgrade,
) -> Response {
    let settings = state.hub.settings();
    let authorized = token(&headers).is_some_and(|t| {
        settings
            .speakers
            .iter()
            .any(|s| s.enabled && s.id == id && same_token(t, &s.token))
    });
    if !authorized {
        return unauthorized();
    }
    upgrade
        .max_message_size(6400)
        .max_frame_size(6400)
        .on_upgrade(move |socket| connected(state, id, socket))
}

async fn connected(state: ApiState, id: String, mut socket: WebSocket) {
    let hello = tokio::time::timeout(Duration::from_secs(5), socket.recv()).await;
    let Ok(Some(Ok(Message::Text(hello)))) = hello else {
        return;
    };
    let Ok(ClientMessage::Hello {
        protocol,
        speaker_id,
        firmware,
        sample_rate,
        channels,
        sample_format,
    }) = serde_json::from_str(&hello)
    else {
        return;
    };
    if protocol != PROTOCOL_VERSION
        || speaker_id != id
        || sample_rate != SAMPLE_RATE
        || channels != 1
        || sample_format != "s16le"
        || firmware.len() > 128
    {
        return;
    }
    let Ok((generation, mut outbound, cancel)) = state.hub.connect(&id, firmware).await else {
        return;
    };
    let (mut sender, mut receiver) = socket.split();
    let mut heartbeat = tokio::time::interval(Duration::from_secs(10));
    let mut last_message = Instant::now();
    loop {
        tokio::select! {
            _ = state.cancel.cancelled() => break,
            _ = cancel.cancelled() => break,
            _ = heartbeat.tick() => {
                if last_message.elapsed() > Duration::from_secs(45) { break; }
                if sender.send(Message::Ping(vec![].into())).await.is_err() { break; }
            }
            outgoing = outbound.recv() => {
                let Some(outgoing) = outgoing else { break; };
                let Ok(text) = serde_json::to_string(&outgoing) else { break; };
                if sender.send(Message::Text(text.into())).await.is_err() { break; }
            }
            incoming = receiver.next() => {
                let Some(Ok(incoming)) = incoming else { break; };
                last_message = Instant::now();
                match incoming {
                    Message::Binary(bytes) => {
                        let Ok(samples) = pcm_from_bytes(&bytes) else { break; };
                        state.hub.audio_frame(&id, generation, &samples).await;
                    }
                    Message::Text(text) => {
                        let Ok(event) = serde_json::from_str::<ClientMessage>(&text) else { break; };
                        if matches!(event, ClientMessage::Hello { .. }) { break; }
                        state.hub.client_event(&id, generation, event).await;
                    }
                    Message::Ping(payload) => { if sender.send(Message::Pong(payload)).await.is_err() { break; } }
                    Message::Pong(_) => {},
                    Message::Close(_) => break,
                }
            }
        }
    }
    cancel.cancel();
    let _ = tokio::time::timeout(Duration::from_secs(1), sender.close()).await;
    state.hub.disconnect(&id, generation).await;
}
