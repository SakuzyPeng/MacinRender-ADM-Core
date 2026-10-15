//! PCM consistency 2.4: the production meter against an independent BS.1770-4 reference.
//!
//! The reference below shares no code with ebur128: it filters with the 48 kHz K-weighting
//! coefficients published in ITU-R BS.1770-4, forms 400 ms blocks on a 100 ms grid and applies
//! the -70 LUFS absolute and -10 LU relative gates directly. Programmes follow the synthetic
//! signals of EBU Tech 3341 (1 kHz sines at fixed levels, gated sequences, windowed bursts and
//! true-peak sines); expected values are derived from the signal, not read from the meter.
use mradm_dsp::meter::{Meter, MeterChannel, MeterMode};
use std::f64::consts::{FRAC_PI_4, TAU};

const RATES: [u32; 4] = [44_100, 48_000, 96_000, 192_000];

/// BS.1770-4 Table 1 and Table 2 (48 kHz): pre-filter and RLB high-pass, a0 = 1.
const PRE_FILTER: ([f64; 3], [f64; 2]) = (
    [
        1.535_124_859_586_97,
        -2.691_696_189_406_38,
        1.198_392_810_852_85,
    ],
    [-1.690_659_293_182_41, 0.732_480_774_215_85],
);
const RLB_FILTER: ([f64; 3], [f64; 2]) = (
    [1.0, -2.0, 1.0],
    [-1.990_047_454_833_98, 0.990_072_250_366_21],
);

fn loudness(energy: f64) -> f64 {
    -0.691 + 10.0 * energy.log10()
}

fn decibels(linear: f64) -> f64 {
    20.0 * linear.log10()
}

/// Per-frame weighted K-weighted power at 48 kHz.
fn reference_power(samples: &[f32], weights: &[f64]) -> Vec<f64> {
    let channels = weights.len();
    let frames = samples.len() / channels;
    let mut power = vec![0.0; frames];
    for (channel, &weight) in weights.iter().enumerate() {
        if weight == 0.0 {
            continue;
        }
        let mut state = [[0.0f64; 4]; 2];
        for (frame, total) in power.iter_mut().enumerate() {
            let mut value = f64::from(samples[frame * channels + channel]);
            for ((b, a), s) in [PRE_FILTER, RLB_FILTER].iter().zip(&mut state) {
                let out = b[0] * value + b[1] * s[0] + b[2] * s[1] - a[0] * s[2] - a[1] * s[3];
                *s = [value, s[0], out, s[2]];
                value = out;
            }
            *total += weight * value * value;
        }
    }
    power
}

fn window(power: &[f64], end: usize, length: usize) -> f64 {
    power[end - length..end].iter().sum::<f64>() / length as f64
}

struct Reading {
    integrated: f64,
    momentary: f64,
    short_term: f64,
}

fn reference(samples: &[f32], weights: &[f64]) -> Reading {
    let power = reference_power(samples, weights);
    let step = 4_800;
    let blocks: Vec<f64> = (0..)
        .map(|block| block * step + 4 * step)
        .take_while(|&end| end <= power.len())
        .map(|end| window(&power, end, 4 * step))
        .collect();
    let absolute = 10f64.powf((-70.0 + 0.691) / 10.0);
    let gated: Vec<f64> = blocks.into_iter().filter(|&e| e >= absolute).collect();
    let integrated = if gated.is_empty() {
        f64::NEG_INFINITY
    } else {
        let relative = gated.iter().sum::<f64>() / gated.len() as f64 * 0.1;
        let kept: Vec<f64> = gated.into_iter().filter(|&e| e >= relative).collect();
        loudness(kept.iter().sum::<f64>() / kept.len() as f64)
    };
    Reading {
        integrated,
        momentary: loudness(window(&power, power.len(), 19_200)),
        short_term: loudness(window(&power, power.len(), 144_000)),
    }
}

fn measure(samples: &[f32], rate: u32, map: &[MeterChannel], chunk: usize) -> (Reading, Vec<f64>) {
    let channels = if map.is_empty() { 2 } else { map.len() as u32 };
    let mut meter = Meter::new(channels, rate, MeterMode::Monitor, map).unwrap();
    let mut peak = Meter::new(channels, rate, MeterMode::TruePeak, map).unwrap();
    for part in samples.chunks(chunk * channels as usize) {
        meter.add_frames(part).unwrap();
        peak.add_frames(part).unwrap();
    }
    let peaks = (0..channels).map(|c| peak.true_peak(c).unwrap()).collect();
    (
        Reading {
            integrated: meter.integrated().unwrap(),
            momentary: meter.momentary().unwrap(),
            short_term: meter.shortterm().unwrap(),
        },
        peaks,
    )
}

