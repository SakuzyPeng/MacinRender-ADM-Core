//! The render service's request rules and post-processing on whole renders (formerly the C++
//! `render_service_test`, `render_trim_fixture_test`, `loudness_fixture_test`,
//! `peak_limiter_fixture_test` and `normalization_acceptance_test`, R3 C4): policy errors, trim
//! windows and their rejections, metrics that follow the window, windows equal to slices of the
//! full render for every speaker backend, a window beyond 2^31 frames of a sparse BW64 input,
//! loudness normalisation, the True Peak limiter and peak make-up, and the final PCM's loudness and
//! True Peak measured independently. Cached preparations, request texts and cancellation are
//! `render.rs`; path conversion at the C ABI is `mradm-capi`'s `options.rs`.
mod common;
use common::render::*;
use mradm_dsp::meter::{Meter, MeterMode};
use mradm_engine::render::Request;
use mradm_engine::request::{BitDepth, Selection};
#[cfg(unix)]
use mradm_testkit::adm::Type;
use mradm_testkit::adm::gain_linear;
use mradm_testkit::wave::{AudioId, Wave};
use std::path::{Path, PathBuf};

const RATE: u32 = 48000;

/// One front object over `samples` (32-bit PCM so the test signal is kept almost exactly).
fn object_input(dir: &Dir, name: &str, samples: &[f32]) -> PathBuf {
    let path = dir.file(name);
    let (axml, uid) = Single::objects(vec![polar(0., 0., [gain_linear(1.0)])]).built();
    let mut wave = Wave::new(1, RATE, 32);
    wave.chna = Some(vec![AudioId::new(1, &uid, "", "")]);
    wave.axml = Some(axml);
    wave.write(&path, samples).unwrap();
    path
}

fn sine(frames: usize, amplitude: f32, hz: f32) -> Vec<f32> {
    (0..frames)
        .map(|n| amplitude * (std::f32::consts::TAU * hz * n as f32 / RATE as f32).sin())
        .collect()
}

fn service(input: &Path, output: &Path, renderer: Selection, layout: &str) -> Request {
    let mut r = request(input, output, renderer, layout);
    r.peak_limit = false;
    r
}

fn trimmed(input: &Path, output: &Path, start: f64, end: Option<f64>) -> Run {
    let mut r = service(input, output, Selection::Ear, "0+5+0");
    r.render_start_sec = start;
    r.render_end_sec = end;
    run(r)
}

/// 2 s, silent in the first half and 0.5 in the second (the C++ trim fixture).
fn half_silent(dir: &Dir) -> PathBuf {
    let samples: Vec<f32> = (0..2 * RATE as usize)
        .map(|f| if f >= RATE as usize { 0.5 } else { 0. })
        .collect();
    object_input(dir, "half.wav", &samples)
}

#[test]
fn policy_errors_are_parameter_and_io_errors() {
    let dir = scratch("service-policy");
    let input = half_silent(&dir);
    let mut r = service(&input, &dir.file("out.wav"), Selection::Saf, "0+5+0");
    r.semantic_policy_json = Some(b"{".to_vec());
    assert_eq!(run(r.clone()).code(), 1, "malformed policy JSON");
    r.semantic_policy_json = None;
    r.semantic_policy_path = Some(text(&dir.file("missing.json")));
    assert_eq!(run(r).code(), 3, "a missing policy file");
}

#[test]
fn trim_windows_cut_and_reject_as_requested() {
    let dir = scratch("service-trim");
    let input = half_silent(&dir);
    let frames = |start, end| {
        let out = dir.file("trim.wav");
        assert!(trimmed(&input, &out, start, end).ok());
        Audio::read(&out).frames
    };
    assert_eq!(frames(0., Some(1.)), RATE as usize);
    assert_eq!(frames(0.5, Some(1.5)), RATE as usize);
    assert_eq!(frames(0., None), 2 * RATE as usize);
    assert_eq!(
        frames(0., Some(f64::MAX)),
        2 * RATE as usize,
        "a huge finite end clamps"
    );

    let (inf, nan) = (f64::INFINITY, f64::NAN);
    let rejected = [
        (3., None),
        (inf, None),
        (-inf, None),
        (nan, None),
        (-0.1, None),
        (nan, Some(1.)),
        (0., Some(inf)),
        (0., Some(-inf)),
        (0., Some(nan)),
        (0.5, Some(nan)),
        (1., Some(1.)),
        (1., Some(1. + 0.4 / RATE as f64)),
    ];
    for (start, end) in rejected {
        let out = dir.file("rejected.wav");
        let result = trimmed(&input, &out, start, end);
        assert_eq!(result.code(), 1, "start={start} end={end:?}");
        assert!(!out.exists(), "start={start} end={end:?} leaves no output");
    }
}

