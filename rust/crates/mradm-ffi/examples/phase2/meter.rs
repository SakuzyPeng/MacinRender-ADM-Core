//! Production meter on exact dyadic input: true peak (gated) and loudness (observed).
use super::{doubles, integers, signal};
use mradm_dsp::meter::{Meter, MeterMode};
use std::path::Path;

/// 4x oversampling below 96 kHz, 2x below 192 kHz, sample peak above.
pub(super) const RATES: [u32; 4] = [44100, 48000, 96000, 192000];
/// Stereo uses a specialized scanner; twelve channels take the generic path.
pub(super) const CHANNELS: [u32; 2] = [2, 12];
const CHUNK: usize = 4800;

/// One second of noise peaking at 0.25, half a second 24 dB lower, half a second of silence.
/// Channel 0 opens with the fs/4 sequence 0.5, 0.5, -0.5, -0.5 whose true peak is 0.5·√2,
/// well above the noise, so its peak exercises the interpolation filter.
fn programme(rate: u32, channels: u32) -> Vec<f32> {
    let (loud, quiet) = (rate as usize, rate as usize / 2);
    let frames = loud + 2 * quiet;
    let channels = channels as usize;
    let noise = signal(frames * channels);
    let mut samples = vec![0f32; frames * channels];
    for frame in 0..loud + quiet {
        let scale = if frame < loud { 0.25 } else { 0.015625 };
        for channel in 0..channels {
            let index = frame * channels + channel;
            samples[index] = if channel == 0 && frame < loud / 2 {
                if frame % 4 < 2 { 0.5 } else { -0.5 }
            } else {
                noise[index] * scale
            };
        }
    }
    samples
}

pub(super) fn run(out: &Path) {
    for rate in RATES {
        for channels in CHANNELS {
            let prefix = format!("meter-{rate}-{channels}");
            let samples = programme(rate, channels);
            integers(
                out,
                &format!("{prefix}.10-parameters.i32"),
                &[
                    rate as i32,
                    channels as i32,
                    (samples.len() / channels as usize) as i32,
                    CHUNK as i32,
                ],
            );
            let mut offline =
                Meter::new(channels, rate, MeterMode::IntegratedTruePeak, &[]).unwrap();
            let mut monitor = Meter::new(channels, rate, MeterMode::Monitor, &[]).unwrap();
            // Momentary and short-term are read at the end of the loud second; the trailing
            // silence would make them -inf.
            let (loud, rest) = samples.split_at(rate as usize * channels as usize);
            let mut window = Vec::new();
            for part in [loud, rest] {
                for chunk in part.chunks(CHUNK * channels as usize) {
                    offline.add_frames(chunk).unwrap();
                    monitor.add_frames(chunk).unwrap();
                }
                if window.is_empty() {
                    window = vec![monitor.momentary().unwrap(), monitor.shortterm().unwrap()];
                }
            }
            let peaks: Vec<f64> = (0..channels)
                .map(|channel| offline.true_peak(channel).unwrap())
                .collect();
            doubles(out, &format!("{prefix}.20-true-peak.f64"), &peaks);
            window.extend([offline.integrated().unwrap(), monitor.integrated().unwrap()]);
            doubles(out, &format!("{prefix}.30-loudness.f64"), &window);
        }
    }
}
