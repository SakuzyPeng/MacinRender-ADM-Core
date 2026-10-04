//! Opt-in suitability probe. C is linked only into this evaluator, never Rust DSP.
mod reference;

use ebur128::{Channel, EbuR128, Mode};
use std::{
    alloc::{GlobalAlloc, Layout, System},
    cell::Cell,
    f64::consts::TAU,
    hint::black_box,
    time::Instant,
};

// Counts only Rust allocations on this thread during an explicitly armed scope.
// C malloc() is NOT intercepted. These numbers are not a comparison of RSS.
#[derive(Clone, Copy, Default)]
struct Allocations {
    calls: usize,
    reallocations: usize,
    requested_bytes: usize,
}
thread_local! { static ALLOCATIONS: Cell<Option<Allocations>> = const { Cell::new(None) }; }
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let _ = ALLOCATIONS.try_with(|slot| {
            if let Some(mut count) = slot.get() {
                count.calls += 1;
                count.requested_bytes += layout.size();
                slot.set(Some(count));
            }
        });
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) };
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let _ = ALLOCATIONS.try_with(|slot| {
            if let Some(mut count) = slot.get() {
                count.reallocations += 1;
                count.requested_bytes += size;
                slot.set(Some(count));
            }
        });
        unsafe { System.realloc(pointer, layout, size) }
    }
}
#[global_allocator]
static ALLOCATOR: Counting = Counting;

fn counted(f: impl FnOnce()) -> Allocations {
    ALLOCATIONS.with(|slot| slot.set(Some(Allocations::default())));
    f();
    ALLOCATIONS.with(|slot| slot.replace(None).unwrap())
}

const HOA_MAP: [Channel; 12] = [
    Channel::Left,
    Channel::Right,
    Channel::Center,
    Channel::Unused,
    Channel::Mp090,
    Channel::Mm090,
    Channel::Mp135,
    Channel::Mm135,
    Channel::Up045,
    Channel::Um045,
    Channel::Up135,
    Channel::Um135,
];

fn meter(channels: u32, rate: u32, mode: Mode, map: Option<&[Channel]>) -> EbuR128 {
    let mut result = EbuR128::new(channels, rate, mode).unwrap();
    if let Some(map) = map {
        result.set_channel_map(map).unwrap();
    }
    result
}

fn loudness(meter: &EbuR128, kind: usize) -> f64 {
    match kind {
        0 => meter.loudness_global(),
        1 => meter.loudness_momentary(),
        2 => meter.loudness_shortterm(),
        _ => unreachable!(),
    }
    .unwrap()
}

fn error(a: f64, b: f64) -> f64 {
    if a == b {
        return 0.0;
    }
    assert!(a.is_finite() && b.is_finite(), "finite mismatch: {a} / {b}");
    (a - b).abs()
}

fn db(amplitude: f64) -> f64 {
    20.0 * amplitude.log10()
}

fn loudness_error(a: f64, b: f64, numerical_silences: &mut usize) -> f64 {
    // Rust flushes tiny filter state to zero earlier than the C reference.
    // Report this separately instead of letting a meaningless -600 LUFS tail
    // hide active-signal errors. This floor is far below R128's -70 LUFS gate.
    if a != b && a < -300.0 && b < -300.0 {
        *numerical_silences += 1;
        0.0
    } else {
        error(a, b)
    }
}

// Deterministic input bytes shared by C and Rust in this process. No random crate.
fn signal(rate: u32, channels: u32, seconds: f64, pattern: &str) -> Vec<f32> {
    let frames = (seconds * f64::from(rate)) as usize;
    let mut result = vec![0.0; frames * channels as usize];
    let mut rng = 0x3141_5926u32;
    for (frame, values) in result.chunks_exact_mut(channels as usize).enumerate() {
        let time = frame as f64 / f64::from(rate);
        for (channel, value) in values.iter_mut().enumerate() {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            let noise = f64::from(rng) / f64::from(u32::MAX) - 0.5;
            let sine = (TAU * (440.0 + 113.0 * channel as f64) * time).sin();
            *value = match pattern {
                "silence" => 0.0,
                "sine" => 0.2 * sine,
                "mixed" => 0.17 * sine + 0.11 * noise,
                "gated" => {
                    let level = [0.0, 0.00001, 0.2, 0.008, 0.08, 0.0][time as usize % 6];
                    level * sine
                }
                "intersample" => {
                    0.9 * (TAU * 0.25 * frame as f64 + std::f64::consts::FRAC_PI_4).sin()
                }
                "edge_impulses" => {
                    if frame == 0 || frame + 1 == frames || frame % 997 == 0 {
                        0.99
                    } else {
                        0.0
                    }
                }
                _ => unreachable!(),
            } as f32;
        }
    }
    result
}