/// 1 kHz sine with continuous phase; `levels` gives (seconds, peak dBFS per channel).
fn sines(rate: u32, levels: &[(f64, &[f64])]) -> Vec<f32> {
    let mut samples = Vec::new();
    let mut frame = 0u64;
    for &(seconds, gains) in levels {
        let end = frame + (seconds * f64::from(rate)).round() as u64;
        while frame < end {
            let phase = TAU * 1000.0 * frame as f64 / f64::from(rate);
            for gain in gains {
                samples.push((10f64.powf(gain / 20.0) * phase.sin()) as f32);
            }
            frame += 1;
        }
    }
    samples
}

fn stereo(seconds: f64, dbfs: &'static [f64; 2]) -> (f64, &'static [f64]) {
    (seconds, dbfs)
}

const M23: [f64; 2] = [-23.0, -23.0];
const M33: [f64; 2] = [-33.0, -33.0];
const M36: [f64; 2] = [-36.0, -36.0];
const M72: [f64; 2] = [-72.0, -72.0];
const M26: [f64; 2] = [-26.0, -26.0];
const M20: [f64; 2] = [-20.0, -20.0];
const M30: [f64; 2] = [-30.0, -30.0];

/// Name, expected integrated loudness and (seconds, peak dBFS per channel) segments.
type Programme = (&'static str, f64, Vec<(f64, &'static [f64])>);

/// Stationary and gated programmes whose integrated loudness is -23 LUFS (-33 for the second).
fn gated_programmes() -> Vec<Programme> {
    vec![
        ("-23 dBFS", -23.0, vec![stereo(20.0, &M23)]),
        ("-33 dBFS", -33.0, vec![stereo(20.0, &M33)]),
        (
            "-36/-23/-36",
            -23.0,
            vec![stereo(10.0, &M36), stereo(60.0, &M23), stereo(10.0, &M36)],
        ),
        (
            "-72/-36/-23/-36/-72",
            -23.0,
            vec![
                stereo(10.0, &M72),
                stereo(10.0, &M36),
                stereo(60.0, &M23),
                stereo(10.0, &M36),
                stereo(10.0, &M72),
            ],
        ),
        (
            "-26/-20/-26",
            -23.0,
            vec![stereo(20.0, &M26), stereo(20.1, &M20), stereo(20.0, &M26)],
        ),
    ]
}

const FIVE: [MeterChannel; 5] = [
    MeterChannel::Left,
    MeterChannel::Right,
    MeterChannel::Center,
    MeterChannel::LeftSurround,
    MeterChannel::RightSurround,
];
const FIVE_WEIGHTS: [f64; 5] = [1.0, 1.0, 1.0, 1.41, 1.41];

#[test]
fn integrated_loudness_matches_the_reference_at_48_khz() {
    let mut programmes = gated_programmes();
    programmes.push((
        "5.0",
        -23.0,
        vec![(20.0, &[-28.0, -28.0, -24.0, -30.0, -30.0][..])],
    ));
    for (name, expected, levels) in programmes {
        let samples = sines(48_000, &levels);
        let (map, weights): (&[MeterChannel], &[f64]) = if levels[0].1.len() == 5 {
            (&FIVE, &FIVE_WEIGHTS)
        } else {
            (&[], &[1.0, 1.0])
        };
        let (meter, _) = measure(&samples, 48_000, map, 4_800);
        let truth = reference(&samples, weights);
        for (what, actual, wanted) in [
            ("integrated", meter.integrated, truth.integrated),
            ("momentary", meter.momentary, truth.momentary),
            ("short-term", meter.short_term, truth.short_term),
        ] {
            assert!(
                (actual - wanted).abs() < 1e-9,
                "{name} {what}: meter {actual} reference {wanted}"
            );
        }
        // EBU Tech 3341 tolerance for these signals.
        assert!(
            (meter.integrated - expected).abs() < 0.1,
            "{name}: {}",
            meter.integrated
        );
    }
}

#[test]
fn integrated_loudness_holds_at_every_supported_rate() {
    for rate in RATES {
        for (name, expected, levels) in gated_programmes() {
            let (meter, _) = measure(&sines(rate, &levels), rate, &[], 4_800);
            assert!(
                (meter.integrated - expected).abs() < 0.1,
                "{rate} Hz {name}: {}",
                meter.integrated
            );
        }
    }
}

