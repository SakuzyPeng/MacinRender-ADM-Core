//! Locale-independent decimal parameters (PCM consistency 3.5, audit N01).
//!
//! `str::parse` rounds correctly on every platform. The C library routes the CLI used before go
//! through `strtold`, whose `long double` is 64-bit on macOS arm64 and Windows, 80-bit on Linux
//! x64 and 128-bit on Linux arm64, so the same text could round twice in different ways and also
//! followed the process locale's decimal point. Binary32 parameters are parsed directly, so the
//! text is rounded once.

fn digits(text: &str) -> Option<String> {
    let text = text.trim_ascii();
    if text.is_empty() {
        return None;
    }
    // Digit separators, as the command-line parser accepted them.
    Some(text.chars().filter(|c| !matches!(c, '_' | '\'')).collect())
}

/// A finite binary64 value from decimal text; surrounding ASCII whitespace is ignored.
pub fn parse_f64(text: &str) -> Option<f64> {
    digits(text)?.parse::<f64>().ok().filter(|v| v.is_finite())
}

/// A finite binary32 value from decimal text, rounded once from the decimal value.
pub fn parse_f32(text: &str) -> Option<f32> {
    digits(text)?.parse::<f32>().ok().filter(|v| v.is_finite())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_plain_decimals_and_rejects_the_rest() {
        assert_eq!(parse_f64(" -23.5 "), Some(-23.5));
        assert_eq!(parse_f64("1_000.25"), Some(1000.25));
        assert_eq!(parse_f64("+1e-3"), Some(1e-3));
        assert_eq!(parse_f32("0.1"), Some(0.1f32));
        for bad in [
            "",
            " ",
            "nan",
            "inf",
            "-infinity",
            "1e999",
            "1.5x",
            "0x10",
            "1,5",
        ] {
            assert_eq!(parse_f64(bad), None, "{bad}");
        }
        assert_eq!(parse_f32("1e39"), None);
    }

    #[test]
    fn binary32_is_rounded_once_from_the_decimal_text() {
        // Halfway between two binary32 neighbours after the first rounding to binary64: the
        // two-step route rounds to even at the second step, the direct parse does not.
        let text = "1.000000059604644775390625000000000001";
        let direct = parse_f32(text).unwrap();
        let twice = parse_f64(text).unwrap() as f32;
        assert_eq!(direct.to_bits(), 0x3f80_0001);
        assert_eq!(twice.to_bits(), 0x3f80_0000);
    }
}
