use crate::{audio::normalize_wav, config::Settings};
use anyhow::{bail, Context, Result};
use async_trait::async_trait;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};
use tokio_util::sync::CancellationToken;
use whisper_rs::{FullParams, SamplingStrategy, WhisperContext, WhisperContextParameters};

pub const MODEL_URL: &str =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin";
pub const MODEL_SHA256: &str = "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002";
pub const MODEL_SIZE: u64 = 147_964_211;

#[async_trait]
pub trait ISpeechEngine: Send + Sync {
    async fn transcribe(
        &self,
        samples: Vec<i16>,
        settings: Settings,
        cancel: CancellationToken,
    ) -> Result<String>;
    async fn synthesize(&self, text: String, settings: Settings) -> Result<Vec<u8>>;
}

type CachedWhisper = Arc<Mutex<Option<(String, WhisperContext)>>>;
#[derive(Default)]
pub struct LocalSpeech {
    context: CachedWhisper,
}

#[async_trait]
impl ISpeechEngine for LocalSpeech {
    async fn transcribe(
        &self,
        samples: Vec<i16>,
        settings: Settings,
        cancel: CancellationToken,
    ) -> Result<String> {
        // Native debug logs can contain recognized words. Both log backends are disabled.
        whisper_rs::install_logging_hooks();
        let shared = self.context.clone();
        tokio::task::spawn_blocking(move || {
            if cancel.is_cancelled() {
                bail!("Transcription cancelled");
            }
            if settings.whisper_model.is_empty() {
                bail!("Download or select a Whisper model in Settings");
            }
            let mut cache = shared
                .lock()
                .map_err(|_| anyhow::anyhow!("Speech recognizer needs restarting"))?;
            if cache
                .as_ref()
                .is_none_or(|(path, _)| *path != settings.whisper_model)
            {
                let mut options = WhisperContextParameters::default();
                options.use_gpu(false);
                let context = WhisperContext::new_with_params(&settings.whisper_model, options)
                    .context("Cannot load the selected Whisper model")?;
                *cache = Some((settings.whisper_model.clone(), context));
            }
            if cancel.is_cancelled() {
                bail!("Transcription cancelled");
            }
            let context = &cache.as_ref().context("Speech model unavailable")?.1;
            let mut state = context.create_state()?;
            let mut params = FullParams::new(SamplingStrategy::Greedy { best_of: 1 });
            params.set_n_threads(settings.whisper_threads as i32);
            params.set_language(Some("en"));
            params.set_translate(false);
            params.set_no_context(true);
            params.set_print_special(false);
            params.set_print_progress(false);
            params.set_print_realtime(false);
            params.set_print_timestamps(false);
            let abort = Box::new(cancel);
            // The token has a stable address and outlives the synchronous native call.
            // The callback only reads the thread-safe token; no context/state access occurs.
            // whisper-rs 0.16.0's closure wrapper erases its allocation type incorrectly.
            unsafe {
                params.set_abort_callback(Some(abort_transcription));
                params.set_abort_callback_user_data(
                    (&*abort as *const CancellationToken).cast_mut().cast(),
                );
            }
            let audio: Vec<f32> = samples.iter().map(|s| *s as f32 / 32768.0).collect();
            let result = state.full(params, &audio);
            drop(abort);
            result?;
            let mut text = String::new();
            for segment in state.as_iter() {
                text.push_str(&segment.to_str_lossy()?);
            }
            Ok(text.trim().to_owned())
        })
        .await
        .context("Speech recognition worker stopped")?
    }
    async fn synthesize(&self, text: String, settings: Settings) -> Result<Vec<u8>> {
        tokio::task::spawn_blocking(move || synthesize_windows(&text, &settings.voice_id))
            .await
            .context("Voice synthesis worker stopped")?
    }
}

unsafe extern "C" fn abort_transcription(data: *mut std::ffi::c_void) -> bool {
    // SAFETY: installed only above, with a live CancellationToken for the complete call.
    unsafe { (&*data.cast::<CancellationToken>()).is_cancelled() }
}

#[derive(Clone, Serialize)]
pub struct Voice {
    pub id: String,
    pub name: String,
    pub language: String,
}