#[test]
fn metrics_follow_the_window() {
    let dir = scratch("service-metrics");
    let input = half_silent(&dir);
    for (renderer, layout) in [(Selection::Ear, "0+5+0"), (Selection::Hoa, "hoa3")] {
        let peak = |start, end| {
            let mut r = service(&input, &dir.file("m.wav"), renderer, layout);
            r.render_start_sec = start;
            r.render_end_sec = Some(end);
            run(r).outcome.metrics.and_then(|m| m.peak_dbtp)
        };
        assert!(
            peak(0., 1.).is_none(),
            "{layout}: the silent half has no True Peak"
        );
        assert!(peak(1., 2.).is_some(), "{layout}: the active half has one");
    }
}

#[test]
fn windows_equal_slices_of_the_full_render() {
    let dir = scratch("service-windows");
    let samples: Vec<f32> = (0..2 * RATE as usize)
        .map(|f| 0.5 * (f as f32 * 0.013).sin())
        .collect();
    let input = object_input(&dir, "sine.wav", &samples);
    let (start, count) = (24000usize, 16000usize);
    for (renderer, layout, tolerance) in [
        (Selection::Ear, "0+5+0", 0.),
        (Selection::Saf, "0+5+0", 0.),
        (Selection::Hoa, "hoa3", 0.),
        (Selection::Binaural, "0+2+0", 1e-6),
    ] {
        let full = render(service(&input, &dir.file("full.wav"), renderer, layout)).1;
        let mut r = service(&input, &dir.file("window.wav"), renderer, layout);
        r.render_start_sec = start as f64 / RATE as f64;
        r.render_end_sec = Some((start + count) as f64 / RATE as f64);
        let window = render(r).1;
        let slice = &full.samples[start * full.channels..(start + count) * full.channels];
        assert_eq!(window.samples.len(), slice.len(), "{layout}");
        let worst = window
            .samples
            .iter()
            .zip(slice)
            .fold(0f32, |m, (a, b)| m.max((a - b).abs()));
        assert!(worst <= tolerance, "{renderer:?}: {worst}");
    }
}

/// A 6-object ADM input rewritten as a BW64 whose data chunk is a sparse hole of `frames` frames
/// (the C++ seed: 32 frames of a 24-bit RIFF, its chunks copied after a leading `ds64`).
#[cfg(unix)]
fn sparse_bw64(path: &Path, frames: u64) {
    let (axml, uids) = tracks(vec![(Type::Objects, vec![polar(0., 0., [])]); 6], false);
    let mut wave = Wave::new(6, RATE, 24);
    wave.chna = Some(
        uids.iter()
            .enumerate()
            .map(|(i, uid)| AudioId::new(i as u16 + 1, uid, "", ""))
            .collect(),
    );
    wave.axml = Some(axml);
    let seed = wave.bytes(&[0.; 6 * 32]).unwrap();
    let mut chunks = Vec::new();
    let mut offset = 12;
    while offset + 8 <= seed.len() {
        let size = u32::from_le_bytes(seed[offset + 4..offset + 8].try_into().unwrap()) as usize;
        let id = &seed[offset..offset + 4];
        if id != b"data" && id != b"ds64" {
            chunks.extend_from_slice(&seed[offset..offset + 8 + size + (size & 1)]);
        }
        offset += 8 + size + (size & 1);
    }
    let data_bytes = frames * 18;
    let header = (12 + 8 + 28 + chunks.len() + 8) as u64;
    let mut out = Vec::new();
    out.extend_from_slice(b"BW64");
    out.extend_from_slice(&u32::MAX.to_le_bytes());
    out.extend_from_slice(b"WAVE");
    out.extend_from_slice(b"ds64");
    out.extend_from_slice(&28u32.to_le_bytes());
    out.extend_from_slice(&(header + data_bytes - 8).to_le_bytes());
    out.extend_from_slice(&data_bytes.to_le_bytes());
    out.extend_from_slice(&frames.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&chunks);
    out.extend_from_slice(b"data");
    out.extend_from_slice(&u32::MAX.to_le_bytes());
    assert_eq!(out.len() as u64, header);
    std::fs::write(path, &out).unwrap();
    std::fs::OpenOptions::new()
        .write(true)
        .open(path)
        .unwrap()
        .set_len(header + data_bytes)
        .unwrap();
}

