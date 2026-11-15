//! HpTF regressions formerly in the C++ `hptf_eq_test`, `hptf_eq_regression_test` and
//! `hptf_dsp_test` (R3 B), against the Rust kernels directly: the absolute response of a real
//! AutoEq curve, auto-trim over narrow and quantised peaks, long-run stability, preamp-only
//! profiles, exact fade values, reset priorities, partition invariance for every band type and
//! concurrent publication.
use mradm_dsp::hptf::{
    BLEND_FRAMES, Band, BandKind, Cascade, Coefficients, Live, PreampMode, Snapshot,
    StereoSnapshot, design, parse_parametric_eq,
};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

const RATE: u32 = 48000;

/// AutoEq: results/oratory1990/over-ear/Sony MDR-MV1/Sony MDR-MV1 ParametricEQ.txt.
const MV1: &str = "Preamp: -4.1 dB
Filter 1: ON LSC Fc 105 Hz Gain 9.7 dB Q 0.70
Filter 2: ON PK Fc 46 Hz Gain -9.2 dB Q 0.37
Filter 3: ON PK Fc 1722 Hz Gain 2.8 dB Q 0.95
Filter 4: ON PK Fc 4643 Hz Gain 9.2 dB Q 1.99
Filter 5: ON PK Fc 5779 Hz Gain -9.2 dB Q 1.50
Filter 6: ON HSC Fc 10000 Hz Gain -2.2 dB Q 0.70
Filter 7: ON PK Fc 705 Hz Gain 0.8 dB Q 2.45
Filter 8: ON PK Fc 242 Hz Gain -0.7 dB Q 2.04
Filter 9: ON PK Fc 128 Hz Gain 0.5 dB Q 2.09
Filter 10: ON PK Fc 2955 Hz Gain -0.9 dB Q 4.58
";

fn designed(text: &str, mode: PreampMode) -> Coefficients {
    let profile = parse_parametric_eq(text.as_bytes()).expect("profile parses");
    design(&profile.bands, profile.preamp_db, RATE, mode).expect("profile designs")
}

fn band(kind: BandKind, frequency: f64, gain_db: f64, q: f64) -> Band {
    Band {
        kind,
        enabled: true,
        frequency,
        gain_db,
        q,
    }
}

fn db(c: &Coefficients, hz: f64) -> f64 {
    c.magnitude_db(hz).unwrap()
}

/// The running cascade's response at `hz`: a single-bin DFT of its impulse response.
fn running_response(c: Coefficients, hz: f64, taps: usize) -> f64 {
    let mut cascade = Cascade::new(1).unwrap();
    cascade.set_coefficients(c).unwrap();
    let mut h = vec![0f32; taps];
    h[0] = 1.;
    cascade.process(&mut h).unwrap();
    let w = 2. * std::f64::consts::PI * hz / f64::from(RATE);
    let (mut re, mut im) = (0f64, 0f64);
    for (n, &x) in h.iter().enumerate() {
        let a = -w * n as f64;
        re += f64::from(x) * a.cos();
        im += f64::from(x) * a.sin();
    }
    re.hypot(im)
}

fn noise(frames: usize, channels: usize, seed: u32) -> Vec<f32> {
    let mut s = seed;
    (0..frames * channels)
        .map(|_| {
            s = s.wrapping_mul(1664525).wrapping_add(1013904223);
            (s >> 8) as f32 / (1u32 << 24) as f32 - 0.5
        })
        .collect()
}

fn snapshot(coefficients: Coefficients, revision: u64) -> StereoSnapshot {
    Snapshot {
        coefficients,
        revision,
    }
    .into()
}

/// A gain-only target whose preamp identifies the revision.
fn revision_gain(revision: u64) -> Coefficients {
    let (gain, db) = if !revision.is_multiple_of(2) {
        (0.5, -6.0206)
    } else {
        (0.25, -12.0412)
    };
    Coefficients {
        sample_rate: RATE,
        preamp_gain: gain,
        preamp_db: db,
        max_response_db: db,
        ..Coefficients::default()
    }
}

fn gain(value: f32) -> Coefficients {
    Coefficients {
        sample_rate: RATE,
        preamp_gain: value,
        preamp_db: 20. * value.log10(),
        max_response_db: 20. * value.log10(),
        ..Coefficients::default()
    }
}

