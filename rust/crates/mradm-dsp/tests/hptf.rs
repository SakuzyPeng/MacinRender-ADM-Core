use mradm_dsp::hptf::{
    BLEND_FRAMES, Band, BandKind, Biquad, Cascade, Coefficients, MAX_BANDS, PreampMode, Processor,
    Snapshot, design,
};

fn band(kind: BandKind, frequency: f64, gain_db: f64, q: f64) -> Band {
    Band {
        kind,
        enabled: true,
        frequency,
        gain_db,
        q,
    }
}
fn profile() -> Coefficients {
    design(
        &[
            band(BandKind::LowShelf, 105., 9.7, 0.7),
            band(BandKind::Peaking, 46., -9.2, 0.37),
            band(BandKind::HighShelf, 10000., -2.2, 0.7),
        ],
        -4.1,
        48000,
        PreampMode::WarnOnly,
    )
    .unwrap()
}
fn noise(frames: usize, channels: usize) -> Vec<f32> {
    let mut seed = 7u32;
    (0..frames * channels)
        .map(|_| {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            ((seed >> 8) as f32 / 16777216.0 - 0.5) * 0.25
        })
        .collect()
}
fn close(a: &[f32], b: &[f32], tolerance: f32) {
    assert_eq!(a.len(), b.len());
    for (index, (&a, &b)) in a.iter().zip(b).enumerate() {
        assert!((a - b).abs() <= tolerance, "sample {index}: {a} != {b}");
    }
}

// Direct Form I is an independent recurrence (four delay samples per section),
// while production uses transposed Direct Form II (two state values).
struct Direct {
    coefficients: Coefficients,
    channels: usize,
    state: Vec<[f64; 4]>,
}
impl Direct {
    fn new(coefficients: Coefficients, channels: usize) -> Self {
        Self {
            coefficients,
            channels,
            state: vec![[0.0; 4]; channels * coefficients.band_count],
        }
    }
    fn process(&mut self, input: &[f32]) -> Vec<f32> {
        let mut out = Vec::with_capacity(input.len());
        for frame in input.chunks(self.channels) {
            for (channel, &input) in frame.iter().enumerate() {
                let mut x = input as f64 * self.coefficients.preamp_gain as f64;
                for (section, coefficients) in self.coefficients.sections
                    [..self.coefficients.band_count]
                    .iter()
                    .enumerate()
                {
                    let s = &mut self.state[channel * self.coefficients.band_count + section];
                    let y = (coefficients.b0 as f64 * x)
                        + (coefficients.b1 as f64 * s[0])
                        + (coefficients.b2 as f64 * s[1])
                        - (coefficients.a1 as f64 * s[2])
                        - (coefficients.a2 as f64 * s[3]);
                    *s = [x, s[0], y, s[2]];
                    x = y;
                }
                out.push(x as f32);
            }
        }
        out
    }
}

