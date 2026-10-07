//! Platform-independent `sin`/`cos` for f64.
//!
//! The standard library forwards to the platform libm, whose results differ by an ULP between
//! glibc, the Universal CRT and Apple's libm. Coefficient tables built from them (resampler
//! sinc/window tables) therefore differ across platforms. These functions use only IEEE-754
//! additions, subtractions and multiplications — Rust never contracts them into FMA — so the
//! result is bit-identical on every target.
//!
//! The kernels and the medium-range argument reduction are translated from musl
//! (`src/math/__sin.c`, `__cos.c`, `__rem_pio2.c`), which derive from FreeBSD msun; see
//! NOTICE.txt. Accuracy is that of musl: below 1 ULP in practice.
//!
//! Supported domain: finite |x| < 2^20·π/2 (about 1.6e6). The large-argument Payne–Hanek path
//! of musl is not translated; such arguments panic. NaN and infinities return NaN.
#![forbid(unsafe_code)]

/// Largest supported |x|: musl's medium-range reduction is exact below 2^20·π/2.
pub const MAX_ARGUMENT: f64 = 1_647_099.0;

const S1: f64 = f64::from_bits(0xBFC55555_55555549); // -1.66666666666666324348e-1
const S2: f64 = f64::from_bits(0x3F811111_1110F8A6); // 8.33333333332248946124e-3
const S3: f64 = f64::from_bits(0xBF2A01A0_19C161D5); // -1.98412698298579493134e-4
const S4: f64 = f64::from_bits(0x3EC71DE3_57B1FE7D); // 2.75573137070700676789e-6
const S5: f64 = f64::from_bits(0xBE5AE5E6_8A2B9CEB); // -2.50507602534068634195e-8
const S6: f64 = f64::from_bits(0x3DE5D93A_5ACFD57C); // 1.58969099521155010221e-10

const C1: f64 = f64::from_bits(0x3FA55555_5555554C); // 4.16666666666666019037e-2
const C2: f64 = f64::from_bits(0xBF56C16C_16C15177); // -1.38888888888741095749e-3
const C3: f64 = f64::from_bits(0x3EFA01A0_19CB1590); // 2.48015872894767294178e-5
const C4: f64 = f64::from_bits(0xBE927E4F_809C52AD); // -2.75573143513906633035e-7
const C5: f64 = f64::from_bits(0x3E21EE9E_BDB4B1C4); // 2.08757232129817482790e-9
const C6: f64 = f64::from_bits(0xBDA8FAE9_BE8838D4); // -1.13596475577881948265e-11

const TOINT: f64 = 1.5 / f64::EPSILON;
const PIO4: f64 = std::f64::consts::FRAC_PI_4;
const INVPIO2: f64 = f64::from_bits(0x3FE45F30_6DC9C883); // 6.36619772367581382433e-1
const PIO2_1: f64 = f64::from_bits(0x3FF921FB_54400000); // 1.57079632673412561417
const PIO2_1T: f64 = f64::from_bits(0x3DD0B461_1A626331); // 6.07710050650619224932e-11
const PIO2_2: f64 = f64::from_bits(0x3DD0B461_1A600000); // 6.07710050630396597660e-11
const PIO2_2T: f64 = f64::from_bits(0x3BA3198A_2E037073); // 2.02226624879595063154e-21
const PIO2_3: f64 = f64::from_bits(0x3BA3198A_2E000000); // 2.02226624871116645580e-21
const PIO2_3T: f64 = f64::from_bits(0x397B839A_252049C1); // 8.47842766036889956997e-32

/// sin(x + y) for |x + y| <= π/4, with y the tail of a reduced argument.
fn kernel_sin(x: f64, y: f64, has_tail: bool) -> f64 {
    let z = x * x;
    let w = z * z;
    let r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6);
    let v = z * x;
    if has_tail {
        x - ((z * (0.5 * y - v * r) - y) - v * S1)
    } else {
        x + v * (S1 + z * r)
    }
}

/// cos(x + y) for |x + y| <= π/4.
fn kernel_cos(x: f64, y: f64) -> f64 {
    let z = x * x;
    let w = z * z;
    let r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
    let hz = 0.5 * z;
    let w = 1.0 - hz;
    w + (((1.0 - w) - hz) + (z * r - x * y))
}

fn biased_exponent(x: f64) -> i32 {
    ((x.to_bits() >> 52) & 0x7ff) as i32
}