#[test]
fn a_real_autoeq_curve_keeps_its_absolute_response() {
    let c = designed(MV1, PreampMode::WarnOnly);
    assert_eq!(c.band_count, 10);
    // The running filter matches the analytic response that drives auto-trim.
    for hz in [50., 100., 250., 700., 1700., 3000., 5800., 12000.] {
        let measured = 20. * running_response(c, hz, 32768).max(1e-30).log10();
        assert!((measured - db(&c, hz)).abs() <= 0.05, "{hz} Hz");
    }
    // Anchors checked against an external evaluation of the curve (with its -4.1 dB preamp).
    for (hz, expected) in [(100., -5.04), (1700., -1.36), (5800., -8.47)] {
        assert!((db(&c, hz) - expected).abs() <= 0.1, "{hz} Hz");
    }
    assert!((f64::from(c.max_response_db) + 0.10).abs() <= 0.1);
    // Already safe: auto-trim leaves it alone.
    assert_eq!(designed(MV1, PreampMode::AutoTrim).auto_trim_db, 0.);
    // OFF bands stay in the profile but not in the cascade.
    let off = designed(
        "Preamp: 0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 3 dB Q 1\nFilter 2: OFF PK Fc 2000 Hz Gain 3 dB Q 1\n",
        PreampMode::WarnOnly,
    );
    assert_eq!(off.band_count, 1);
}

#[test]
fn warn_only_reports_and_auto_trim_removes_a_hot_peak() {
    let hot = "Preamp: 0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 6 dB Q 2\n";
    let warn = designed(hot, PreampMode::WarnOnly);
    assert!((f64::from(warn.max_response_db) - 6.).abs() <= 0.05);
    assert_eq!(warn.auto_trim_db, 0.);
    let trim = designed(hot, PreampMode::AutoTrim);
    assert!(trim.auto_trim_db < -5.9);
    let peak = (0..512)
        .map(|i| db(&trim, 20. * ((1000f64).ln() / 511. * f64::from(i)).exp()))
        .fold(f64::NEG_INFINITY, f64::max);
    assert!(peak <= 0.001);
}

#[test]
fn auto_trim_covers_narrow_overlapping_and_off_grid_peaks() {
    let profiles = [
        "Filter 1: ON PK Fc 1000 Hz Gain 12 dB Q 80\n",
        "Filter 1: ON PK Fc 1093.17 Hz Gain 24 dB Q 300\n",
        "Filter 1: ON PK Fc 960 Hz Gain 8 dB Q 50\nFilter 2: ON PK Fc 1000 Hz Gain 12 dB Q 50\nFilter 3: ON NO Fc 1004 Hz Q 80\n",
        "Filter 1: ON LSC Fc 230 Hz Gain 9 dB Q 3\nFilter 2: ON HSC Fc 8500 Hz Gain 6 dB Q 2\n",
        "Filter 1: ON LP Fc 4100 Hz Q 15\n",
    ];
    for text in profiles {
        let c = designed(text, PreampMode::AutoTrim);
        assert!(c.max_response_db <= 0.001, "{text}");
        // An offset grid with a different cardinality from the implementation's.
        for i in 0..8191 {
            let hz = 20. * 1000f64.powf((f64::from(i) + 0.37) / 8191.);
            assert!(db(&c, hz) <= 0.002, "{text} at {hz} Hz");
        }
        for hz in [1000., 1093.17, 960., 4100.] {
            assert!(db(&c, hz) <= 0.002, "{text} at the {hz} Hz center");
        }
    }
    // The running filter at the peak an earlier search grid missed.
    let c = designed(profiles[0], PreampMode::AutoTrim);
    let response = running_response(c, 1000., 65536);
    assert!(response <= 1.0002 && response > 0.99, "{response}");
}

#[test]
fn auto_trim_covers_peaks_shifted_by_coefficient_quantisation() {
    for fc in [20.1, 30.7, 46.3, 98.73, 999.13, 17777.77] {
        for q in [0.37, 8., 80., 300.] {
            let c = design(
                &[band(BandKind::Peaking, fc, 12., q)],
                0.,
                RATE,
                PreampMode::AutoTrim,
            )
            .expect("quantised narrow-band filter remains stable");
            let width = (8. * fc / q).max(1.);
            let (low, high) = ((fc - width).max(20.), (fc + width).min(20000.));
            for i in 0..=2048 {
                let hz = low + (high - low) * f64::from(i) / 2048.;
                assert!(db(&c, hz) <= 0.002, "fc={fc} Q={q} hz={hz}");
            }
        }
    }
}