#[test]
fn design_anchors_and_parameter_contracts() {
    for rate in [8000, 32000, 44100, 48000, 96000, 192000] {
        let p = design(
            &[band(BandKind::Peaking, 997.13, 6., 1.5)],
            0.,
            rate,
            PreampMode::WarnOnly,
        )
        .unwrap();
        assert!((p.magnitude_db(997.13).unwrap() - 6.).abs() < 0.002);
        let p = design(
            &[band(BandKind::Peaking, 1093.17, 24., 300.)],
            0.,
            rate,
            PreampMode::AutoTrim,
        )
        .unwrap();
        assert!(p.max_response_db <= 0.001 && p.auto_trim_db < -23.);
        assert!(p.magnitude_db(1093.17).unwrap() <= 0.002);
        for kind in [BandKind::LowShelf, BandKind::HighShelf] {
            let p = design(
                &[band(kind, 1000., 6., 0.707)],
                0.,
                rate,
                PreampMode::WarnOnly,
            )
            .unwrap();
            let (low, high) = (
                p.magnitude_db(0.).unwrap(),
                p.magnitude_db(rate as f64 / 2.).unwrap(),
            );
            let expected = if matches!(kind, BandKind::LowShelf) {
                (6., 0.)
            } else {
                (0., 6.)
            };
            assert!((low - expected.0).abs() < 0.02 && (high - expected.1).abs() < 0.02);
        }
    }
    let high = band(BandKind::Peaking, 20000., 6., 1.);
    assert_eq!(
        design(&[high], 0., 32000, PreampMode::WarnOnly)
            .unwrap()
            .band_count,
        0
    );
    assert_eq!(
        design(&[high], 0., 48000, PreampMode::WarnOnly)
            .unwrap()
            .band_count,
        1
    );
    assert!(design(&[high; MAX_BANDS + 1], 0., 8000, PreampMode::WarnOnly).is_err());
    for value in [f64::NAN, f64::INFINITY, 800., -800.] {
        assert!(design(&[], value, 48000, PreampMode::WarnOnly).is_err());
    }
    assert!(design(&[], 0., 0, PreampMode::WarnOnly).is_err());
    for mut invalid in [
        band(BandKind::Peaking, 0., 0., 1.),
        band(BandKind::Peaking, 1000., 0., 0.),
        band(BandKind::Peaking, 1000., f64::NAN, 1.),
    ] {
        invalid.enabled = false;
        assert!(design(&[invalid], 0., 48000, PreampMode::WarnOnly).is_err());
    }
    for invalid in [
        band(BandKind::Peaking, 1e-30, 3., 1.),
        band(BandKind::Peaking, 1000., 1e308, 1.),
        band(BandKind::Peaking, 1000., 3., 1e-300),
    ] {
        assert!(design(&[invalid], 0., 48000, PreampMode::WarnOnly).is_err());
    }
    assert!(BandKind::try_from(7).is_err());
    assert!(PreampMode::try_from(2).is_err());
}

#[test]
fn cascade_matches_direct_form_one_partitioning_and_channel_isolation() {
    let coefficients = profile();
    let source = noise(8192, 3);
    let mut expected = Direct::new(coefficients, 3);
    let reference = expected.process(&source);
    for frames in [1, 17, 137, 4096] {
        let mut c = Cascade::new(3).unwrap();
        c.set_coefficients(coefficients).unwrap();
        let mut out = source.clone();
        for block in out.chunks_mut(frames * 3) {
            c.process(block).unwrap();
        }
        close(&out, &reference, 2e-6);
    }
    let mut c = Cascade::new(3).unwrap();
    c.set_coefficients(coefficients).unwrap();
    let mut impulse = vec![0.; 4096 * 3];
    impulse[1] = 1.;
    c.process(&mut impulse).unwrap();
    assert!(
        impulse
            .as_chunks::<3>()
            .0
            .iter()
            .all(|f| f[0] == 0. && f[2] == 0.)
    );
}

#[test]
fn bypass_clone_reset_and_nonfinite_state_recovery() {
    let mut c = Cascade::new(2).unwrap();
    let mut special = [0., -0., f32::from_bits(0x7fc01234), f32::INFINITY];
    let bits = special.map(f32::to_bits);
    c.process(&mut special).unwrap();
    assert_eq!(special.map(f32::to_bits), bits);
    c.set_coefficients(profile()).unwrap();
    c.process(&mut noise(256, 2)).unwrap();
    let mut other = c.clone();
    c.process(&mut []).unwrap();
    let (mut a, mut b) = (noise(256, 2), noise(256, 2));
    c.process(&mut a).unwrap();
    other.process(&mut b).unwrap();
    assert_eq!(a, b);
    c.reset();
    let mut silent = [0.; 32];
    c.process(&mut silent).unwrap();
    assert_eq!(silent, [0.; 32]);
    let mut bad = [0.1; 32];
    bad[3] = f32::NAN;
    c.process(&mut bad).unwrap();
    assert!(bad.iter().any(|x| !x.is_finite()));
    let mut fresh = Cascade::new(2).unwrap();
    fresh.set_coefficients(profile()).unwrap();
    let (mut a, mut b) = (noise(256, 2), noise(256, 2));
    c.process(&mut a).unwrap();
    fresh.process(&mut b).unwrap();
    assert_eq!(a, b);
    assert!(Cascade::new(usize::MAX).is_err());
    let mut zero = Cascade::new(0).unwrap();
    zero.process(&mut []).unwrap();
    assert!(zero.process(&mut [1.]).is_err());
}