#[cfg(windows)]
struct RuntimeApartment;
#[cfg(windows)]
impl RuntimeApartment {
    fn new() -> Result<Self> {
        unsafe {
            windows::Win32::System::WinRT::RoInitialize(
                windows::Win32::System::WinRT::RO_INIT_MULTITHREADED,
            )?;
        }
        Ok(Self)
    }
}
#[cfg(windows)]
impl Drop for RuntimeApartment {
    fn drop(&mut self) {
        unsafe {
            windows::Win32::System::WinRT::RoUninitialize();
        }
    }
}

#[cfg(windows)]
pub fn voices() -> Result<Vec<Voice>> {
    let _apartment = RuntimeApartment::new()?;
    let installed = windows::Media::SpeechSynthesis::SpeechSynthesizer::AllVoices()?;
    let mut output = Vec::new();
    for voice in installed {
        output.push(Voice {
            id: voice.Id()?.to_string(),
            name: voice.DisplayName()?.to_string(),
            language: voice.Language()?.to_string(),
        });
    }
    Ok(output)
}
#[cfg(not(windows))]
pub fn voices() -> Result<Vec<Voice>> {
    Ok(vec![])
}

#[cfg(windows)]
pub fn synthesize_windows(text: &str, voice_id: &str) -> Result<Vec<u8>> {
    use windows::{
        core::HSTRING,
        Media::SpeechSynthesis::SpeechSynthesizer,
        Storage::Streams::{Buffer, DataReader, InputStreamOptions},
    };
    let _apartment = RuntimeApartment::new()?;
    let synth = SpeechSynthesizer::new()?;
    if !voice_id.is_empty() {
        let mut found = false;
        for voice in SpeechSynthesizer::AllVoices()? {
            if voice.Id()? == voice_id {
                synth.SetVoice(&voice)?;
                found = true;
                break;
            }
        }
        if !found {
            bail!("The selected Windows voice is no longer installed");
        }
    }
    let stream = synth
        .SynthesizeTextToStreamAsync(&HSTRING::from(text))?
        .get()?;
    let size = stream.Size()?;
    if size > 24 * 1024 * 1024 {
        bail!("Generated speech is too large");
    }
    let buffer = stream
        .ReadAsync(
            &Buffer::Create(size as u32)?,
            size as u32,
            InputStreamOptions::None,
        )?
        .get()?;
    let mut bytes = vec![0; buffer.Length()? as usize];
    DataReader::FromBuffer(&buffer)?.ReadBytes(&mut bytes)?;
    stream.Close()?;
    synth.Close()?;
    normalize_wav(&bytes)
}
#[cfg(not(windows))]
pub fn synthesize_windows(_text: &str, _voice_id: &str) -> Result<Vec<u8>> {
    bail!("This build uses Windows installed voices for local TTS")
}

pub async fn download_model(directory: &Path) -> Result<PathBuf> {
    use futures_util::StreamExt;
    use tokio::io::AsyncWriteExt;
    tokio::fs::create_dir_all(directory).await?;
    let destination = directory.join("ggml-base.en.bin");
    let temporary = directory.join(format!("model-{}.part", uuid::Uuid::new_v4().simple()));
    let result = async {
        let client = reqwest::Client::builder()
            .timeout(std::time::Duration::from_secs(600))
            .build()?;
        let response = client.get(MODEL_URL).send().await?.error_for_status()?;
        let mut chunks = response.bytes_stream();
        let mut file = tokio::fs::File::create(&temporary).await?;
        let mut hasher = Sha256::new();
        let mut size = 0_u64;
        while let Some(chunk) = chunks.next().await {
            let chunk = chunk?;
            size += chunk.len() as u64;
            if size > MODEL_SIZE {
                bail!("Model download exceeds expected size");
            }
            hasher.update(&chunk);
            file.write_all(&chunk).await?;
        }
        if size != MODEL_SIZE || format!("{:x}", hasher.finalize()) != MODEL_SHA256 {
            bail!("Model integrity check failed");
        }
        file.sync_all().await?;
        drop(file);
        tokio::fs::rename(&temporary, &destination).await?;
        Ok(destination.clone())
    }
    .await;
    if result.is_err() {
        let _ = tokio::fs::remove_file(&temporary).await;
    }
    result
}