#[test]
fn a_low_high_q_band_stays_stable_over_a_long_run() {
    // 20 Hz with Q 8: where binary32 state loses precision.
    let c = designed(
        "Preamp: 0 dB\nFilter 1: ON PK Fc 20 Hz Gain 10 dB Q 8\n",
        PreampMode::WarnOnly,
    );
    let mut cascade = Cascade::new(2).unwrap();
    cascade.set_coefficients(c).unwrap();
    let source = noise(4096, 2, 4242);
    let mut worst = 0f32;
    for _ in 0..200 {
        let mut block = source.clone();
        cascade.process(&mut block).unwrap();
        assert!(block.iter().all(|x| x.is_finite()));
        worst = block.iter().fold(worst, |m, x| m.max(x.abs()));
    }
    assert!(worst < 100., "{worst}");
}

#[test]
fn a_preamp_only_profile_is_compensation_not_bypass() {
    for text in [
        "Preamp: -6 dB\n",
        "Preamp: -6 dB\nFilter 1: OFF PK Fc 1000 Hz Gain 3 dB Q 1\n",
    ] {
        let c = designed(text, PreampMode::WarnOnly);
        assert!(!c.is_bypass(), "{text}");
        let mut cascade = Cascade::new(2).unwrap();
        cascade.set_coefficients(c).unwrap();
        let input = [0.5f32, -0.5, 0.25, -0.25];
        let mut pcm = input;
        cascade.process(&mut pcm).unwrap();
        for (out, x) in pcm.iter().zip(input) {
            assert!((f64::from(*out) - f64::from(x) * 10f64.powf(-6. / 20.)).abs() < 1e-6);
        }
    }
}

#[test]
fn fades_have_exact_values_and_queued_targets_start_where_they_end() {
    let live = Live::new(2, RATE).unwrap();
    live.publish(snapshot(gain(0.5), 1)).unwrap();
    live.process(&mut []);
    assert_eq!(
        live.applied_revision(),
        0,
        "an empty call keeps the update pending"
    );
    let mut pcm = vec![1f32; 4096];
    live.process(&mut pcm);
    for frame in 0..2048 {
        let expected = 1. - 0.5 * frame as f32 / 2048.;
        assert_eq!((pcm[frame * 2], pcm[frame * 2 + 1]), (expected, expected));
    }
    assert_eq!(live.applied_revision(), 1);
    live.publish(snapshot(gain(0.25), 2)).unwrap();
    live.process(&mut vec![1f32; 514]);
    live.publish(snapshot(gain(0.75), 3)).unwrap();
    let mut pcm = vec![1f32; 4800];
    live.process(&mut pcm);
    // The fade to 2 ends 1791 frames into this call; 3 starts on the next frame.
    assert_eq!(live.applied_revision(), 2);
    assert_ne!(pcm[1790 * 2], 0.25);
    assert_eq!(pcm[1791 * 2], 0.25);
    assert_eq!(*pcm.last().unwrap(), 0.398_437_5);
    let mut pcm = vec![1f32; 274];
    live.process(&mut pcm);
    assert_eq!(pcm[0], 0.398_681_64);
    assert_eq!(live.applied_revision(), 2);
    live.reset_state();
    assert_eq!(
        live.applied_revision(),
        3,
        "a reset adopts the active fade's target"
    );
    live.publish(snapshot(gain(0.125), 4)).unwrap();
    live.reset_state();
    let mut pcm = vec![1f32; 256];
    live.process(&mut pcm);
    assert!(
        pcm.iter().all(|&x| x == 0.125),
        "a reset adopts the pending target"
    );
    live.publish_bypass(5).unwrap();
    live.reset_state();
    assert_eq!(live.applied_revision(), 5);
    assert!(live.active_snapshot().coefficients[0].is_bypass());
}

