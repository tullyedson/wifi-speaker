use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashSet,
    net::SocketAddr,
    path::{Path, PathBuf},
};
use url::Url;
use uuid::Uuid;

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq, Default)]
#[serde(rename_all = "snake_case")]
pub enum BackendMode {
    #[default]
    Webhook,
    OpenAi,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Settings {
    pub version: u32,
    pub listen_url: String,
    pub public_url: String,
    pub outbound_url: String,
    pub outbound_token: String,
    pub api_token: String,
    pub backend_mode: BackendMode,
    pub model: String,
    pub model_max_tokens: u32,
    pub router_instance_id: String,
    pub router_header_name: String,
    pub system_prompt: String,
    pub wake_names: Vec<String>,
    pub whisper_model: String,
    pub whisper_threads: u16,
    pub voice_id: String,
    pub speech_threshold: f32,
    pub silence_ms: u32,
    pub max_utterance_seconds: u32,
    pub response_timeout_seconds: u32,
    pub speakers: Vec<SpeakerConfig>,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SpeakerConfig {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub tags: Vec<String>,
    pub token: String,
    #[serde(default = "enabled")]
    pub enabled: bool,
    #[serde(default = "default_microphone_gain")]
    pub microphone_gain: f32,
    #[serde(default = "default_volume")]
    pub volume: u8,
}
pub fn default_microphone_gain() -> f32 {
    1.0
}
pub fn default_volume() -> u8 {
    65
}
fn enabled() -> bool {
    true
}
pub fn new_token() -> String {
    format!("{}{}", Uuid::new_v4().simple(), Uuid::new_v4().simple())
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            version: 1,
            listen_url: "http://0.0.0.0:48490".into(),
            public_url: "http://127.0.0.1:48490".into(),
            outbound_url: String::new(),
            outbound_token: String::new(),
            api_token: new_token(),
            backend_mode: BackendMode::Webhook,
            model: String::new(),
            model_max_tokens: 1024,
            router_instance_id: String::new(),
            router_header_name: String::new(),
            system_prompt: "Reply concisely in plain language suitable for speaking aloud.".into(),
            wake_names: vec!["Speaker".into()],
            whisper_model: String::new(),
            whisper_threads: 4,
            voice_id: String::new(),
            speech_threshold: 0.012,
            silence_ms: 850,
            max_utterance_seconds: 30,
            response_timeout_seconds: 120,
            speakers: vec![],
        }
    }
}

pub fn valid_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 64
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