fn compare_case(
    name: &str,
    samples: &[f32],
    rate: u32,
    channels: u32,
    mode: Mode,
    map: Option<&[Channel]>,
) {
    let mut rust = meter(channels, rate, mode, map);
    let mut c = reference::Meter::new(channels, rate, mode, map);
    let chunks = [1, 127, 512, 4096, 13, 997];
    let n = channels as usize;
    let mut offset = 0;
    let mut block = 0;
    let mut next_query = (rate as usize / 10).max(1);
    let mut max_lu: f64 = 0.0;
    let mut max_tp: f64 = 0.0;
    let mut queries = 0;
    let mut numerical_silences = 0;
    if samples.is_empty() {
        for (kind, required) in [(0, Mode::I), (1, Mode::M), (2, Mode::S)] {
            if mode.contains(required) {
                assert_eq!(loudness(&rust, kind), f64::NEG_INFINITY);
                assert_eq!(c.loudness(kind), f64::NEG_INFINITY);
            }
        }
        assert_eq!(rust.true_peak(0).unwrap(), 0.0);
        assert_eq!(c.peak(0), 0.0);
        queries += 1;
    }
    while offset < samples.len() / n {
        let count = chunks[block % chunks.len()]
            .min(samples.len() / n - offset)
            .min(next_query - offset);
        let input = &samples[offset * n..(offset + count) * n];
        rust.add_frames_f32(input).unwrap();
        c.add(input);
        offset += count;
        block += 1;
        if offset == next_query || offset == samples.len() / n {
            for (kind, required) in [(0, Mode::I), (1, Mode::M), (2, Mode::S)] {
                if mode.contains(required) {
                    max_lu = max_lu.max(loudness_error(
                        loudness(&rust, kind),
                        c.loudness(kind),
                        &mut numerical_silences,
                    ));
                }
            }
            if mode.contains(Mode::TRUE_PEAK) {
                for channel in 0..channels {
                    max_tp = max_tp.max(error(
                        db(rust.true_peak(channel).unwrap()),
                        db(c.peak(channel)),
                    ));
                }
            }
            queries += 1;
            next_query += (rate as usize / 10).max(1);
        }
    }
    println!(
        "{{\"kind\":\"comparison\",\"name\":{name:?},\"rate\":{rate},\"channels\":{channels},\"mode\":{},\"queries\":{queries},\"numerical_silence_differences\":{numerical_silences},\"max_lufs_error\":{max_lu:e},\"max_true_peak_db_error\":{max_tp:e}}}",
        mode.bits()
    );
    assert!(max_lu < 0.001, "{name}: LUFS error {max_lu}");
    assert!(max_tp < 0.01, "{name}: True Peak error {max_tp}");
}

fn analytic_checks() {
    // 1 kHz mono at -20 dBFS peak should measure -23 LUFS within 0.1 LU.
    // This is an independent signal/expected-level check, not an assertion that
    // the complete official EBU compliance files have been run.
    let samples: Vec<f32> = (0..240_000)
        .map(|frame| (0.1 * (TAU * 1000.0 * f64::from(frame) / 48_000.0).sin()) as f32)
        .collect();
    let mut rust = meter(1, 48_000, Mode::S | Mode::I | Mode::TRUE_PEAK, None);
    rust.add_frames_f32(&samples).unwrap();
    let integrated = rust.loudness_global().unwrap();
    assert!((integrated + 23.0).abs() < 0.1);
    assert!((rust.loudness_momentary().unwrap() + 23.0).abs() < 0.1);
    assert!((rust.loudness_shortterm().unwrap() + 23.0).abs() < 0.1);
    assert!((db(rust.true_peak(0).unwrap()) + 20.0).abs() < 0.1);

    let shifted = signal(48_000, 1, 1.0, "intersample");
    let mut peak = meter(1, 48_000, Mode::TRUE_PEAK, None);
    peak.add_frames_f32(&shifted).unwrap();
    let sample_db = db(peak.sample_peak(0).unwrap());
    let true_db = db(peak.true_peak(0).unwrap());
    assert!(true_db - sample_db > 2.5);
    assert!((true_db - db(0.9)).abs() < 0.4);

    let mut short = meter(1, 48_000, Mode::I, None);
    short.add_frames_f32(&samples[..19_199]).unwrap();
    assert_eq!(short.loudness_global().unwrap(), f64::NEG_INFINITY);
    short.add_frames_f32(&samples[19_199..19_200]).unwrap();
    assert!(short.loudness_global().unwrap().is_finite());
    println!(
        "{{\"kind\":\"analytic_checks\",\"mono_1khz_lufs\":{integrated},\"intersample_sample_dbfs\":{sample_db},\"intersample_true_dbtp\":{true_db},\"passed\":true}}"
    );
}

