use serde::{Deserialize, Serialize};
use uuid::Uuid;

pub const SAMPLE_RATE: u32 = 16_000;
pub const FRAME_SAMPLES: usize = 320;
pub const PROTOCOL_VERSION: u16 = 1;

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SpeakerIdentity {
    pub id: String,
    pub name: String,
    pub tags: Vec<String>,
}

#[derive(Clone, Serialize, Deserialize)]
pub struct PromptRequest {
    pub version: u16,
    pub request_id: Uuid,
    pub conversation_id: String,
    pub speaker: SpeakerIdentity,
    pub text: String,
    pub wake_name: String,
    pub response_url: String,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct TextResponse {
    pub request_id: Uuid,
    pub speaker_id: String,
    pub text: String,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Announcement {
    pub request_id: Uuid,
    #[serde(default)]
    pub speaker_ids: Vec<String>,
    #[serde(default)]
    pub tags: Vec<String>,
    pub text: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MicrophoneSampleRequest {
    pub seconds: u32,
}

#[derive(Serialize)]
pub struct RecognitionTestResult {
    pub speaker_id: String,
    pub text: Option<String>,
    pub wake_name: Option<String>,
    pub configured_wake_names: Vec<String>,
    pub utterance_seconds: f64,
    pub utterance_rms: f32,
    pub model_filename: String,
    pub error: Option<String>,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case", deny_unknown_fields)]
pub enum ClientMessage {
    Hello {
        protocol: u16,
        speaker_id: String,
        firmware: String,
        sample_rate: u32,
        channels: u8,
        sample_format: String,
    },
    PlaybackStarted {
        request_id: Uuid,
    },
    PlaybackFinished {
        request_id: Uuid,
    },
    PlaybackError {
        request_id: Uuid,
        code: String,
    },
    Muted {
        value: bool,
    },
    Volume {
        value: u8,
    },
    Ping,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum ServerMessage {
    Ready {
        protocol: u16,
        frame_samples: usize,
        #[serde(default = "crate::config::default_volume")]
        volume: u8,
    },
    State {
        state: Phase,
    },
    Play {
        request_id: Uuid,
        audio_url: String,
        sample_rate: u32,
        channels: u8,
        bytes: usize,
    },
    Cancel,
    Pong,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    Listening,
    Transcribing,
    Waiting,
    Synthesizing,
    Speaking,
    Muted,
    Paused,
    Offline,
}

#[derive(Clone, Serialize)]
pub struct SpeakerStatus {
    pub speaker: SpeakerIdentity,
    pub connected: bool,
    pub phase: Phase,
    pub level: f32,
    pub input_level: f32,
    pub microphone_gain: f32,
    pub configured_volume: u8,
    pub device_volume: Option<u8>,
    pub queued_utterances: usize,
    pub audio_frames: u64,
    pub requests: u64,
    pub microphone_test_active: bool,
    pub recognition_test_active: bool,
    pub firmware: String,
    pub last_error: Option<String>,
}

#[derive(Clone, Serialize)]
pub struct HubStatus {
    pub paused: bool,
    pub listening_address: Option<String>,
    pub model_ready: bool,
    pub speakers: Vec<SpeakerStatus>,
}
