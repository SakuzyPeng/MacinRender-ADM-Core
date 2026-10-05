use mradm_dsp::hoa::{
    self, BlockInput, Diffuse, Encoder, HAS_INTERPOLATION, JUMP, MeterPreprocessor, Plan, RowInput,
    Source,
};
use std::sync::Arc;
fn source() -> Source {
    Source {
        position: [0., 0., 1.],
        gain: 1.,
        ..Source::default()
    }
}
fn plan(diffuse: f32) -> Arc<Plan> {
    let source = Source {
        diffuse,
        ..source()
    };
    Arc::new(
        Plan::new(
            1,
            &[RowInput {
                input: 0,
                block_offset: 0,
                block_count: 1,
            }],
            &[BlockInput {
                start: 0,
                end: u64::MAX,
                source_count: 1,
                object_gain: 1.,
                ..BlockInput::default()
            }],
            &[0],
            &[source],
        )
        .unwrap()
        .0,
    )
}
fn signal(n: usize) -> Vec<f32> {
    (0..n)
        .map(|i| ((i * 13 % 83) as f32 - 41.) / 128.)
        .collect()
}
#[test]
fn independent_axes_lfe_and_diagnostics() {
    let p = plan(0.);
    let c = p.coefficients(0, 0).unwrap();
    assert_eq!(c.direct[0].to_bits(), 1f32.to_bits());
    assert_eq!(c.direct[1], 0.);
    assert_eq!(c.direct[2], 0.);
    assert_eq!(c.direct[3].to_bits(), 1f32.to_bits());
    assert_eq!(c.direct[6], -0.5);
    assert!((c.direct[8] - 3f32.sqrt() * 0.5).abs() < 1e-7);
    let source = Source {
        position: [0.25, 0.75, 0.375],
        cartesian: 1,
        gain: 0.5,
        ..source()
    };
    assert_eq!(
        hoa::coefficients::length(source.position).to_bits(),
        0.875f32.to_bits()
    );
    let (p, trace) = Plan::new(
        1,
        &[RowInput {
            input: 0,
            block_offset: 0,
            block_count: 1,
        }],
        &[BlockInput {
            end: 1024,
            source_count: 1,
            object_gain: 0.5,
            kind: 2,
            flags: JUMP,
            ..BlockInput::default()
        }],
        &[0],
        &[source],
    )
    .unwrap();
    assert_eq!(trace.flags, 0);
    let c = p.coefficients(0, 0).unwrap();
    assert_eq!(c.direct[0], 0.25);
    assert!(c.direct[1..].iter().all(|x| x.to_bits() == 0));
    let p = Arc::new(p);
    let mut encoder = Encoder::new(Arc::clone(&p), 1024, 240, false).unwrap();
    let mut meter = MeterPreprocessor::new(p, 1024, 240).unwrap();
    let input = [0.5; 63];
    let mut encoded = [0.; 63 * 16];
    encoder.process(&input, &mut encoded, 0, 63).unwrap();
    for f in encoded.chunks(16) {
        assert_eq!(f[0].to_bits(), 0.125f32.to_bits());
        assert!(f[1..].iter().all(|x| x.to_bits() == 0));
    }
    let original = encoded;
    let mut decoded = [9.; 63 * 12];
    let mut lfe = [9.; 63];
    meter
        .process(&input, &encoded, &mut decoded, &mut lfe, 0, 63)
        .unwrap();
    assert_eq!(encoded, original);
    assert!(decoded.iter().all(|x| x.to_bits() == 0));
    assert!(lfe.iter().all(|x| x.to_bits() == 0.125f32.to_bits()));
}
#[test]
fn independent_diffuse_impulses_and_wrap() {
    let delays = [
        37, 53, 67, 83, 97, 109, 127, 149, 163, 181, 199, 211, 233, 251, 271, 293, 313, 337, 359,
        383, 409, 431, 457, 487, 521, 557, 593, 631, 673, 719, 761, 809,
    ];
    let signs = [
        1., -1., 1., 1., -1., -1., 1., -1., -1., 1., 1., -1., 1., -1., -1., 1., -1., 1., 1., -1.,
        1., -1., -1., 1., 1., -1., 1., -1., -1., 1., -1., 1.,
    ];
    for start in [0, 1, 1023, 1024, 100037] {
        let mut d = Diffuse::default();
        d.reset(start);
        for f in 0..2200 {
            let mut input = [0.; 16];
            if f == 0 {
                input[7] = 0.5;
            }
            let mut out = [0.125; 16];
            d.add(input, &mut out);
            for (i, v) in out.iter().enumerate() {
                let expected = if i == 7 {
                    0.125
                        + delays
                            .iter()
                            .position(|t| *t == f)
                            .map_or(0., |k| (0.5 * signs[k]) * (1. / 32f32.sqrt()))
                } else {
                    0.125
                };
                assert_eq!(v.to_bits(), expected.to_bits());
            }
        }
    }
}
fn timeline() -> Arc<Plan> {
    let src = [
        Source {
            position: [0., 0., 1.],
            diffuse: 0.4,
            ..source()
        },
        Source {
            position: [90., 0., 1.],
            diffuse: 1.,
            ..source()
        },
        Source {
            position: [-45., 30., 1.],
            diffuse: 0.,
            ..source()
        },
    ];
    let blocks = [
        BlockInput {
            start: 10,
            end: 100,
            source_count: 1,
            object_gain: 1.,
            ..BlockInput::default()
        },
        BlockInput {
            start: 100,
            end: 50,
            source_offset: 1,
            source_count: 1,
            object_gain: 1.,
            ..BlockInput::default()
        },
        BlockInput {
            start: 100,
            end: 5000,
            interpolation: 9000,
            source_offset: 2,
            source_count: 1,
            object_gain: 1.,
            flags: HAS_INTERPOLATION,
            ..BlockInput::default()
        },
    ];
    Arc::new(
        Plan::new(
            1,
            &[RowInput {
                input: 0,
                block_offset: 0,
                block_count: 3,
            }],
            &blocks,
            &[0, 1, 2],
            &src,
        )
        .unwrap()
        .0,
    )
}
#[test]
fn arbitrary_chunks_reset_instances_and_error_atomicity() {
    let plan = timeline();
    let input = signal(4103);
    let mut expected = vec![0.125; input.len() * 16];
    let mut reference = Encoder::new(Arc::clone(&plan), 8192, 240, false).unwrap();
    reference
        .process(&input, &mut expected, 0, input.len())
        .unwrap();
    for n in [1, 7, 31, 37, 255, 809, 1023, 1024, 1537, 2048] {
        let mut encoder = Encoder::new(Arc::clone(&plan), n, 240, false).unwrap();
        let mut actual = vec![0.125; expected.len()];
        for f in (0..input.len()).step_by(n) {
            let end = (f + n).min(input.len());
            encoder
                .process(
                    &input[f..end],
                    &mut actual[f * 16..end * 16],
                    f as u64,
                    end - f,
                )
                .unwrap();
        }
        assert_eq!(actual, expected);
        encoder.reset(0);
        let mut sentinel = [0.125; 16];
        assert!(encoder.process(&[0.5], &mut sentinel, u64::MAX, 1).is_err());
        assert!(encoder.process(&[0.5], &mut sentinel[..15], 0, 1).is_err());
        assert_eq!(sentinel, [0.125; 16]);
        encoder.process(&[], &mut sentinel, 0, 0).unwrap();
        assert_eq!(sentinel, [0.125; 16]);
        encoder.process(&input[..1], &mut sentinel, 0, 1).unwrap();
        assert_eq!(&sentinel, &expected[..16]);
    }
    for start in [37, 1023, 2049] {
        let mut a = Encoder::new(Arc::clone(&plan), 1024, 240, true).unwrap();
        let mut b = Encoder::new(Arc::clone(&plan), 1024, 240, true).unwrap();
        let mut x = vec![0.; 1024 * 16];
        a.process(&input[..1024], &mut x, 0, 1024).unwrap();
        a.reset(start);
        b.reset(start);
        x.fill(0.125);
        let mut y = x.clone();
        a.process(&input[..1024], &mut x, start, 1024).unwrap();
        b.process(&input[..1024], &mut y, start, 1024).unwrap();
        assert_eq!(x, y);
    }
}
#[test]
fn meter_rejections_and_shared_plan_are_independent() {
    let plan = timeline();
    let input = signal(2048);
    let mut encoder = Encoder::new(Arc::clone(&plan), 2048, 240, true).unwrap();
    let mut encoded = vec![0.; 2048 * 16];
    encoder.process(&input, &mut encoded, 0, 2048).unwrap();
    let mut a = MeterPreprocessor::new(Arc::clone(&plan), 2048, 240).unwrap();
    let mut b = MeterPreprocessor::new(plan, 2048, 240).unwrap();
    let mut x = vec![0.125; 2048 * 12];
    let mut y = x.clone();
    let mut xl = vec![0.125; 2048];
    let mut yl = xl.clone();
    assert!(
        a.process(&input, &encoded, &mut x[..7], &mut xl, 0, 2048)
            .is_err()
    );
    assert!(
        a.process(&input, &encoded[..11], &mut x, &mut xl, 0, 2048)
            .is_err()
    );
    assert!(
        a.process(&input, &encoded, &mut x, &mut xl[..3], 0, 2048)
            .is_err()
    );
    assert_eq!(x, y);
    assert_eq!(xl, yl);
    a.process(&input, &encoded, &mut x, &mut xl, 0, 2048)
        .unwrap();
    b.process(&input, &encoded, &mut y, &mut yl, 0, 2048)
        .unwrap();
    assert_eq!(x, y);
    assert_eq!(xl, yl);
    assert!(xl.iter().all(|x| *x == 0.));
    assert!(x.chunks(12).all(|frame| frame[3].to_bits() == 0));
    a.reset(37);
    a.process(
        &input[..7],
        &encoded[..112],
        &mut x[..84],
        &mut xl[..7],
        37,
        7,
    )
    .unwrap();
    b.process(
        &input[..7],
        &encoded[..112],
        &mut y[..84],
        &mut yl[..7],
        37,
        7,
    )
    .unwrap();
    assert_eq!(x, y);
}
#[test]
fn malformed_preparation_and_nonfinite_pcm() {
    let r = [RowInput {
        input: 0,
        block_offset: 0,
        block_count: 1,
    }];
    let b = [BlockInput {
        end: 100,
        source_count: 1,
        object_gain: 1.,
        ..BlockInput::default()
    }];
    assert!(Plan::new(0, &r, &b, &[0], &[source()]).is_err());
    assert!(Plan::new(1, &r, &b, &[1], &[source()]).is_err());
    assert!(
        Plan::new(
            1,
            &r,
            &b,
            &[0],
            &[Source {
                gain: f32::INFINITY,
                ..source()
            }]
        )
        .is_err()
    );
    assert!(Encoder::new(plan(0.), 0, 1, false).is_err());
    assert!(Encoder::new(plan(0.), usize::MAX, 1, false).is_err());
    let p = plan(0.5);
    let mut encoder = Encoder::new(p, 1024, 240, false).unwrap();
    let mut out = [0.; 1024 * 16];
    let mut input = [0.; 1024];
    input[0] = f32::NAN;
    encoder.process(&input, &mut out, 0, 1024).unwrap();
    assert!(out.iter().any(|x| x.is_nan()));
    encoder.reset(0);
    input.fill(0.);
    out.fill(0.);
    encoder.process(&input, &mut out, 0, 1024).unwrap();
    assert!(out.iter().all(|x| *x == 0.));
}

