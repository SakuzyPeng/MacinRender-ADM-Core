use mradm_math::{MAX_ARGUMENT, cos, sin, sin_cos};

fn ulps(a: f64, b: f64) -> u64 {
    let key = |v: f64| {
        let bits = v.to_bits() as i64;
        if bits < 0 { i64::MIN - bits } else { bits }
    };
    key(a).abs_diff(key(b))
}

/// Inputs covering the resampler's window (≤ 8π) and sinc (≤ ~380) arguments and beyond, with exact
/// dyadic values so the inputs themselves are identical everywhere.
fn grid() -> Vec<f64> {
    let mut values = vec![
        0.0, -0.0, 1e-300, 1e-9, 0.5, 1.0, 2.0, 3.0, 1e5, -1e5, 1.6e6,
    ];
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    for scale in [1.0, 8.0, 32.0, 1024.0, 131_072.0] {
        for _ in 0..20_000 {
            state = state
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1);
            let unit = (state >> 11) as f64 / (1u64 << 53) as f64;
            values.push((unit * 2.0 - 1.0) * scale);
        }
    }
    for k in -64..=64 {
        let near = k as f64 * std::f64::consts::FRAC_PI_2;
        values.extend([
            near,
            f64::from_bits(near.to_bits() + 1),
            f64::from_bits(near.to_bits().saturating_sub(1)),
        ]);
    }
    values
}

#[test]
fn agrees_with_platform_libm_within_two_ulps() {
    // Each side is within 1 ULP of the true value, so they differ by at most 2.
    for x in grid() {
        let (s, c) = sin_cos(x);
        assert!(
            ulps(s, x.sin()) <= 2 || (s - x.sin()).abs() < 1e-300,
            "sin({x:e}) = {s:e} vs {:e}",
            x.sin()
        );
        assert!(ulps(c, x.cos()) <= 2, "cos({x:e}) = {c:e} vs {:e}", x.cos());
        assert_eq!((s, c), (sin(x), cos(x)));
    }
}

#[test]
fn special_values() {
    assert_eq!(sin(0.0).to_bits(), 0.0f64.to_bits());
    assert_eq!(sin(-0.0).to_bits(), (-0.0f64).to_bits());
    assert_eq!(cos(0.0), 1.0);
    assert_eq!(cos(-0.0), 1.0);
    for v in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        assert!(sin(v).is_nan() && cos(v).is_nan());
    }
    assert!(std::panic::catch_unwind(|| sin(MAX_ARGUMENT)).is_err());
    assert!(std::panic::catch_unwind(|| cos(-MAX_ARGUMENT * 2.0)).is_err());
}

/// Fixed bit patterns: the default test run on every CI platform checks that the result does
/// not depend on the host libm or instruction set.
#[test]
fn bit_patterns_are_platform_independent() {
    let inputs: [f64; 12] = [
        0.5,
        1.0,
        2.0,
        3.0,
        std::f64::consts::FRAC_PI_4,
        std::f64::consts::FRAC_PI_2,
        std::f64::consts::PI,
        6.25,
        100.0,
        12_345.678_9,
        96_800.125,
        -1.0e5,
    ];
    let actual: Vec<(u64, u64)> = inputs
        .iter()
        .map(|&x| (sin(x).to_bits(), cos(x).to_bits()))
        .collect();
    assert_eq!(actual, EXPECTED, "inputs {inputs:?}");
}

const EXPECTED: &[(u64, u64)] = &[
    (0x3fdeaee8744b05f0, 0x3fec1528065b7d50),
    (0x3feaed548f090cee, 0x3fe14a280fb5068c),
    (0x3fed18f6ead1b446, 0xbfdaa22657537205),
    (0x3fc210386db6d55b, 0xbfefae04be85e5d2),
    (0x3fe6a09e667f3bcc, 0x3fe6a09e667f3bcd),
    (0x3ff0000000000000, 0x3c91a62633145c07),
    (0x3ca1a62633145c07, 0xbff0000000000000),
    (0xbfa0fcddc3f512bc, 0x3feffb7d58a8f975),
    (0xbfe03425b78c4db8, 0x3feb981dbf665fdf),
    (0xbfe68298a1cec146, 0x3fe6be7c89fe4a8e),
    (0x3fef5ee9d617daf1, 0x3fc942461ad45466),
    (0xbfa24daa9c527e96, 0xbfeffac3841b3da7),
];