/// A window past frame 2^31 of a sparse BW64 bed: the reader seeks with 64-bit frames.
#[cfg(unix)]
#[test]
fn a_window_beyond_two_to_the_31_frames_renders() {
    let dir = scratch("service-sparse");
    let input = dir.file("sparse.wav");
    let frames = i32::MAX as u64 + 2 * u64::from(RATE);
    sparse_bw64(&input, frames);
    let start = frames - 2048;
    assert!(start > i32::MAX as u64);
    let mut r = service(&input, &dir.file("window.wav"), Selection::Saf, "0+5+0");
    r.render_start_sec = start as f64 / RATE as f64;
    r.render_end_sec = Some((start + 1024) as f64 / RATE as f64);
    let (_, audio) = render(r);
    assert_eq!((audio.channels, audio.frames), (6, 1024));
}

fn measure(audio: &Audio) -> (f64, f64) {
    let mut meter = Meter::new(
        audio.channels as u32,
        audio.rate,
        MeterMode::IntegratedTruePeak,
        &[],
    )
    .unwrap();
    meter.add_frames(&audio.samples).unwrap();
    (
        meter.integrated().unwrap(),
        20. * meter.max_true_peak().unwrap().log10(),
    )
}

#[test]
fn loudness_normalisation_reaches_its_target() {
    let dir = scratch("service-loudness");
    let input = object_input(&dir, "loud.wav", &sine(5 * RATE as usize, 0.5, 440.));
    let mut r = service(&input, &dir.file("out.wav"), Selection::Saf, "0+5+0");
    r.measure_loudness = true;
    r.loudness_target_lufs = -23.;
    let (_, audio) = render(r.clone());
    let (lufs, _) = measure(&audio);
    assert!((lufs + 23.).abs() < 1.5, "{lufs} LUFS");

    let silent = object_input(&dir, "silent.wav", &vec![0.; 3 * RATE as usize]);
    r.input = text(&silent);
    let (_, audio) = render(r.clone());
    assert!(
        audio.samples.iter().all(|v| *v == 0.),
        "silence stays silent"
    );
    r.input = text(&dir.file("missing.wav"));
    assert_eq!(run(r).code(), 3);
}

#[test]
fn the_limiter_and_peak_make_up() {
    let dir = scratch("service-limiter");
    let peak_of = |amplitude: f32, edit: &dyn Fn(&mut Request)| {
        let input = object_input(
            &dir,
            &format!("in-{amplitude}.wav"),
            &sine(RATE as usize, amplitude, 440.),
        );
        let mut r = service(&input, &dir.file("out.wav"), Selection::Saf, "0+5+0");
        r.peak_limit = true;
        edit(&mut r);
        let (_, audio) = render(r);
        audio
            .samples
            .iter()
            .fold(0f64, |m, v| m.max(f64::from(v.abs())))
    };
    let ceiling = 10f64.powf(-1. / 20.);
    let limited = peak_of(0.95, &|r| r.peak_limit_dbtp = -1.);
    assert!(
        limited <= ceiling * 1.001 && limited > ceiling * 0.9,
        "{limited}"
    );
    let untouched = peak_of(0.10, &|r| r.peak_limit_dbtp = -1.);
    assert!((0.08..0.12).contains(&untouched), "{untouched}");
    let red_line = 10f64.powf(-6. / 20.);
    let raised = peak_of(0.01, &|r| {
        r.peak_limit_dbtp = -6.;
        r.peak_normalize_to_limit = true;
    });
    assert!(
        raised <= red_line * 1.02 && raised > red_line * 0.85,
        "{raised}"
    );

    let input = object_input(&dir, "requires.wav", &sine(RATE as usize, 0.1, 440.));
    let mut r = service(&input, &dir.file("out.wav"), Selection::Saf, "0+5+0");
    r.peak_normalize_to_limit = true;
    assert_eq!(run(r).code(), 1, "peak make-up requires the limiter");
}