/// Absolute and relative gates around their thresholds. Each programme alternates a level that
/// must be kept with one that must be gated; the meter has to take the same decisions as the
/// reference, which is checked through the integrated value.
#[test]
fn gate_boundaries_match_the_reference() {
    // A 1 kHz stereo sine at x dBFS reads x LUFS (the K-weighting gain at 1 kHz is +0.69 dB).
    let near = |a: f64, b: f64| -> Vec<f32> {
        let mut levels = Vec::new();
        for _ in 0..6 {
            levels.push((2.0, vec![a, a]));
            levels.push((2.0, vec![b, b]));
        }
        let borrowed: Vec<(f64, &[f64])> = levels.iter().map(|(s, g)| (*s, g.as_slice())).collect();
        sines(48_000, &borrowed)
    };
    for (a, b) in [
        (-69.9, -70.1),
        (-69.98, -70.02),
        (-20.0, -29.9),
        (-20.0, -30.1),
        (-20.0, -29.98),
        (-20.0, -30.02),
    ] {
        let samples = near(a, b);
        let (meter, _) = measure(&samples, 48_000, &[], 4_800);
        let truth = reference(&samples, &[1.0, 1.0]);
        assert!(
            (meter.integrated - truth.integrated).abs() < 1e-9,
            "{a}/{b}: meter {} reference {}",
            meter.integrated,
            truth.integrated
        );
    }
    // Entirely below the absolute gate: no block is kept.
    let quiet = sines(48_000, &[(5.0, &[-70.5, -70.5][..])]);
    assert_eq!(
        measure(&quiet, 48_000, &[], 4_800).0.integrated,
        f64::NEG_INFINITY
    );
}

/// Bursts whose period equals the window: the windowed loudness stays at -23 LUFS once the
/// window is full, wherever it is read.
#[test]
fn momentary_and_short_term_windows() {
    for rate in RATES {
        let mut short = Vec::new();
        for _ in 0..5 {
            short.extend([stereo(1.34, &M20), stereo(1.66, &M30)]);
        }
        let mut momentary = Vec::new();
        for _ in 0..25 {
            momentary.extend([stereo(0.18, &M20), stereo(0.22, &M30)]);
        }
        let short = sines(rate, &short);
        let momentary = sines(rate, &momentary);
        let second = 2 * rate as usize;
        let mut meter = Meter::new(2, rate, MeterMode::Monitor, &[]).unwrap();
        let mut read = Vec::new();
        // Read every 10 ms after the first period.
        for (index, part) in short.chunks(second / 100).enumerate() {
            meter.add_frames(part).unwrap();
            if index >= 300 {
                read.push(meter.shortterm().unwrap());
            }
        }
        assert!(
            read.iter().all(|s| (s + 23.0).abs() < 0.1),
            "{rate} Hz short-term {read:?}"
        );
        meter.reset();
        read.clear();
        for (index, part) in momentary.chunks(second / 100).enumerate() {
            meter.add_frames(part).unwrap();
            if index >= 40 {
                read.push(meter.momentary().unwrap());
            }
        }
        assert!(
            read.iter().all(|m| (m + 23.0).abs() < 0.1),
            "{rate} Hz momentary {read:?}"
        );
    }
}

fn tone(rate: u32, frequency: f64, phase: f64, amplitude: f64, seconds: f64) -> Vec<f32> {
    let frames = (seconds * f64::from(rate)) as usize;
    (0..frames)
        .flat_map(|n| {
            let value = amplitude * (TAU * frequency * n as f64 / f64::from(rate) + phase).sin();
            [value as f32, value as f32]
        })
        .collect()
}

/// Inter-sample peaks of sines whose samples miss the crest. The true peak of a sine is its
/// amplitude; 4x (44.1/48 kHz) and 2x (96 kHz) oversampling must read it within the Tech 3341
/// tolerance of +0.2/-0.4 dB. At 192 kHz ebur128 does not oversample and reports the sample peak.
#[test]
fn true_peak_of_intersample_sines() {
    for rate in RATES {
        let fs = f64::from(rate);
        let mut cases = vec![
            // Quarter-rate sine sampled 45 degrees off the crest: sample peak 3.01 dB low.
            (fs / 4.0, FRAC_PI_4, 0.5),
            (fs / 4.0, FRAC_PI_4, 1.0),
            (fs / 4.0, FRAC_PI_4, 1.0 / std::f64::consts::SQRT_2),
            (fs / 4.0, 0.0, 0.5),
            (997.0, 0.3, 0.9),
        ];
        // Audio-band sines near 20 kHz.
        cases.extend([(19_997.0, 0.1, 0.5), (15_013.0, 0.7, 0.5)]);
        for (frequency, phase, amplitude) in cases {
            if frequency >= fs / 2.0 {
                continue;
            }
            let samples = tone(rate, frequency, phase, amplitude, 1.0);
            let (_, peaks) = measure(&samples, rate, &[], 4_800);
            let sample_peak = samples
                .iter()
                .map(|s| f64::from(s.abs()))
                .fold(0.0, f64::max);
            let error = decibels(peaks[0]) - decibels(amplitude);
            assert_eq!(peaks[0], peaks[1]);
            assert!(
                peaks[0] >= sample_peak,
                "{rate} Hz {frequency} Hz below sample peak"
            );
            if rate == 192_000 {
                assert_eq!(peaks[0], sample_peak, "{frequency} Hz at 192 kHz");
            } else {
                assert!(
                    (-0.4..=0.2).contains(&error),
                    "{rate} Hz {frequency} Hz phase {phase}: {error} dB"
                );
            }
        }
    }
}

