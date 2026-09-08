use crate::{config::Settings, protocol::SAMPLE_RATE};
use anyhow::{bail, Result};
use std::{collections::VecDeque, io::Cursor};

pub fn level(samples: &[i16]) -> f32 {
    if samples.is_empty() {
        return 0.0;
    }
    (samples
        .iter()
        .map(|s| (*s as f64 / 32768.0).powi(2))
        .sum::<f64>()
        / samples.len() as f64)
        .sqrt() as f32
}

/// Keeps a short pre-roll and a bounded current utterance. Audio never goes to disk.
pub struct UtteranceBuffer {
    pre: VecDeque<i16>,
    current: Vec<i16>,
    voiced: usize,
    quiet: usize,
    threshold: f32,
    silence_samples: usize,
    max_samples: usize,
}
impl UtteranceBuffer {
    pub fn new(settings: &Settings) -> Self {
        Self {
            pre: VecDeque::new(),
            current: vec![],
            voiced: 0,
            quiet: 0,
            threshold: settings.speech_threshold,
            silence_samples: settings.silence_ms as usize * 16,
            max_samples: settings.max_utterance_seconds as usize * SAMPLE_RATE as usize,
        }
    }
    pub fn reset(&mut self) {
        self.pre.clear();
        self.current.clear();
        self.voiced = 0;
        self.quiet = 0;
    }
    pub fn push(&mut self, samples: &[i16]) -> Option<Vec<i16>> {
        let speech = level(samples) >= self.threshold;
        if self.current.is_empty() {
            if speech {
                self.current.extend(self.pre.drain(..));
            } else {
                self.pre.extend(samples.iter().copied());
                while self.pre.len() > 8000 {
                    self.pre.pop_front();
                }
                return None;
            }
        }
        self.current.extend_from_slice(samples);
        if speech {
            self.voiced += samples.len();
            self.quiet = 0;
        } else {
            self.quiet += samples.len();
        }
        if self.quiet >= self.silence_samples || self.current.len() >= self.max_samples {
            let enough_speech = self.voiced >= 4000;
            let utterance = std::mem::take(&mut self.current);
            self.reset();
            return enough_speech.then_some(utterance);
        }
        None
    }
}

pub fn apply_gain(samples: &mut [i16], gain: f32) {
    for sample in samples {
        *sample = (*sample as f32 * gain).round().clamp(-32768.0, 32767.0) as i16;
    }
}

pub fn wake_name(text: &str, names: &[String]) -> Option<String> {
    let words: Vec<String> = text
        .split(|c: char| !c.is_alphanumeric())
        .filter(|s| !s.is_empty())
        .map(str::to_lowercase)
        .collect();
    for name in names {
        let target: Vec<String> = name
            .split(|c: char| !c.is_alphanumeric())
            .filter(|s| !s.is_empty())
            .map(str::to_lowercase)
            .collect();
        if !target.is_empty() && words.windows(target.len()).any(|w| w == target) {
            return Some(name.clone());
        }
    }
    None
}

pub fn pcm_from_bytes(bytes: &[u8]) -> Result<Vec<i16>> {
    if bytes.is_empty() || bytes.len() > 6400 || !bytes.len().is_multiple_of(2) {
        bail!("Invalid PCM frame length");
    }
    Ok(bytes
        .chunks_exact(2)
        .map(|b| i16::from_le_bytes([b[0], b[1]]))
        .collect())
}

pub fn encode_wav(samples: &[i16]) -> Result<Vec<u8>> {
    let mut cursor = Cursor::new(Vec::with_capacity(samples.len() * 2 + 44));
    let mut writer = hound::WavWriter::new(
        &mut cursor,
        hound::WavSpec {
            channels: 1,
            sample_rate: SAMPLE_RATE,
            bits_per_sample: 16,
            sample_format: hound::SampleFormat::Int,
        },
    )?;
    for sample in samples {
        writer.write_sample(*sample)?;
    }
    writer.finalize()?;
    Ok(cursor.into_inner())
}

/// Converts installed Windows voice output to the satellite's fixed PCM format.
pub fn normalize_wav(bytes: &[u8]) -> Result<Vec<u8>> {
    let reader = hound::WavReader::new(Cursor::new(bytes))?;
    let spec = reader.spec();
    if spec.channels == 0
        || spec.channels > 2
        || spec.sample_rate == 0
        || spec.bits_per_sample != 16
        || spec.sample_format != hound::SampleFormat::Int
    {
        bail!("TTS output must be a 16-bit PCM WAV");
    }
    let input: Vec<i16> = reader
        .into_samples::<i16>()
        .collect::<std::result::Result<_, _>>()?;
    let mono: Vec<f32> = input
        .chunks_exact(spec.channels as usize)
        .map(|c| c.iter().map(|s| *s as f32).sum::<f32>() / c.len() as f32)
        .collect();
    let count = mono.len() as u64 * SAMPLE_RATE as u64 / spec.sample_rate as u64;
    if count > SAMPLE_RATE as u64 * 180 {
        bail!("Spoken response exceeds three minutes");
    }
    let mut output = Vec::with_capacity(count as usize);
    for index in 0..count {
        let position = index as f64 * spec.sample_rate as f64 / SAMPLE_RATE as f64;
        let left = position as usize;
        let fraction = (position - left as f64) as f32;
        let a = mono.get(left).copied().unwrap_or_default();
        let b = mono.get(left + 1).copied().unwrap_or(a);
        output.push((a + (b - a) * fraction).round().clamp(-32768.0, 32767.0) as i16);
    }
    encode_wav(&output)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn name_anywhere_with_real_word_boundaries() {
        let names = vec!["Juniper".into()];
        assert!(wake_name("What is the temperature, JUNIPER?", &names).is_some());
        assert!(wake_name("Juniper, tell me a story.", &names).is_some());
        assert!(wake_name("Junipers grow outside", &names).is_none());
    }
    #[test]
    fn preserves_speech_before_name_and_discards_idle_audio() {
        let mut buffer = UtteranceBuffer::new(&Settings::default());
        for _ in 0..2000 {
            assert!(buffer.push(&[0; 320]).is_none());
        }
        assert_eq!(buffer.pre.len(), 8000);
        for _ in 0..30 {
            assert!(buffer.push(&[8000; 320]).is_none());
        }
        let mut captured = None;
        for _ in 0..50 {
            if let Some(audio) = buffer.push(&[0; 320]) {
                captured = Some(audio);
            }
        }
        let captured = captured.unwrap();
        assert_eq!(captured.iter().filter(|s| **s == 8000).count(), 9600);
        assert!(buffer.current.is_empty());
    }
    #[test]
    fn noise_click_and_malformed_frames_do_not_form_commands() {
        let mut buffer = UtteranceBuffer::new(&Settings::default());
        buffer.push(&[9000; 320]);
        for _ in 0..100 {
            assert!(buffer.push(&[0; 320]).is_none());
        }
        assert!(pcm_from_bytes(&[1]).is_err());
        assert!(pcm_from_bytes(&vec![0; 6402]).is_err());
    }
    #[test]
    fn wav_round_trip_preserves_audio() {
        let samples = vec![0, 1000, -1000, 32767, -32768];
        let encoded = encode_wav(&samples).unwrap();
        let normalized = normalize_wav(&encoded).unwrap();
        let actual: Vec<i16> = hound::WavReader::new(Cursor::new(normalized))
            .unwrap()
            .into_samples()
            .map(Result::unwrap)
            .collect();
        assert_eq!(actual, samples);
    }
}