fn comparisons() {
    assert_eq!(reference::Meter::version(), [1, 2, 6]);
    let mode = Mode::S | Mode::I | Mode::TRUE_PEAK;
    for rate in [44_100, 48_000, 88_200, 96_000, 176_400, 192_000] {
        for channels in [1, 2, 6, 12] {
            let input = signal(rate, channels, 4.13, "mixed");
            compare_case("rate_channel_matrix", &input, rate, channels, mode, None);
        }
    }
    for channels in [4, 5, 8, 16, 24, 64] {
        let input = signal(48_000, channels, 4.13, "mixed");
        compare_case("channel_matrix", &input, 48_000, channels, mode, None);
    }
    for pattern in ["silence", "sine", "gated", "intersample", "edge_impulses"] {
        let input = signal(48_000, 2, 6.13, pattern);
        compare_case(pattern, &input, 48_000, 2, mode, None);
    }
    for mode in [
        Mode::I,
        Mode::TRUE_PEAK,
        Mode::I | Mode::TRUE_PEAK,
        Mode::S | Mode::I,
    ] {
        let input = signal(48_000, 2, 4.13, "mixed");
        compare_case("production_modes", &input, 48_000, 2, mode, None);
    }
    for milliseconds in [0, 1, 399, 400, 401, 2999, 3000, 3001] {
        let input = signal(48_000, 1, f64::from(milliseconds) / 1000.0, "sine");
        compare_case("window_boundaries", &input, 48_000, 1, mode, None);
    }
    let mut input = signal(48_000, 12, 4.13, "mixed");
    compare_case("hoa_714_map", &input, 48_000, 12, mode, Some(&HOA_MAP));
    for frame in input.as_chunks_mut::<12>().0 {
        for (channel, sample) in frame.iter_mut().enumerate() {
            if channel != 3 {
                *sample = 0.0;
            }
        }
    }
    compare_case("lfe_only", &input, 48_000, 12, mode, Some(&HOA_MAP));
    let mut lfe = meter(12, 48_000, mode, Some(&HOA_MAP));
    lfe.add_frames_f32(&input).unwrap();
    assert_eq!(lfe.loudness_global().unwrap(), f64::NEG_INFINITY);
    assert!(lfe.true_peak(3).unwrap() > 0.1);
}

