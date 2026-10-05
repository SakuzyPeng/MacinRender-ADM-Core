use mradm_dsp::pcm_mix::{
    Block, Coefficients, HAS_INTERPOLATION, JUMP, Matrix, Mixer, Plan, Row, SMOOTH,
};
use std::sync::Arc;

fn block(start: u64, end: u64, interpolation: u64, flags: u32) -> Block {
    Block {
        start,
        end,
        interpolation,
        flags,
    }
}
fn plan(blocks: Vec<Block>, gains: Vec<f32>) -> Arc<Plan> {
    let row = Row {
        input_channel: 0,
        block_offset: 0,
        block_count: blocks.len(),
        output_gain: 0.5,
    };
    Arc::new(Plan::new(1, 1, vec![row], blocks, Coefficients::Speaker(gains)).unwrap())
}

#[test]
fn timeline_gaps_duplicates_clamping_live_gains_and_partitioning() {
    let p = plan(
        vec![
            block(0, 2, 0, JUMP),
            block(4, 9, 99, HAS_INTERPOLATION),
            block(6, 6, 0, JUMP),
            block(6, 10, 0, JUMP),
            block(12, 20, 0, 0),
        ],
        vec![1., 2., 99., 3., 4.],
    );
    let mut whole = Mixer::new(Arc::clone(&p), 32, 2, false).unwrap();
    let mut split = Mixer::new(p, 32, 2, false).unwrap();
    let expected = [
        1., 1., 0., 0., 1., 1.5, 3., 3., 3., 3., 0., 0., 3., 3.5, 4., 4.,
    ];
    let mut a = [0.25; 16];
    whole
        .speaker(&[1.; 16], &mut a, &[2.; 16], 0, 16, None, None)
        .unwrap();
    for (actual, gain) in a.iter().zip(expected) {
        assert_eq!(*actual, gain + 0.25);
    }
    let mut b = [0.25; 16];
    for start in (0..16).step_by(3) {
        let end = (start + 3).min(16);
        split
            .speaker(
                &[1.; 3][..end - start],
                &mut b[start..end],
                &[2.; 3][..end - start],
                start as u64,
                end - start,
                None,
                None,
            )
            .unwrap();
    }
    assert_eq!(a, b);
    split.reset();
    let mut window = [0.25; 4];
    split
        .speaker(&[1.; 4], &mut window, &[2.; 4], 12, 4, None, None)
        .unwrap();
    assert_eq!(window, a[12..]);
}

#[test]
fn block_endpoint_smoothing_and_single_row_keep_distinct_semantics() {
    let p = plan(
        vec![block(0, 3, 0, JUMP | SMOOTH), block(4, 8, 0, JUMP | SMOOTH)],
        vec![1., 3.],
    );
    let mut smooth = Mixer::new(Arc::clone(&p), 8, 0, true).unwrap();
    let mut plain = Mixer::new(p, 8, 0, true).unwrap();
    let mut a = [0.; 8];
    let mut b = a;
    smooth
        .speaker(&[1.; 8], &mut a, &[], 0, 8, None, Some(1.))
        .unwrap();
    plain
        .speaker(&[1.; 8], &mut b, &[], 0, 8, Some(0), Some(1.))
        .unwrap();
    assert_eq!(b, [1., 1., 1., 0., 3., 3., 3., 3.]);
    for (i, &v) in a.iter().enumerate() {
        let alpha = i as f32 / 7.;
        assert_eq!(v, 1. - alpha + 3. * alpha);
    }
    // A zero-frame call at the far end must not move the cursor.
    plain.reset();
    plain
        .speaker(&[], &mut [], &[], u64::MAX, 0, None, None)
        .unwrap();
    let mut first = [0.];
    plain
        .speaker(&[1.], &mut first, &[], 0, 1, None, None)
        .unwrap();
    assert_eq!(first, [0.5]);
}

#[test]
fn ear_precision_order_and_sparse_nonfinite_behavior() {
    let ulp = 2.0_f64.powi(-23);
    let previous = 1. + 0.49 * ulp;
    let current = 1. + 0.99 * ulp;
    let p = Arc::new(
        Plan::new(
            1,
            1,
            vec![Row {
                input_channel: 0,
                block_offset: 0,
                block_count: 2,
                output_gain: 1.,
            }],
            vec![
                block(0, 1, 0, JUMP | SMOOTH),
                block(1, 8, 2, HAS_INTERPOLATION | SMOOTH),
            ],
            Coefficients::Ear(vec![previous, 0., current, 0.]),
        )
        .unwrap(),
    );
    let mut exact = Mixer::new(Arc::clone(&p), 8, 0, false).unwrap();
    let mut smooth = Mixer::new(p, 8, 0, true).unwrap();
    let mut d = [0.];
    let mut f = [0.];
    exact.ear(&[1.], &mut d, &mut f, 2, 1).unwrap();
    assert_eq!(d[0].to_bits(), 1.0_f32.to_bits() + 1);
    smooth.ear(&[1.], &mut d, &mut f, 2, 1).unwrap();
    assert_eq!(d, [1.]);
    assert_eq!(f, [0.]);
    // Static EAR zeros are skipped; generic and fixed matrices still multiply zero.
    exact.reset();
    exact.ear(&[f32::NAN], &mut d, &mut f, 0, 1).unwrap();
    assert!(d[0].is_nan());
    assert_eq!(f, [0.]);
    exact.ear(&[f32::NAN], &mut d, &mut f, 2, 1).unwrap();
    assert!(f[0].is_nan());
}