/// x = n·π/2 + (y0 + y1) with |y0 + y1| <= π/4 (musl's medium-range Cody–Waite reduction).
fn reduce(x: f64) -> (i32, f64, f64) {
    assert!(
        x.abs() < MAX_ARGUMENT,
        "mradm_math: |x| must be below 2^20*pi/2"
    );
    let mut fn_ = x * INVPIO2 + TOINT - TOINT;
    let mut r = x - fn_ * PIO2_1;
    let mut w = fn_ * PIO2_1T;
    if r - w < -PIO4 {
        fn_ -= 1.0;
        r = x - fn_ * PIO2_1;
        w = fn_ * PIO2_1T;
    } else if r - w > PIO4 {
        fn_ += 1.0;
        r = x - fn_ * PIO2_1;
        w = fn_ * PIO2_1T;
    }
    let n = fn_ as i32;
    let mut y0 = r - w;
    let ex = biased_exponent(x);
    if ex - biased_exponent(y0) > 16 {
        let t = r;
        w = fn_ * PIO2_2;
        r = t - w;
        w = fn_ * PIO2_2T - ((t - r) - w);
        y0 = r - w;
        if ex - biased_exponent(y0) > 49 {
            let t = r;
            w = fn_ * PIO2_3;
            r = t - w;
            w = fn_ * PIO2_3T - ((t - r) - w);
            y0 = r - w;
        }
    }
    let y1 = (r - y0) - w;
    (n, y0, y1)
}

/// Platform-independent sine. Panics for finite |x| >= [`MAX_ARGUMENT`].
pub fn sin(x: f64) -> f64 {
    if !x.is_finite() {
        return f64::NAN;
    }
    let ax = x.abs();
    if ax <= PIO4 {
        // |x| < 2^-26: sin(x) rounds to x (musl returns x without evaluating the kernel).
        if ax < f64::from_bits(0x3e50_0000_0000_0000) {
            return x;
        }
        return kernel_sin(x, 0.0, false);
    }
    let (n, y0, y1) = reduce(x);
    match n & 3 {
        0 => kernel_sin(y0, y1, true),
        1 => kernel_cos(y0, y1),
        2 => -kernel_sin(y0, y1, true),
        _ => -kernel_cos(y0, y1),
    }
}

/// Platform-independent cosine. Panics for finite |x| >= [`MAX_ARGUMENT`].
pub fn cos(x: f64) -> f64 {
    if !x.is_finite() {
        return f64::NAN;
    }
    let ax = x.abs();
    if ax <= PIO4 {
        // |x| < 2^-27·√2: cos(x) rounds to 1.
        if ax < f64::from_bits(0x3e46_a09e_0000_0000) {
            return 1.0;
        }
        return kernel_cos(x, 0.0);
    }
    let (n, y0, y1) = reduce(x);
    match n & 3 {
        0 => kernel_cos(y0, y1),
        1 => -kernel_sin(y0, y1, true),
        2 => -kernel_cos(y0, y1),
        _ => kernel_sin(y0, y1, true),
    }
}

/// `(sin(x), cos(x))`, each identical to the separate functions.
pub fn sin_cos(x: f64) -> (f64, f64) {
    (sin(x), cos(x))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn constants_match_musl_decimal_literals() {
        // musl spells these constants as decimal literals; they must round to the same bits.
        let table: [(f64, &str); 20] = [
            (S1, "-1.66666666666666324348e-1"),
            (S2, "8.33333333332248946124e-3"),
            (S3, "-1.98412698298579493134e-4"),
            (S4, "2.75573137070700676789e-6"),
            (S5, "-2.50507602534068634195e-8"),
            (S6, "1.58969099521155010221e-10"),
            (C1, "4.16666666666666019037e-2"),
            (C2, "-1.38888888888741095749e-3"),
            (C3, "2.48015872894767294178e-5"),
            (C4, "-2.75573143513906633035e-7"),
            (C5, "2.08757232129817482790e-9"),
            (C6, "-1.13596475577881948265e-11"),
            (INVPIO2, "6.36619772367581382433e-1"),
            (PIO2_1, "1.57079632673412561417"),
            (PIO2_1T, "6.07710050650619224932e-11"),
            (PIO2_2, "6.07710050630396597660e-11"),
            (PIO2_2T, "2.02226624879595063154e-21"),
            (PIO2_3, "2.02226624871116645580e-21"),
            (PIO2_3T, "8.47842766036889956997e-32"),
            (PIO4, "7.85398163397448278999e-01"),
        ];
        for (value, literal) in table {
            assert_eq!(
                value.to_bits(),
                literal.parse::<f64>().unwrap().to_bits(),
                "{literal}"
            );
        }
    }
}