#[test]
fn encoder_and_meter_share_only_immutable_data_across_threads() {
    let sources = [
        Source {
            diffuse: 0.5,
            ..source()
        },
        Source {
            gain: 0.5,
            ..source()
        },
    ];
    let rows = [
        RowInput {
            input: 0,
            block_offset: 0,
            block_count: 1,
        },
        RowInput {
            input: 1,
            block_offset: 1,
            block_count: 1,
        },
    ];
    let blocks = [
        BlockInput {
            end: u64::MAX,
            source_count: 1,
            object_gain: 1.,
            ..BlockInput::default()
        },
        BlockInput {
            end: u64::MAX,
            source_offset: 1,
            source_count: 1,
            object_gain: 1.,
            kind: 2,
            flags: JUMP,
            ..BlockInput::default()
        },
    ];
    let plan = Arc::new(Plan::new(2, &rows, &blocks, &[0, 1], &sources).unwrap().0);
    let input = signal(4096);
    let mut base = Encoder::new(Arc::clone(&plan), 2048, 240, false).unwrap();
    let mut encoded = vec![0.; 2048 * 16];
    base.process(&input, &mut encoded, 0, 2048).unwrap();
    let mut meter = MeterPreprocessor::new(Arc::clone(&plan), 2048, 240).unwrap();
    let mut decoded = vec![0.; 2048 * 12];
    let mut lfe = vec![0.; 2048];
    meter
        .process(&input, &encoded, &mut decoded, &mut lfe, 0, 2048)
        .unwrap();
    std::thread::scope(|scope| {
        let shared = &plan;
        let source = &input;
        let wanted = &encoded;
        let encoding = scope.spawn(move || {
            let mut e = Encoder::new(Arc::clone(shared), 2048, 240, false).unwrap();
            let mut out = vec![0.; 2048 * 16];
            for _ in 0..8 {
                out.fill(0.);
                e.reset(0);
                e.process(source, &mut out, 0, 2048).unwrap();
                assert_eq!(&out, wanted);
            }
        });
        let metering = scope.spawn(|| {
            let mut m = MeterPreprocessor::new(Arc::clone(&plan), 2048, 240).unwrap();
            let mut out = vec![0.; 2048 * 12];
            let mut low = vec![0.; 2048];
            for _ in 0..8 {
                m.process(&input, &encoded, &mut out, &mut low, 0, 2048)
                    .unwrap();
                assert_eq!(out, decoded);
                assert_eq!(low, lfe);
            }
        });
        encoding.join().unwrap();
        metering.join().unwrap();
    });
}
