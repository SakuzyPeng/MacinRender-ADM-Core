//! Meter acceptance programmes (PCM consistency 2.4): gate boundaries, inter-sample peaks, long
//! silence, chunking and reset. Sines come from the libm port with exactly reduced phases, so
//! the input is the same on every platform; the independent accuracy checks live in
//! mradm-dsp's tests/meter_acceptance.rs.
use super::{doubles, meter::RATES};
use mradm_dsp::meter::{Meter, MeterMode};
use std::f64::consts::{FRAC_1_SQRT_2, FRAC_PI_4, TAU};
use std::path::Path;

/// Stereo sine with phase `TAU * (cycles * frame mod period) / period + offset`.
fn sine(frames: usize, cycles: u64, period: u64, offset: f64, amplitude: f64) -> Vec<f32> {
    (0..frames as u64)
        .flat_map(|frame| {
            let phase = TAU * ((cycles * frame) % period) as f64 / period as f64 + offset;
            let value = (amplitude * libm::sin(phase)) as f32;
            [value, value]
        })
        .collect()
}

fn decibel_amplitude(dbfs: f64) -> f64 {
    libm::pow(10.0, dbfs / 20.0)
}

/// 1 kHz stereo sine alternating two levels, one second each, three times.
fn alternating(rate: u32, levels: [f64; 2]) -> Vec<f32> {
    let mut samples = Vec::new();
    for _ in 0..3 {
        for dbfs in levels {
            samples.extend(sine(
                rate as usize,
                1000,
                u64::from(rate),
                0.0,
                decibel_amplitude(dbfs),
            ));
        }
    }
    samples
}

/// Noise, a sine, digital silence, a sine at -60 dBFS resuming from cleared filter state, then
/// silence again (five seconds).
fn programme(rate: u32) -> Vec<f32> {
    let mut state = 0x2545_f491_4f6c_dd1du64;
    let tenth = rate as usize / 10;
    let mut samples = Vec::with_capacity(100 * tenth);
    for frame in 0..50 * tenth {
        let phase = TAU * ((3000 * frame as u64) % u64::from(rate)) as f64 / f64::from(rate);
        for channel in 0..2 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let noise = (state >> 40) as f32 / (1u32 << 24) as f32 - 0.5;
            let sine = libm::sin(phase + f64::from(channel));
            samples.push(match frame / tenth {
                0..10 => 0.3 * noise,
                10..20 => (0.7 * sine) as f32,
                30..35 => (0.001 * sine) as f32,
                _ => 0.0,
            });
        }
    }
    samples
}

/// A Monitor meter and an offline meter (integrated + true peak) fed together.
struct Pair {
    monitor: Meter,
    offline: Meter,
}

impl Pair {
    fn new(rate: u32) -> Self {
        Self {
            monitor: Meter::new(2, rate, MeterMode::Monitor, &[]).unwrap(),
            offline: Meter::new(2, rate, MeterMode::IntegratedTruePeak, &[]).unwrap(),
        }
    }

    fn add(&mut self, samples: &[f32]) {
        self.monitor.add_frames(samples).unwrap();
        self.offline.add_frames(samples).unwrap();
    }

    fn add_chunked(&mut self, samples: &[f32], pattern: &[usize]) {
        let mut offset = 0;
        for &frames in pattern.iter().cycle() {
            if offset == samples.len() {
                break;
            }
            let end = samples.len().min(offset + 2 * frames);
            self.add(&samples[offset..end]);
            offset = end;
        }
    }

    fn reset(&mut self) {
        self.monitor.reset();
        self.offline.reset();
    }

    /// Monitor integrated, momentary and short-term, offline integrated and both true peaks.
    /// Readings of -inf are recorded as -1e300 to keep the files finite.
    fn readings(&self) -> [f64; 6] {
        [
            self.monitor.integrated().unwrap(),
            self.monitor.momentary().unwrap(),
            self.monitor.shortterm().unwrap(),
            self.offline.integrated().unwrap(),
            self.offline.true_peak(0).unwrap(),
            self.offline.true_peak(1).unwrap(),
        ]
        .map(|value| {
            if value == f64::NEG_INFINITY {
                -1e300
            } else {
                value
            }
        })
    }

    fn bits(&self) -> [u64; 6] {
        self.readings().map(f64::to_bits)
    }
}

const CHUNKINGS: [&[usize]; 6] = [
    &[usize::MAX / 4],
    &[1],
    &[7],
    &[127],
    &[4800],
    &[1, 4799, 333, 10_007],
];

