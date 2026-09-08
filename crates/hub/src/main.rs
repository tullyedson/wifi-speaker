use anyhow::{Context, Result};
use smart_speaker_hub::{
    config::ConfigStore,
    engine::Hub,
    server,
    speech::{self, ISpeechEngine, LocalSpeech},
};
use std::{path::PathBuf, sync::Arc};
use tokio_util::sync::CancellationToken;

#[tokio::main]
async fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let value = |key: &str| {
        args.iter()
            .position(|s| s == key)
            .and_then(|i| args.get(i + 1))
            .cloned()
    };
    if args.iter().any(|a| a == "--help") {
        println!("speaker-hub --config PATH\nspeaker-hub --download-model DIRECTORY\nspeaker-hub --voices\nspeaker-hub --tts TEXT --output WAV\nspeaker-hub --transcribe WAV --model FILE");
        return Ok(());
    }
    if let Some(directory) = value("--download-model") {
        println!(
            "{}",
            speech::download_model(&PathBuf::from(directory))
                .await?
                .display()
        );
        return Ok(());
    }
    if args.iter().any(|a| a == "--voices") {
        println!("{}", serde_json::to_string_pretty(&speech::voices()?)?);
        return Ok(());
    }
    let speech = Arc::new(LocalSpeech::default());
    if let Some(text) = value("--tts") {
        let output = value("--output").context("--output WAV is required")?;
        let bytes = speech.synthesize(text, Default::default()).await?;
        std::fs::write(output, bytes)?;
        return Ok(());
    }
    if let Some(path) = value("--transcribe") {
        let model = value("--model").context("--model FILE is required")?;
        let normalized = smart_speaker_hub::audio::normalize_wav(&std::fs::read(path)?)?;
        let samples = hound::WavReader::new(std::io::Cursor::new(normalized))?
            .into_samples::<i16>()
            .collect::<std::result::Result<Vec<_>, _>>()?;
        let settings = smart_speaker_hub::config::Settings {
            whisper_model: model,
            ..Default::default()
        };
        println!(
            "{}",
            speech
                .transcribe(samples, settings, CancellationToken::new())
                .await?
        );
        return Ok(());
    }
    let path = value("--config")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("runtime/settings.json"));
    let store = ConfigStore::new(path);
    let hub = Hub::new(store.load()?, speech)?;
    let listener = server::start(hub.clone()).await?;
    println!("Smart Speaker hub listening on {}", listener.address);
    tokio::signal::ctrl_c().await?;
    listener.stop(&hub).await;
    Ok(())
}