#[test]
fn dynamic_updates_are_bounded_atomic_and_resettable() {
    let mut m = Mixer::dynamic(2, 2, &[1], 3, 1024, 0).unwrap();
    let curve = [block(0, 8, 0, JUMP)];
    m.update(0, curve.into_iter(), &[1., 2.], 0.5).unwrap();
    assert!(
        m.update(0, [curve[0]; 4].into_iter(), &[0.; 8], 1.)
            .is_err()
    );
    assert!(m.update(0, curve.into_iter(), &[0., f32::NAN], 1.).is_err());
    assert!(
        m.update(
            0,
            [block(4, 8, 0, 0), block(2, 8, 0, 0)].into_iter(),
            &[0.; 4],
            1.
        )
        .is_err()
    );
    assert!(m.update(1, curve.into_iter(), &[0.; 2], 1.).is_err());
    let mut out = [0.; 4];
    m.speaker(&[99., 1., 99., 2.], &mut out, &[], 0, 2, Some(0), None)
        .unwrap();
    assert_eq!(out, [0.5, 1., 1., 2.]);
    let mut untouched = [42.; 4];
    assert!(
        m.speaker(&[0.; 4], &mut untouched, &[], u64::MAX, 2, Some(0), None)
            .is_err()
    );
    assert!(
        m.speaker(&[0.; 4], &mut untouched, &[0.; 1], 0, 2, Some(0), None)
            .is_err()
    );
    assert_eq!(untouched, [42.; 4]);
    m.reset();
    m.update(0, [block(5, 9, 0, JUMP)].into_iter(), &[3., 4.], 1.)
        .unwrap();
    out.fill(0.);
    m.speaker(&[0., 1., 0., 1.], &mut out, &[], 4, 2, Some(0), None)
        .unwrap();
    assert_eq!(out, [0., 0., 3., 4.]);
    // Reuse a curve for the same window, as the dry point branch does during size transitions.
    m.update(
        0,
        [block(0, 4, 0, JUMP), block(4, 8, 0, JUMP)].into_iter(),
        &[1., 2., 3., 4.],
        0.5,
    )
    .unwrap();
    let input = [1.; 16];
    let mut first = [0.; 16];
    let mut again = [0.; 16];
    m.speaker(&input, &mut first, &[], 0, 8, Some(0), None)
        .unwrap();
    m.speaker(&input, &mut again, &[], 0, 8, Some(0), None)
        .unwrap();
    assert_eq!(first, again);
}

#[test]
fn shared_table_lifetime_independent_cursors_and_fixed_matrix() {
    let p = plan(
        vec![block(0, 4, 0, JUMP), block(4, 8, 0, JUMP)],
        vec![1., 2.],
    );
    let mut a = Mixer::new(Arc::clone(&p), 8, 0, false).unwrap();
    let mut b = Mixer::new(p, 8, 0, false).unwrap();
    let mut out = [0.];
    a.speaker(&[1.], &mut out, &[], 4, 1, None, None).unwrap();
    assert_eq!(out, [1.]);
    out.fill(0.);
    b.speaker(&[1.], &mut out, &[], 0, 1, None, None).unwrap();
    assert_eq!(out, [0.5]);
    a.reset();
    out.fill(0.);
    a.speaker(&[1.], &mut out, &[], 0, 1, None, None).unwrap();
    assert_eq!(out, [0.5]);
    let matrix = Matrix::new(4, 2, &[1., 0., 1., 0., 0., 1., 0., 1.]).unwrap();
    let mut output = [42.; 6];
    matrix
        .process(&[1., 2., 3., 4., 5., 6., 7., 8.], &mut output, 2)
        .unwrap();
    assert_eq!(output, [4., 6., 12., 14., 42., 42.]);
    let before = output;
    assert!(matrix.process(&[1.; 7], &mut output, 2).is_err());
    assert_eq!(output, before);
    assert!(Matrix::new(usize::MAX, 2, &[]).is_err());
    assert!(Matrix::new(1, 1, &[f32::INFINITY]).is_err());
    let zeros = Matrix::new(1, 1, &[0.]).unwrap();
    zeros.process(&[f32::NAN], &mut output, 1).unwrap();
    assert!(output[0].is_nan());

    let identity = Matrix::new(2, 2, &[1., 0., 0., 1.]).unwrap();
    let exact = [0.125_f32, -0.5, 2., -4.];
    let mut actual = [0.; 4];
    identity.process(&exact, &mut actual, 2).unwrap();
    for (a, b) in actual.iter().zip(exact) {
        assert_eq!(a.to_bits(), b.to_bits());
    }
}