impl Settings {
    pub fn bind_address(&self) -> Result<SocketAddr> {
        let url = checked_url(&self.listen_url)?;
        if url.scheme() != "http" || url.path() != "/" || url.query().is_some() {
            bail!("Listen URL must be an http:// IP address and port, without a path or query");
        }
        let ip = url
            .host_str()
            .context("Listen URL needs an IP address")?
            .trim_matches(['[', ']'])
            .parse()?;
        Ok(SocketAddr::new(
            ip,
            url.port_or_known_default().context("Missing listen port")?,
        ))
    }
    pub fn callback_url(&self) -> String {
        format!("{}/v1/responses", self.public_url.trim_end_matches('/'))
    }
    pub fn validate(&self) -> Result<()> {
        if self.version != 1 {
            bail!("Unsupported settings version");
        }
        self.bind_address()?;
        let public = checked_url(&self.public_url)?;
        if public.scheme() != "http" || public.path() != "/" || public.query().is_some() {
            bail!("Public URL must use http:// without a path or query for this firmware");
        }
        if matches!(public.host_str(), Some("0.0.0.0" | "::" | "[::]")) {
            bail!("Public URL must be an address devices can reach, not a wildcard");
        }
        if !self.outbound_url.is_empty() {
            checked_url(&self.outbound_url)?;
        }
        if self.backend_mode == BackendMode::OpenAi && self.model.trim().is_empty() {
            bail!("An OpenAI-compatible model name is required");
        }
        if !(64..=16384).contains(&self.model_max_tokens) {
            bail!("Model response token limit must be between 64 and 16384");
        }
        if !self.router_instance_id.is_empty() && !valid_id(&self.router_instance_id) {
            bail!(
                "Router instance ID must contain 1 to 64 letters, numbers, hyphens or underscores"
            );
        }
        if !self.router_header_name.is_empty() {
            let name = self.router_header_name.to_ascii_lowercase();
            if !(3..=64).contains(&name.len())
                || !name.starts_with("x-")
                || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
                || matches!(name.as_str(), "x-speaker-id" | "x-request-id")
            {
                bail!("Caller ID header must be an X- header of 3 to 64 letters, numbers or hyphens, distinct from the speaker and request headers");
            }
            if self.router_instance_id.is_empty() {
                bail!("Set a hub instance ID when using a caller ID header");
            }
        }
        if !valid_token(&self.api_token) {
            bail!("App API token must contain 32 to 256 letters, numbers, hyphens or underscores");
        }
        if self.wake_names.is_empty()
            || self.wake_names.len() > 8
            || self
                .wake_names
                .iter()
                .any(|s| s.trim().is_empty() || s.len() > 64)
        {
            bail!("Choose one to eight wake names");
        }
        if !(1..=16).contains(&self.whisper_threads) {
            bail!("Speech recognition threads must be between 1 and 16");
        }
        if !self.speech_threshold.is_finite() || !(0.001..=0.3).contains(&self.speech_threshold) {
            bail!("Speech threshold must be between 0.001 and 0.3");
        }
        if !(300..=3000).contains(&self.silence_ms) {
            bail!("End-of-speech silence must be between 300 and 3000 milliseconds");
        }
        if !(5..=60).contains(&self.max_utterance_seconds) {
            bail!("Utterance limit must be between 5 and 60 seconds");
        }
        if !(10..=600).contains(&self.response_timeout_seconds) {
            bail!("Response timeout must be between 10 and 600 seconds");
        }
        if self.speakers.len() > 32 {
            bail!("This hub supports up to 32 registered speakers");
        }
        let mut ids = HashSet::new();
        let mut tokens = HashSet::new();
        for s in &self.speakers {
            if !valid_id(&s.id) || !ids.insert(&s.id) {
                bail!("Speaker IDs must be unique and contain only letters, numbers, hyphens and underscores");
            }
            if !s.microphone_gain.is_finite() || !(0.25..=8.0).contains(&s.microphone_gain) {
                bail!("Speaker microphone gain must be between 0.25 and 8");
            }
            if s.volume > 100 {
                bail!("Speaker volume must be between 0 and 100 percent");
            }
            if s.name.trim().is_empty() || s.name.len() > 128 {
                bail!("Speaker name must contain 1 to 128 characters");
            }
            if !valid_token(&s.token) || !tokens.insert(&s.token) || s.token == self.api_token {
                bail!("Each speaker needs its own unique token of 32 to 256 letters, numbers, hyphens or underscores");
            }
            if s.tags.len() > 16 || s.tags.iter().any(|t| t.trim().is_empty() || t.len() > 64) {
                bail!("Use up to 16 non-empty tags per speaker, each at most 64 characters");
            }
        }
        Ok(())
    }
}

pub fn checked_url(value: &str) -> Result<Url> {
    let url = Url::parse(value).context("Invalid URL")?;
    if !matches!(url.scheme(), "http" | "https")
        || url.host_str().is_none()
        || !url.username().is_empty()
        || url.password().is_some()
        || url.fragment().is_some()
    {
        bail!("Use an http:// or https:// URL without embedded credentials or fragments");
    }
    Ok(url)
}

fn valid_token(value: &str) -> bool {
    (32..=256).contains(&value.len())
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

pub struct ConfigStore {
    path: PathBuf,
}
impl ConfigStore {
    pub fn new(path: PathBuf) -> Self {
        Self { path }
    }
    pub fn path(&self) -> &Path {
        &self.path
    }
    pub fn load(&self) -> Result<Settings> {
        if !self.path.exists() {
            let settings = Settings::default();
            self.save(&settings)?;
            return Ok(settings);
        }
        let settings: Settings = serde_json::from_slice(&std::fs::read(&self.path)?)
            .context("Cannot read settings; existing file has been preserved")?;
        settings.validate()?;
        Ok(settings)
    }
    pub fn save(&self, settings: &Settings) -> Result<()> {
        settings.validate()?;
        if let Some(parent) = self.path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let temporary = self
            .path
            .with_extension(format!("{}.tmp", Uuid::new_v4().simple()));
        let result = (|| {
            use std::io::Write;
            let mut file = std::fs::OpenOptions::new()
                .create_new(true)
                .write(true)
                .open(&temporary)?;
            file.write_all(&serde_json::to_vec_pretty(settings)?)?;
            file.sync_all()?;
            drop(file);
            std::fs::rename(&temporary, &self.path)?;
            Ok(())
        })();
        if result.is_err() {
            let _ = std::fs::remove_file(&temporary);
        }
        result
    }
}