pub(super) fn run(out: &Path) {
    for rate in RATES {
        let prefix = format!("meter-acceptance-{rate}");

        // Gates: absolute -70 LUFS and relative -10 LU. Equal-duration high/low segments
        // cross the relative gate at L = 0.1 * (H + L) / 2 = H / 19, about -32.79 dBFS
        // for a -20 dBFS high segment. The independent accuracy test checks both sides.
        let relative_low = -20.0 - 10.0 * libm::log10(19.0);
        let mut gates = Vec::new();
        for levels in [
            [-69.9, -70.1],
            [-69.98, -70.02],
            [-20.0, relative_low + 0.1],
            [-20.0, relative_low - 0.1],
            [-20.0, relative_low + 0.02],
            [-20.0, relative_low - 0.02],
            [-70.5, -70.5],
        ] {
            let mut pair = Pair::new(rate);
            pair.add_chunked(&alternating(rate, levels), &[4800]);
            gates.extend(pair.readings());
        }
        doubles(out, &format!("{prefix}.10-gates.f64"), &gates);

        // Inter-sample peaks: quarter-rate sines off the crest, the exact sequence 1, 1, -1, -1
        // (true peak sqrt 2), a lone full-scale sample, and audio-band sines.
        let quarter = rate as usize / 4;
        let fs = u64::from(rate);
        let mut cases = vec![
            sine(quarter, 1, 4, FRAC_PI_4, 0.5),
            sine(quarter, 1, 4, FRAC_PI_4, 1.0),
            sine(quarter, 1, 4, FRAC_PI_4, FRAC_1_SQRT_2),
            sine(quarter, 1, 4, 0.0, 0.5),
            sine(quarter, 9, 20, 0.3, 0.9),
            sine(quarter, 997, fs, 0.3, 0.9),
            sine(quarter, 15_013, fs, 0.7, 0.5),
            sine(quarter, 19_997, fs, 0.1, 0.5),
        ];
        cases.push(
            (0..quarter)
                .flat_map(|n| {
                    let v = if n % 4 < 2 { 1.0 } else { -1.0 };
                    [v, -v]
                })
                .collect(),
        );
        let mut impulse = vec![0.0f32; 2 * quarter];
        impulse[2 * (quarter / 2)] = 1.0;
        impulse[2 * (quarter / 2) + 3] = -1.0;
        cases.push(impulse);
        let mut peaks = Vec::new();
        for samples in cases {
            let mut pair = Pair::new(rate);
            pair.add_chunked(&samples, &[4800]);
            peaks.extend(&pair.readings()[4..]);
        }
        doubles(out, &format!("{prefix}.20-true-peak.f64"), &peaks);

        // Long silence: after the programme (read after 1 s, 4 s and 60 s), and before it.
        let samples = programme(rate);
        let second = vec![0.0f32; 2 * rate as usize];
        let mut silence = Vec::new();
        let mut pair = Pair::new(rate);
        pair.add_chunked(&samples, &[4800]);
        for elapsed in 1..=60 {
            pair.add_chunked(&second, &[4800]);
            if matches!(elapsed, 1 | 4 | 60) {
                silence.extend(pair.readings());
            }
        }
        let mut pair = Pair::new(rate);
        for _ in 0..60 {
            pair.add_chunked(&second, &[4800]);
        }
        pair.add_chunked(&samples, &[4800]);
        silence.extend(pair.readings());
        doubles(out, &format!("{prefix}.30-long-silence.f64"), &silence);

        // Chunking: the same programme split six ways.
        let mut fresh = Pair::new(rate);
        let empty = fresh.bits();
        fresh.add_chunked(&samples, &[4800]);
        let expected = fresh.bits();
        let mut chunking = Vec::new();
        for pattern in CHUNKINGS {
            let mut pair = Pair::new(rate);
            pair.add_chunked(&samples, pattern);
            assert_eq!(pair.bits(), expected, "{prefix} chunking {pattern:?}");
            chunking.extend(pair.readings());
        }
        doubles(out, &format!("{prefix}.40-chunking.f64"), &chunking);

        // Reset: after unrelated input, and in the middle of the programme (as on a seek).
        let mut reset = Vec::new();
        let mut pair = Pair::new(rate);
        pair.add(&sine(rate as usize, 440, fs, 0.0, 0.9));
        pair.reset();
        assert_eq!(pair.bits(), empty, "{prefix} empty reset");
        pair.add_chunked(&samples, &[4800]);
        assert_eq!(pair.bits(), expected, "{prefix} reset versus fresh");
        reset.extend(pair.readings());
        let mut pair = Pair::new(rate);
        let (head, tail) = samples.split_at(2 * (rate as usize * 2 + rate as usize / 20));
        pair.add_chunked(head, &[4800]);
        reset.extend(pair.readings());
        pair.reset();
        assert_eq!(pair.bits(), empty, "{prefix} empty seek reset");
        pair.add_chunked(tail, &[4800]);
        let mut fresh = Pair::new(rate);
        fresh.add_chunked(tail, &[4800]);
        assert_eq!(
            pair.bits(),
            fresh.bits(),
            "{prefix} seek reset versus fresh tail"
        );
        reset.extend(pair.readings());
        doubles(out, &format!("{prefix}.50-reset.f64"), &reset);
    }
}