#[test]
fn a_reset_prefers_the_newest_target_and_reports_its_metadata() {
    let constant = |live: &Live, frames: usize, expected: Option<f32>| {
        let mut pcm = vec![1f32; frames * 2];
        live.process(&mut pcm);
        if let Some(e) = expected {
            assert!(pcm.iter().all(|x| (x - e).abs() < 1e-6));
        }
    };
    let live = Live::new(2, RATE).unwrap();
    live.publish(snapshot(revision_gain(1), 1)).unwrap();
    constant(&live, BLEND_FRAMES, None);
    live.publish(snapshot(revision_gain(2), 2)).unwrap();
    constant(&live, 256, None);
    let active = live.active_snapshot();
    assert_eq!(active.revision, 1, "an in-flight target is not yet applied");
    assert_eq!(active.coefficients[0].preamp_db, revision_gain(1).preamp_db);
    live.reset_state();
    assert_eq!(live.active_snapshot().revision, 2);
    constant(&live, 4096, Some(0.25));
    live.publish(snapshot(revision_gain(3), 3)).unwrap();
    constant(&live, 256, None);
    live.publish(snapshot(revision_gain(4), 4)).unwrap();
    live.reset_state();
    assert_eq!(
        live.applied_revision(),
        4,
        "a newer pending target wins over the fade"
    );
    constant(&live, 4096, Some(0.25));
    live.publish_bypass(5).unwrap();
    live.reset_state();
    assert!(live.applied_revision() == 5 && live.active_snapshot().coefficients[0].is_bypass());
    constant(&live, 4096, Some(1.));
}

#[test]
fn every_band_type_is_independent_of_partitioning_at_every_rate() {
    let kinds = [
        BandKind::Peaking,
        BandKind::LowShelf,
        BandKind::HighShelf,
        BandKind::LowPass,
        BandKind::HighPass,
        BandKind::BandPass,
        BandKind::Notch,
    ];
    for rate in [8000, 32000, 44100, 48000, 96000, 192000] {
        for kind in kinds {
            for mode in [PreampMode::WarnOnly, PreampMode::AutoTrim] {
                let c = design(&[band(kind, 997.13, 9., 2.5)], -1.5, rate, mode).unwrap();
                let source = noise(4096, 2, 7)
                    .iter()
                    .map(|x| x * 0.25)
                    .collect::<Vec<_>>();
                let (mut whole, mut parts) = (source.clone(), source);
                let mut first = Cascade::new(2).unwrap();
                let mut second = Cascade::new(2).unwrap();
                first.set_coefficients(c).unwrap();
                second.set_coefficients(c).unwrap();
                first.process(&mut whole).unwrap();
                for chunk in parts.chunks_mut(137 * 2) {
                    second.process(chunk).unwrap();
                }
                let trim = matches!(mode, PreampMode::AutoTrim);
                assert_eq!(whole, parts, "{rate} {kind:?} trim={trim}");
            }
        }
    }
}

#[test]
fn concurrent_queries_see_one_applied_revision_and_reach_the_last() {
    let live = Arc::new(Live::new(2, RATE).unwrap());
    let stop = Arc::new(AtomicBool::new(false));
    let audio = {
        let (live, stop) = (live.clone(), stop.clone());
        std::thread::spawn(move || {
            let mut pcm = [0f32; 512];
            while !stop.load(Ordering::Relaxed) {
                pcm.fill(0.1);
                live.process(&mut pcm);
            }
        })
    };
    let query = {
        let (live, stop) = (live.clone(), stop.clone());
        std::thread::spawn(move || {
            let mut coherent = true;
            while !stop.load(Ordering::Relaxed) {
                let s = live.active_snapshot();
                if s.revision != 0 {
                    let (c, e) = (s.coefficients[0], revision_gain(s.revision));
                    coherent &= c.preamp_gain == e.preamp_gain
                        && c.preamp_db == e.preamp_db
                        && c.max_response_db == e.max_response_db;
                }
            }
            coherent
        })
    };
    const UPDATES: u64 = 500;
    for revision in 1..=UPDATES {
        live.publish(snapshot(revision_gain(revision), revision))
            .unwrap();
        if revision % 8 == 0 {
            std::thread::yield_now();
        }
    }
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
    while live.applied_revision() != UPDATES && std::time::Instant::now() < deadline {
        std::thread::yield_now();
    }
    stop.store(true, Ordering::Relaxed);
    audio.join().unwrap();
    assert!(query.join().unwrap(), "a query mixed two revisions");
    assert_eq!(live.applied_revision(), UPDATES);
}