fn state_checks() {
    let mode = Mode::S | Mode::I | Mode::TRUE_PEAK;
    let samples = signal(48_000, 12, 4.13, "mixed");
    let mut full = meter(12, 48_000, mode, Some(&HOA_MAP));
    full.add_frames_f32(&samples).unwrap();
    for frames in [1, 127, 512, 4096] {
        let mut chunked = meter(12, 48_000, mode, Some(&HOA_MAP));
        for chunk in samples.chunks(frames * 12) {
            chunked.add_frames_f32(chunk).unwrap();
        }
        for kind in 0..3 {
            assert!(error(loudness(&full, kind), loudness(&chunked, kind)) < 1e-9);
        }
        for channel in 0..12 {
            assert!(
                error(
                    full.true_peak(channel).unwrap(),
                    chunked.true_peak(channel).unwrap()
                ) < 1e-7
            );
        }
    }
    let reset_allocations = counted(|| full.reset());
    assert_eq!(reset_allocations.calls + reset_allocations.reallocations, 0);
    assert_eq!(full.channel_map(), HOA_MAP);
    assert_eq!(full.loudness_global().unwrap(), f64::NEG_INFINITY);
    assert_eq!(full.true_peak(0).unwrap(), 0.0);
    full.add_frames_f32(&samples).unwrap();
    let mut fresh = meter(12, 48_000, mode, Some(&HOA_MAP));
    fresh.add_frames_f32(&samples).unwrap();
    for kind in 0..3 {
        assert_eq!(
            loudness(&full, kind).to_bits(),
            loudness(&fresh, kind).to_bits()
        );
    }
    assert!(EbuR128::new(0, 48_000, Mode::I).is_err());
    assert!(EbuR128::new(65, 48_000, Mode::I).is_err());
    assert!(EbuR128::new(2, 0, Mode::I).is_err());
    assert!(EbuR128::new(2, 48_000, Mode::empty()).is_err());
    assert!(full.set_channel(12, Channel::Left).is_err());
    assert!(full.add_frames_f32(&[0.0; 13]).is_err());
    println!(
        "{{\"kind\":\"state_checks\",\"chunk_sizes\":[1,127,512,4096],\"reset_allocations\":0,\"passed\":true}}"
    );
}

fn memory_checks() {
    for histogram in [false, true] {
        let mode = Mode::S
            | Mode::I
            | if histogram {
                Mode::HISTOGRAM
            } else {
                Mode::empty()
            };
        let mut rust = meter(2, 48_000, mode, None);
        let mut c = reference::Meter::new(2, 48_000, mode, None);
        let input = signal(48_000, 2, 0.1, "sine");
        let mut max_lu: f64 = 0.0;
        let mut query_time = 0u128;
        let counts = counted(|| {
            for block in 0..6000 {
                rust.add_frames_f32(&input).unwrap();
                c.add(&input);
                if block % 10 == 9 {
                    let start = Instant::now();
                    let measured = black_box(rust.loudness_global().unwrap());
                    query_time += start.elapsed().as_nanos();
                    max_lu = max_lu.max(error(measured, c.loudness(0)));
                }
            }
        });
        assert!(max_lu < 0.001);
        if histogram {
            assert_eq!(counts.calls + counts.reallocations, 0);
        }
        println!(
            "{{\"kind\":\"long_monitor\",\"audio_seconds\":600,\"histogram\":{histogram},\"rust_allocations\":{},\"rust_reallocations\":{},\"rust_requested_bytes\":{},\"integrated_query_mean_ns\":{},\"max_lufs_error\":{max_lu:e}}}",
            counts.calls,
            counts.reallocations,
            counts.requested_bytes,
            query_time / 600
        );
    }
    for channels in [2, 12, 24, 64] {
        let mut rust = meter(channels, 48_000, Mode::S | Mode::I | Mode::TRUE_PEAK, None);
        let input = signal(48_000, channels, 0.01, "mixed");
        let count = counted(|| {
            for _ in 0..100 {
                rust.add_frames_f32(&input).unwrap();
                black_box(rust.loudness_global().unwrap());
                black_box(rust.loudness_momentary().unwrap());
                black_box(rust.loudness_shortterm().unwrap());
                for channel in 0..channels {
                    black_box(rust.true_peak(channel).unwrap());
                }
            }
        });
        assert_eq!(count.calls + count.reallocations, 0);
        println!(
            "{{\"kind\":\"prepared_processing\",\"channels\":{channels},\"audio_seconds\":1,\"rust_allocations\":0}}"
        );
    }
}

