//! fmt / printf number formatting the C++ engine's messages used.

/// `{:.<precision>g}`: `precision` significant digits (at least one), exponent form when the
/// decimal exponent is below -4 or not below the precision, trailing zeros and a bare point
/// removed, a two-digit minimum exponent (`1e+06`), and `nan` / `inf` with their sign.
pub fn general(value: f64, precision: usize) -> String {
    let sign = if value.is_sign_negative() { "-" } else { "" };
    if value.is_nan() {
        return format!("{sign}nan");
    }
    if value.is_infinite() {
        return format!("{sign}inf");
    }
    if value == 0.0 {
        return format!("{sign}0");
    }
    let p = precision.max(1);
    // The exponent after rounding to `p` significant digits decides the style, as in C.
    let scientific = format!("{:.*e}", p - 1, value);
    let (mantissa, exponent) = scientific.split_once('e').expect("exponent form");
    let exponent: i32 = exponent.parse().expect("decimal exponent");
    if exponent < -4 || exponent >= p as i32 {
        let mantissa = trim_zeros(mantissa);
        let sign = if exponent < 0 { '-' } else { '+' };
        format!("{mantissa}e{sign}{:02}", exponent.unsigned_abs())
    } else {
        let decimals = (p as i32 - 1 - exponent) as usize;
        trim_zeros(&format!("{value:.decimals$}")).to_owned()
    }
}

fn trim_zeros(text: &str) -> &str {
    if text.contains('.') {
        text.trim_end_matches('0').trim_end_matches('.')
    } else {
        text
    }
}

/// `{:.<precision>f}` (`{:+.<precision>f}` with `plus`): the correctly rounded decimal (exact
/// ties to even), and `nan` / `inf` with their sign as fmt writes them.
pub fn fixed(value: f64, precision: usize, plus: bool) -> String {
    let sign = if value.is_sign_negative() {
        "-"
    } else if plus {
        "+"
    } else {
        ""
    };
    if value.is_nan() {
        return format!("{sign}nan");
    }
    if value.is_infinite() {
        return format!("{sign}inf");
    }
    format!("{sign}{:.*}", precision, value.abs())
}
