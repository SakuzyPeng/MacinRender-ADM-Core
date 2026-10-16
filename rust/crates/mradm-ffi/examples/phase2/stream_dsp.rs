//! File-streaming and monitor arithmetic on fixed inputs (PCM consistency 3.1).
use super::{floats, signal};
use mradm_dsp::{monitor, stream_dsp};
use std::path::Path;

pub(super) fn run(out: &Path) {
    // Spreader cone over extents around the 0..1 normalised range and in degrees, with depth.
    let extents = [-0.5f32, 0.0, 0.125, 0.25, 0.5, 0.75, 1.0, 10.0, 30.0, 360.0];
    let mut spread = Vec::new();
    for &width in &extents {
        for &height in &extents {
            for depth in [0.0f32, 0.125, 0.5, 2.0] {
                spread.push(stream_dsp::extent_spread_degrees(width, height, depth));
            }
        }
    }
    floats(out, "stream-dsp.10-spread.f32", &spread);

    // Stereo fold of every 7.5 degrees plus the BS.2051 loudspeaker azimuths.
    let mut azimuths: Vec<f32> = (-24..=24).map(|step| step as f32 * 7.5).collect();
    azimuths.extend([
        22.5, -22.5, 30.0, -30.0, 45.0, -45.0, 60.0, -60.0, 110.0, -110.0, 135.0, -135.0, 150.0,
        -150.0, 1.0e-3, -1.0e-3, 89.99, -89.99,
    ]);
    let fold: Vec<f32> = azimuths
        .iter()
        .flat_map(|&az| monitor::stereo_fold(az))
        .collect();
    floats(out, "stream-dsp.20-fold.f32", &fold);

    // Live override gains across the UI range in 0.05 dB steps and at the extremes.
    let mut decibels: Vec<f32> = (-1200..=240).map(|step| step as f32 * 0.05).collect();
    decibels.extend([-120.0, -96.0, -0.001, 0.001, 23.99]);
    let gains: Vec<f32> = decibels
        .iter()
        .map(|&db| stream_dsp::live_gain(db))
        .collect();
    floats(out, "stream-dsp.30-live-gain.f32", &gains);

    // Weighted stereo sum of three sources, then a topology crossfade into a second sum.
    const FRAMES: usize = 512;
    let noise = signal(6 * FRAMES);
    let sources: Vec<&[f32]> = noise
        .as_chunks::<FRAMES>()
        .0
        .iter()
        .map(|chunk| &chunk[..])
        .collect();
    let ramp: Vec<f32> = (0..FRAMES).map(|f| 1.0 - f as f32 / 600.0).collect();
    let mut outgoing = vec![0.0f32; 2 * FRAMES];
    let mut incoming = vec![0.0f32; 2 * FRAMES];
    for pair in sources.chunks(2) {
        stream_dsp::accumulate_stereo(&mut outgoing, pair[0], pair[1], &ramp, FRAMES).unwrap();
    }
    for (index, pair) in sources.chunks(2).enumerate() {
        let gain = vec![0.25 + index as f32 * 0.3; FRAMES];
        stream_dsp::accumulate_stereo(&mut incoming, pair[1], pair[0], &gain, FRAMES).unwrap();
    }
    let mut mix = outgoing.clone();
    mix.extend(&incoming);
    stream_dsp::topology_crossfade(&outgoing, &mut incoming, FRAMES).unwrap();
    mix.extend(&incoming);
    floats(out, "stream-dsp.40-mix.f32", &mix);
}