fn history_limit_probe() {
    // Not called by the production project. Evaluate it before considering a
    // bounded-history strategy for the monitor. A finding is reported without
    // weakening the checks for the APIs that the project actually uses.
    let mut rust = meter(1, 48_000, Mode::I, None);
    let mut c = reference::Meter::new(1, 48_000, Mode::I, None);
    let input = signal(48_000, 1, 12.0, "sine");
    rust.add_frames_f32(&input).unwrap();
    c.add(&input);
    let before = rust.gating_block_count_and_energy().unwrap().0;
    rust.set_max_history(1000).unwrap();
    c.set_max_history(1000);
    let quiet: Vec<f32> = input[..67_200].iter().map(|sample| sample * 0.1).collect();
    rust.add_frames_f32(&quiet).unwrap();
    c.add(&quiet);
    let after = rust.gating_block_count_and_energy().unwrap().0;
    let limit_respected = after <= 10;
    let rust_lufs = rust.loudness_global().unwrap();
    let c_lufs = c.loudness(0);
    println!(
        "{{\"kind\":\"unused_api_probe\",\"api\":\"set_max_history\",\"limit_ms\":1000,\"blocks_before\":{before},\"blocks_after\":{after},\"limit_respected\":{limit_respected},\"rust_lufs\":{rust_lufs},\"c_lufs\":{c_lufs}}}"
    );
}

fn benchmark_rust(input: &[f32], channels: u32, mode: Mode, map: Option<&[Channel]>) -> u128 {
    let mut rust = meter(channels, 48_000, mode, map);
    let start = Instant::now();
    for (index, chunk) in input.chunks(512 * channels as usize).enumerate() {
        rust.add_frames_f32(black_box(chunk)).unwrap();
        if mode.contains(Mode::S) && index % 10 == 9 {
            black_box(rust.loudness_momentary().unwrap());
            black_box(rust.loudness_shortterm().unwrap());
        }
        if mode.contains(Mode::S) && index % 100 == 99 {
            black_box(rust.loudness_global().unwrap());
        }
    }
    black_box(rust.loudness_global().unwrap());
    if mode.contains(Mode::TRUE_PEAK) {
        for channel in 0..channels {
            black_box(rust.true_peak(channel).unwrap());
        }
    }
    black_box(&rust);
    start.elapsed().as_micros()
}

fn benchmark_c(input: &[f32], channels: u32, mode: Mode, map: Option<&[Channel]>) -> u128 {
    let mut c = reference::Meter::new(channels, 48_000, mode, map);
    let start = Instant::now();
    for (index, chunk) in input.chunks(512 * channels as usize).enumerate() {
        c.add(black_box(chunk));
        if mode.contains(Mode::S) && index % 10 == 9 {
            black_box(c.loudness(1));
            black_box(c.loudness(2));
        }
        if mode.contains(Mode::S) && index % 100 == 99 {
            black_box(c.loudness(0));
        }
    }
    black_box(c.loudness(0));
    if mode.contains(Mode::TRUE_PEAK) {
        for channel in 0..channels {
            black_box(c.peak(channel));
        }
    }
    black_box(&c);
    start.elapsed().as_micros()
}

fn benchmarks() {
    if cfg!(debug_assertions) {
        eprintln!("Benchmarks require --release");
        std::process::exit(2);
    }
    for (channels, map) in [
        (1, None),
        (2, None),
        (6, None),
        (12, None),
        (12, Some(HOA_MAP.as_slice())),
        (24, None),
    ] {
        let mapping = if map.is_some() { "hoa_714" } else { "default" };
        let input = signal(48_000, channels, 5.0, "mixed");
        for (name, mode) in [
            ("offline", Mode::I | Mode::TRUE_PEAK),
            ("monitor", Mode::S | Mode::I),
        ] {
            benchmark_rust(&input, channels, mode, map);
            benchmark_c(&input, channels, mode, map);
            for round in 0..5 {
                let (rust_us, c_us) = if round % 2 == 0 {
                    (
                        benchmark_rust(&input, channels, mode, map),
                        benchmark_c(&input, channels, mode, map),
                    )
                } else {
                    let c = benchmark_c(&input, channels, mode, map);
                    (benchmark_rust(&input, channels, mode, map), c)
                };
                println!(
                    "{{\"kind\":\"benchmark\",\"name\":{name:?},\"mapping\":{mapping:?},\"rate\":48000,\"channels\":{channels},\"audio_seconds\":5,\"block_frames\":512,\"round\":{round},\"rust_us\":{rust_us},\"c_us\":{c_us}}}"
                );
            }
        }
    }
}

fn main() {
    let command = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "verify".to_owned());
    match command.as_str() {
        "verify" => {
            comparisons();
            analytic_checks();
            state_checks();
            memory_checks();
            history_limit_probe();
        }
        "bench" => benchmarks(),
        _ => panic!("Use verify or bench"),
    }
}
