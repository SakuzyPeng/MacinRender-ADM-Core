//! Parameter entry points (PCM consistency 3.5): ADM dB gains and Cartesian DirectSpeakers
//! projections on import, and the locale-independent decimal parser behind the command line.
use super::{doubles, floats};
use mradm_adm::numeric::{cartesian_to_polar, gain_from_db};
use mradm_dsp::decimal::{parse_f32, parse_f64};
use std::path::Path;

pub(super) fn run(out: &Path) {
    let mut gains: Vec<f32> = (-2400..=480)
        .map(|step| gain_from_db(f64::from(step) * 0.05))
        .collect();
    gains.extend([-200.0, -96.25, -0.001, 0.001, 47.5].map(gain_from_db));
    floats(out, "adm-import.10-gain.f32", &gains);

    let axis = |i: i32| -1.0 + i as f32 * 0.125;
    let mut polar = Vec::new();
    for z in 0..17 {
        for y in 0..17 {
            for x in 0..17 {
                polar.extend(cartesian_to_polar(axis(x), axis(y), axis(z)));
            }
        }
    }
    floats(out, "adm-import.20-cartesian.f32", &polar);

    // Decimal texts at and around rounding boundaries, with signs, exponents, separators and
    // whitespace: each yields its binary64 value and its directly rounded binary32 value.
    let texts = [
        "0",
        "-0",
        "0.1",
        "-23",
        "-23.5",
        " -1.0 ",
        "1e-3",
        "+2.25",
        ".5",
        "5.",
        "1_000.125",
        "37.000001",
        "0.30000000000000004",
        "1.000000059604644775390625",
        "1.000000059604644775390625000000000001",
        "1.0000000596046447753906249999999999",
        "3.4028235677973366e38",
        "1.401298464324817e-45",
        "7.006492321624085e-46",
        "2.2250738585072011e-308",
        "4.9406564584124654e-324",
        "123456789012345678901234567890",
        "0.000000000000000000000000000001",
        "-179.99999999999999999",
        "89.99999",
    ];
    let mut parsed = Vec::new();
    for text in texts {
        parsed.push(parse_f64(text).unwrap());
        parsed.push(f64::from(parse_f32(text).unwrap()));
    }
    doubles(out, "decimal.10-parse.f64", &parsed);
}