#[test]
fn processor_uses_two_independent_histories_and_preserves_reset_priority() {
    let a = profile();
    let b = design(
        &[
            band(BandKind::Peaking, 1500., 5., 1.2),
            band(BandKind::LowPass, 4000., 0., 2.),
        ],
        -2.,
        48000,
        PreampMode::WarnOnly,
    )
    .unwrap();
    let mut p = Processor::new(2, 48000).unwrap();
    p.reset(Some(Snapshot {
        coefficients: a,
        revision: 1,
    }))
    .unwrap();
    let mut old = Direct::new(a, 2);
    let mut new = Direct::new(b, 2);
    let warm = noise(4096, 2);
    old.process(&warm);
    p.process(&mut warm.clone(), None).unwrap();
    let source = noise(4096, 2);
    let left = old.process(&source);
    let right = new.process(&source);
    let mut expected = source.clone();
    for (i, x) in expected.iter_mut().enumerate() {
        let t = ((i / 2) as f64 / BLEND_FRAMES as f64).min(1.) as f32;
        *x = left[i] * (1. - t) + right[i] * t;
    }
    let mut output = source.clone();
    let mut offset = 0;
    for frames in [1, 137, 900, 3058] {
        let target = (offset == 0).then_some(Snapshot {
            coefficients: b,
            revision: 2,
        });
        let update = p
            .process(&mut output[offset * 2..(offset + frames) * 2], target)
            .unwrap();
        offset += frames;
        assert_eq!(
            update.snapshot.revision,
            if offset >= BLEND_FRAMES { 2 } else { 1 }
        );
    }
    close(&output, &expected, 2e-6);
    p.process(
        &mut [0.1; 32],
        Some(Snapshot {
            coefficients: a,
            revision: 3,
        }),
    )
    .unwrap();
    assert_eq!(p.reset(None).unwrap().snapshot.revision, 3);
    p.process(
        &mut [0.1; 32],
        Some(Snapshot {
            coefficients: b,
            revision: 4,
        }),
    )
    .unwrap();
    let bypass = Coefficients {
        sample_rate: 48000,
        ..Coefficients::default()
    };
    assert_eq!(
        p.reset(Some(Snapshot {
            coefficients: bypass,
            revision: 5
        }))
        .unwrap()
        .snapshot
        .revision,
        5
    );
    let mut pcm = source.clone();
    p.process(&mut pcm, None).unwrap();
    assert_eq!(pcm, source);
}

#[test]
fn malformed_frames_and_targets_preserve_state_and_output() {
    let mut c = Cascade::new(2).unwrap();
    c.set_coefficients(profile()).unwrap();
    for bad in [
        Coefficients {
            band_count: MAX_BANDS + 1,
            ..profile()
        },
        Coefficients {
            preamp_gain: f32::NAN,
            ..profile()
        },
    ] {
        assert!(c.set_coefficients(bad).is_err());
        assert_eq!(c.coefficients(), profile());
    }
    let mut unstable = profile();
    unstable.sections[0] = Biquad {
        a1: 2.,
        a2: 1.,
        ..Biquad::default()
    };
    assert!(c.set_coefficients(unstable).is_err());
    let mut out = [42.; 3];
    assert!(c.process(&mut out).is_err());
    assert_eq!(out, [42.; 3]);
    let target = Snapshot {
        coefficients: profile(),
        revision: 1,
    };
    let mut p = Processor::new(2, 48000).unwrap();
    let mut reference = Processor::new(2, 48000).unwrap();
    for processor in [&mut p, &mut reference] {
        processor.process(&mut [0.1; 16], Some(target)).unwrap();
    }
    let before = p.update(false);
    let mut out = [42.; 16];
    assert!(p.process(&mut out, Some(target)).is_err());
    assert_eq!(out, [42.; 16]);
    assert!(p.process(&mut out[..3], None).is_err());
    assert!(
        p.reset(Some(Snapshot {
            coefficients: unstable,
            revision: 2
        }))
        .is_err()
    );
    assert_eq!(p.update(false), before);
    let (mut a, mut b) = (noise(4096, 2), noise(4096, 2));
    p.process(&mut a, None).unwrap();
    reference.process(&mut b, None).unwrap();
    assert_eq!(a, b);
    assert!(Processor::new(usize::MAX, 48000).is_err());
}