/// 6 s of a 1 kHz tone at 0.25 with a 0.5 s inter-sample-peak burst at 3 s (the C++ acceptance
/// signal), scaled.
fn acceptance_signal(scale: f32) -> Vec<f32> {
    let (start, end) = (3 * RATE as usize, 3 * RATE as usize + RATE as usize / 2);
    (0..6 * RATE as usize)
        .map(|n| {
            let t = n as f64 / f64::from(RATE);
            let mut v = 0.25 * (std::f64::consts::TAU * 1000. * t).sin();
            if (start..end).contains(&n) {
                v = (std::f64::consts::FRAC_PI_2 * (n % 4) as f64 + std::f64::consts::FRAC_PI_4)
                    .sin();
            }
            (f64::from(scale) * v) as f32
        })
        .collect()
}

#[test]
fn final_pcm_meets_the_normalisation_targets() {
    let dir = scratch("service-acceptance");
    type Case = (
        &'static str,
        f32,
        fn(&mut Request),
        Option<f64>,
        Option<f64>,
    );
    let cases: [Case; 6] = [
        (
            "loudness -23",
            0.5,
            |r| {
                r.measure_loudness = true;
                r.loudness_target_lufs = -23.;
            },
            Some(-23.),
            None,
        ),
        (
            "loudness -23, final gain -2.5 dB",
            0.5,
            |r| {
                r.measure_loudness = true;
                r.loudness_target_lufs = -23.;
                r.final_gain_db = -2.5;
            },
            Some(-25.5),
            None,
        ),
        (
            "peak make-up to -6 dBTP",
            0.25,
            |r| {
                r.peak_limit = true;
                r.peak_normalize_to_limit = true;
                r.peak_limit_dbtp = -6.;
            },
            None,
            Some(-6.),
        ),
        (
            "peak make-up to -6 dBTP, 24-bit",
            0.25,
            |r| {
                r.peak_limit = true;
                r.peak_normalize_to_limit = true;
                r.peak_limit_dbtp = -6.;
                r.output_bit_depth = BitDepth::I24 as u32;
            },
            None,
            Some(-6.),
        ),
        (
            "default -1 dBTP ceiling",
            1.0,
            |r| r.peak_limit = true,
            None,
            Some(-1.),
        ),
        (
            "silence",
            0.0,
            |r| {
                r.measure_loudness = true;
                r.peak_limit = true;
            },
            None,
            None,
        ),
    ];
    for (name, scale, edit, lufs, peak) in cases {
        let input = object_input(&dir, "acceptance.wav", &acceptance_signal(scale));
        let mut r = service(&input, &dir.file("out.wav"), Selection::Ear, "0+5+0");
        edit(&mut r);
        let (run, audio) = render(r);
        if lufs.is_none() && peak.is_none() {
            assert!(
                audio.samples.iter().all(|v| *v == 0.),
                "{name}: silence stays digital silence"
            );
            let m = run.outcome.metrics;
            assert!(
                m.is_none_or(|m| m.lufs.is_none() && m.peak_dbtp.is_none()),
                "{name}"
            );
            continue;
        }
        let (measured_lufs, measured_peak) = measure(&audio);
        if let Some(target) = lufs {
            assert!(
                (measured_lufs - target).abs() < 1e-5,
                "{name}: {measured_lufs} LUFS"
            );
        }
        if let Some(target) = peak {
            assert!(
                (measured_peak - target).abs() < 1e-5,
                "{name}: {measured_peak} dBTP"
            );
        }
        let metrics = run.outcome.metrics.expect("metrics are reported");
        if let Some(reported) = metrics.lufs {
            assert!(
                (reported - measured_lufs).abs() < 1e-5,
                "{name}: reported {reported} LUFS"
            );
        }
        if let Some(reported) = metrics.peak_dbtp {
            assert!(
                (reported - measured_peak).abs() < 1e-5,
                "{name}: reported {reported} dBTP"
            );
        }
    }
}
