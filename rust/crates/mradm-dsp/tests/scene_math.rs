use mradm_dsp::scene_math::*;

#[test]
fn fixed_sum_order_and_axes() {
    let f = f32::from_bits;
    assert_eq!(
        length([f(0x3f800000), f(0x39b5016c), f(0x368ef881)]).to_bits(),
        0x3f800001
    );
    assert_eq!(
        length([f(0x368ef881), f(0x39b5016c), f(0x3f800000)]).to_bits(),
        0x3f800000
    );
    assert_eq!(length([3., 4., 0.]), 5.);
    assert_eq!(cartesian_to_polar([1., 0., 0.]), [-90., 0., 1.]);
    assert_eq!(cartesian_to_polar([0., 1., 0.])[2], 1.);
    assert_eq!(cartesian_to_polar([0., 0., 1.])[1], 90.);
    assert_eq!(direction(0., 0.), [-0., 1., 0.]);
    assert_eq!(distance([1., 0., 0.], [0., 1., 0.]), 2f32.sqrt());
}
#[test]
fn cross_product_preserves_the_selected_contraction_policy() {
    // (1 + 2^-23) * (1 - 2^-23) - 1 is exactly -2^-46. Separate
    // multiplication rounds the product to one, losing the cross component.
    let a = [f32::from_bits(0x3f800001), 1., 0.];
    let b = [1., f32::from_bits(0x3f7ffffe), 0.];
    assert_eq!(cross_compat(a, b, true)[2].to_bits(), 0xa8800000);
    assert_eq!(cross_compat(a, b, false)[2].to_bits(), 0);
    assert_eq!(cross(a, b)[2].to_bits(), 0);
}
#[test]
fn seams_ties_signed_zero_and_routes() {
    for (x, y) in [
        (180., 180.),
        (540., -180.),
        (900., 180.),
        (-180., -180.),
        (-540., 180.),
        (-900., -180.),
    ] {
        assert_eq!(remainder_degrees(x), y);
    }
    assert_eq!(remainder_degrees(-0.).to_bits(), (-0f32).to_bits());
    assert_eq!(wrap(-180.), 180.);
    let speakers = [
        Speaker {
            azimuth: 30.,
            elevation: 0.,
            is_lfe: 0,
        },
        Speaker {
            azimuth: -30.,
            elevation: 0.,
            is_lfe: 0,
        },
        Speaker {
            azimuth: 0.,
            elevation: 0.,
            is_lfe: 1,
        },
    ];
    assert_eq!(nearest([0., 0., 1.], false, &speakers).unwrap().0, 0);
    assert!(nearest([0., 0., 1.], false, &[]).is_none());
}
#[test]
fn extent_slots_weights_and_divergence() {
    let mut points = [CloudPoint::default(); 17];
    assert_eq!(
        cloud(
            [0., 0., 1.],
            false,
            [0.; 3],
            0.5,
            false,
            false,
            &mut points,
            None
        ),
        1
    );
    assert_eq!(points[0].weight, 0.5);
    assert_eq!(
        cloud(
            [0., 90., 1.],
            false,
            [0.5; 3],
            1.,
            false,
            false,
            &mut points,
            None
        ),
        16
    );
    assert_eq!(points[0].slot, 1);
    assert_eq!(points[15].slot, 16);
    assert!((points[..16].iter().map(|p| p.weight).sum::<f32>() - 1.).abs() < 1e-6);
    let mut branches = [CloudPoint::default(); 3];
    assert_eq!(
        divergence([0., 0., 1.], false, 1., 30., 0., 1., &mut branches),
        3
    );
    assert_eq!(branches.map(|p| p.azimuth), [-30., 0., 30.]);
    assert_eq!(branches.map(|p| p.weight), [0.5, 0., 0.5]);
}
#[test]
fn rotations_posebridge_poles_and_invalid_pose() {
    let head = Rotation::new([30., 0., 0.]).unwrap();
    assert!((head.apply(0., 0., false)[0] + 30.).abs() < 1e-5);
    assert!((head.apply(0., 0., true)[0] + 30.).abs() < 1e-5);
    assert!(Rotation::new([f32::NAN, 0., 0.]).is_err());
    assert!(pose(&[0.; 4], true).is_err());
    assert_eq!(pose(&[0.; 3], false).unwrap(), [0., 0., 0., 1., 0., 0., 0.]);
    for pitch in [-90., -89., 0., 89., 90.] {
        let p = pose(&[30., pitch, 20.], false).unwrap();
        let roundtrip = pose(&p[..4], true).unwrap();
        assert!((p[5] - roundtrip[5]).abs() < 0.001);
        assert!(roundtrip.iter().all(|x| x.is_finite()));
    }
    assert!(
        pose(&[f32::MAX, -f32::MAX, 0.], false)
            .unwrap()
            .iter()
            .all(|x| x.is_finite())
    );
}

/// The loudspeaker backends' direction length must be bit-reproducible (formerly
/// `tests/unit/render_common_numeric_test.cpp`; see `CONSISTENCY_LOCALIZATION.md`). The first
/// three-platform baseline split `hoa-hoa3-point` here: `std::hypot` has no precision guarantee,
/// Darwin returned exactly 1.0 for a unit vector while glibc / UCRT returned 1 - 1 ulp. The
/// expected bits follow from IEEE-754 (exact squares in binary64, correctly rounded sum, sqrt and
/// narrowing), not from one machine's output. The inputs are given as bits and pass through
/// `black_box`, so the release build evaluates the path instead of folding it.
#[test]
fn vector_lengths_are_correctly_rounded() {
    use std::hint::black_box;
    let f = |bits: u32| black_box(f32::from_bits(bits));
    let cases: [(u32, u32, u32, u32, &str); 8] = [
        (
            0x3F5DB3D7,
            0x3F000000,
            0,
            0x3F800000,
            "az=30 el=0, where hypot split",
        ),
        (0xBEAF1D44, 0x3F708FB2, 0, 0x3F800000, "az=110 el=0"),
        (0, 0x3F800000, 0, 0x3F800000, "unit +Y"),
        (0x40400000, 0x40800000, 0, 0x40A00000, "3, 4, 0: exactly 5"),
        (
            0x40000000,
            0x40400000,
            0x40C00000,
            0x40E00000,
            "2, 3, 6: exactly 7",
        ),
        (0, 0, 0, 0, "zero vector"),
        (
            0x7E967699,
            0x7E967699,
            0,
            0x7ED4C986,
            "near f32::MAX: the f64 sum does not overflow",
        ),
        (1, 2, 0, 2, "subnormal: the f64 sum does not underflow"),
    ];
    for (x, y, z, expected, what) in cases {
        assert_eq!(length([f(x), f(y), f(z)]).to_bits(), expected, "{what}");
    }
    // A direction whose length is exactly 1.0 must normalise to itself.
    let (x, y) = (f(0x3F5DB3D7), f(0x3F000000));
    let len = length([x, y, 0.]);
    assert_eq!(len.to_bits(), 0x3F800000);
    assert_eq!((x / len).to_bits(), 0x3F5DB3D7);
    assert_eq!((y / len).to_bits(), 0x3F000000);
}