/// Noise, a sine, digital silence, a sine at -60 dBFS resuming from cleared filter state, then
/// silence again.
fn mixed_programme(rate: u32) -> Vec<f32> {
    let mut state = 0x2545_f491_4f6c_dd1du64;
    let tenth = rate as usize / 10;
    let mut samples = Vec::with_capacity(100 * tenth);
    for frame in 0..50 * tenth {
        let phase = TAU * 3_000.0 * frame as f64 / f64::from(rate);
        for channel in 0..2 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let noise = (state >> 40) as f32 / (1u32 << 24) as f32 - 0.5;
            let sine = (phase + f64::from(channel)).sin();
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

fn bits(meter: &Meter, peak: &Meter) -> Vec<u64> {
    let mut out = vec![
        meter.integrated().unwrap().to_bits(),
        meter.momentary().unwrap().to_bits(),
        meter.shortterm().unwrap().to_bits(),
    ];
    out.extend((0..2).map(|c| peak.true_peak(c).unwrap().to_bits()));
    out
}

/// The renderer feeds the meter one render block at a time, so readings must not depend on the
/// block size. Above f64::EPSILON every chunking takes the same arithmetic path; reset returns a
/// used meter to the state of a new one.
#[test]
fn chunking_and_reset_are_bit_exact() {
    for rate in RATES {
        let samples = mixed_programme(rate);
        let mut expected = None;
        for pattern in [
            vec![usize::MAX],
            vec![1],
            vec![7],
            vec![127],
            vec![4_800],
            vec![1, 4_799, 333, 10_007],
        ] {
            let mut meter = Meter::new(2, rate, MeterMode::Monitor, &[]).unwrap();
            let mut peak = Meter::new(2, rate, MeterMode::IntegratedTruePeak, &[]).unwrap();
            // Reset after unrelated input must leave no trace.
            let other = tone(rate, 440.0, 0.0, 0.9, 0.7);
            meter.add_frames(&other).unwrap();
            peak.add_frames(&other).unwrap();
            meter.reset();
            peak.reset();
            let mut offset = 0;
            for &size in pattern.iter().cycle() {
                if offset == samples.len() {
                    break;
                }
                let end = samples
                    .len()
                    .min(offset.saturating_add(size.saturating_mul(2)));
                meter.add_frames(&samples[offset..end]).unwrap();
                peak.add_frames(&samples[offset..end]).unwrap();
                offset = end;
            }
            let readings = bits(&meter, &peak);
            match &expected {
                None => expected = Some(readings),
                Some(first) => assert_eq!(&readings, first, "{rate} Hz chunks {pattern:?}"),
            }
        }
    }
}

/// Long digital silence after programme: silent blocks fall under the absolute gate, the
/// K-weighting state decays below f64::EPSILON and is cleared, momentary and short-term return
/// to -inf and integrated loudness and true peak keep their values bit for bit.
#[test]
fn long_silence_keeps_the_programme_reading() {
    let rate = 48_000;
    let programme = mixed_programme(rate);
    let mut meter = Meter::new(2, rate, MeterMode::Monitor, &[]).unwrap();
    let mut peak = Meter::new(2, rate, MeterMode::IntegratedTruePeak, &[]).unwrap();
    meter.add_frames(&programme).unwrap();
    peak.add_frames(&programme).unwrap();
    let silence = vec![0.0f32; 2 * rate as usize];
    let mut early = None;
    for second in 1..=600 {
        meter.add_frames(&silence).unwrap();
        peak.add_frames(&silence).unwrap();
        if second == 4 {
            early = Some(bits(&meter, &peak));
        }
    }
    let after = bits(&meter, &peak);
    let early = early.unwrap();
    assert_eq!(
        after[0], early[0],
        "integrated after ten minutes of silence"
    );
    assert_eq!(
        after[3..],
        early[3..],
        "true peak after ten minutes of silence"
    );
    assert_eq!(meter.momentary().unwrap(), f64::NEG_INFINITY);
    assert_eq!(meter.shortterm().unwrap(), f64::NEG_INFINITY);
    assert!(meter.integrated().unwrap().is_finite());
}
