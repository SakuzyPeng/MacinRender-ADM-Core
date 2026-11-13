//! The value conversions of the command-line parser the CLI used before (CLI11 2.5.0
//! `integral_conversion`, `to_flag_value` and the boolean `lexical_cast`), so that the same option
//! values are accepted and rejected: unsigned options take `strtoull` base-0 text (decimal, `0x`
//! hex, leading-`0` octal), digit separators, trailing blanks and `0o` / `0b` prefixes.

/// `isspace` in the C locale.
fn blank(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// `strtoull` / `strtoll`: the magnitude, whether a minus sign was read, the bytes consumed (0 when
/// no digits) and whether the magnitude overflowed `limit`.
struct Scan {
    magnitude: u128,
    negative: bool,
    end: usize,
    overflow: bool,
}

fn scan(text: &[u8], base: u32, limit: u128) -> Scan {
    let none = Scan {
        magnitude: 0,
        negative: false,
        end: 0,
        overflow: false,
    };
    let mut i = 0;
    while i < text.len() && blank(text[i]) {
        i += 1;
    }
    let mut negative = false;
    if i < text.len() && (text[i] == b'+' || text[i] == b'-') {
        negative = text[i] == b'-';
        i += 1;
    }
    let digit = |b: u8| (b as char).to_digit(36).filter(|&d| d < 36);
    let mut base = base;
    if base == 0 {
        let hex = i + 1 < text.len()
            && text[i] == b'0'
            && (text[i + 1] | 0x20) == b'x'
            && text
                .get(i + 2)
                .and_then(|&b| digit(b))
                .is_some_and(|d| d < 16);
        let binary = i + 1 < text.len()
            && text[i] == b'0'
            && (text[i + 1] | 0x20) == b'b'
            && text
                .get(i + 2)
                .and_then(|&b| digit(b))
                .is_some_and(|d| d < 2);
        if hex {
            base = 16;
            i += 2;
        } else if binary {
            base = 2;
            i += 2;
        } else if i < text.len() && text[i] == b'0' {
            base = 8;
        } else {
            base = 10;
        }
    }
    let start = i;
    let mut magnitude: u128 = 0;
    let mut overflow = false;
    while i < text.len() {
        let Some(d) = digit(text[i]).filter(|&d| d < base) else {
            break;
        };
        magnitude = magnitude * u128::from(base) + u128::from(d);
        if magnitude > limit {
            overflow = true;
            magnitude = limit + 1;
        }
        i += 1;
    }
    if i == start {
        return none;
    }
    Scan {
        magnitude,
        negative,
        end: i,
        overflow,
    }
}

/// `std::strtoull(text, &end, base)`: the value (wrapped by a minus sign), the end and ERANGE.
fn strtoull(text: &[u8], base: u32) -> (u64, usize, bool) {
    let s = scan(text, base, u128::from(u64::MAX));
    if s.overflow {
        return (u64::MAX, s.end, true);
    }
    let value = s.magnitude as u64;
    (
        if s.negative {
            value.wrapping_neg()
        } else {
            value
        },
        s.end,
        false,
    )
}

/// `std::strtoll(text, &end, base)`.
fn strtoll(text: &[u8], base: u32) -> (i64, usize, bool) {
    let s = scan(text, base, u128::from(i64::MAX as u64) + 1);
    let max = if s.negative {
        i64::MAX as u128 + 1
    } else {
        i64::MAX as u128
    };
    if s.overflow || s.magnitude > max {
        return (if s.negative { i64::MIN } else { i64::MAX }, s.end, true);
    }
    let value = if s.negative {
        (s.magnitude as i128).wrapping_neg() as i64
    } else {
        s.magnitude as i64
    };
    (value, s.end, false)
}

/// `integral_conversion` into `uint32_t` (also the `unsigned int` of a `CLI::Range`).
pub(crate) fn uint32(input: &[u8]) -> Option<u32> {
    if input.is_empty() || input[0] == b'-' {
        return None;
    }
    let (wide, end, range) = strtoull(input, 0);
    if range {
        return None;
    }
    if end == input.len() && u32::try_from(wide).is_ok() {
        return Some(wide as u32);
    }
    let (signed, end, _) = strtoll(input, 0);
    if end == input.len() {
        let output = if signed < 0 { 0 } else { signed as u32 };
        return (i64::from(output) == signed).then_some(output);
    }
    if input.iter().any(|&b| b == b'_' || b == b'\'') {
        let stripped: Vec<u8> = input
            .iter()
            .copied()
            .filter(|&b| b != b'_' && b != b'\'')
            .collect();
        return uint32(&stripped);
    }
    if input.last().copied().is_some_and(blank) {
        let first = input.iter().position(|&b| !blank(b)).unwrap_or(input.len());
        let last = input
            .iter()
            .rposition(|&b| !blank(b))
            .map_or(first, |p| p + 1);
        return uint32(&input[first..last.max(first)]);
    }
    for (prefix, base) in [(b'o', 8), (b'b', 2)] {
        if input.len() >= 2 && input[0] == b'0' && (input[1] | 0x20) == prefix {
            let rest = &input[2..];
            let (wide, end, range) = strtoull(rest, base);
            if range {
                return None;
            }
            return (end == rest.len() && u32::try_from(wide).is_ok()).then_some(wide as u32);
        }
    }
    None
}

/// `to_flag_value`: the value and whether `errno` is EINVAL (`Err(false)`) or ERANGE (`Err(true)`).
fn flag_value(input: &[u8]) -> Result<i64, bool> {
    if input == b"true" {
        return Ok(1);
    }
    if input == b"false" {
        return Ok(-1);
    }
    let lower = input.to_ascii_lowercase();
    if lower.len() == 1 {
        return match lower[0] {
            b @ b'1'..=b'9' => Ok(i64::from(b - b'0')),
            b'0' | b'f' | b'n' | b'-' => Ok(-1),
            b't' | b'y' | b'+' => Ok(1),
            _ => Err(false),
        };
    }
    match &lower[..] {
        b"true" | b"on" | b"yes" | b"enable" => Ok(1),
        b"false" | b"off" | b"no" | b"disable" => Ok(-1),
        _ => {
            let (value, end, range) = strtoll(&lower, 0);
            if range {
                Err(true)
            } else if end != lower.len() {
                Err(false)
            } else {
                Ok(value)
            }
        }
    }
}

/// `lexical_cast<bool>`; `None` where the conversion failed.
pub(crate) fn boolean(input: &[u8]) -> Option<bool> {
    match flag_value(input) {
        Ok(value) => Some(value > 0),
        Err(true) => Some(input.first() != Some(&b'-')),
        Err(false) => None,
    }
}

/// The text a flag occurrence stores (`Option::get_flag_value`): an empty (or `{}`) value is the
/// flag name's default; a name whose default is `false` stores the negated value when it converts.
pub(crate) fn flag_text(value: &[u8], default_false: bool) -> Vec<u8> {
    if value.is_empty() || value == b"{}" {
        return if default_false {
            b"false".to_vec()
        } else {
            b"true".to_vec()
        };
    }
    if !default_false {
        return value.to_vec();
    }
    match flag_value(value) {
        Ok(1) => b"false".to_vec(),
        Ok(-1) => b"true".to_vec(),
        Ok(v) => (-(v as i128)).to_string().into_bytes(),
        Err(_) => value.to_vec(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unsigned_text_converts_as_cli11_did() {
        for (text, value) in [
            ("5", Some(5)),
            ("+5", Some(5)),
            (" 7", Some(7)),
            ("7 ", Some(7)),
            ("0x10", Some(16)),
            ("010", Some(8)),
            ("1_0", Some(10)),
            ("1'0", Some(10)),
            ("0o17", Some(15)),
            ("0b11", Some(3)),
            ("4294967295", Some(u32::MAX)),
            ("4294967296", None),
            ("-1", None),
            (" -1", None),
            ("1.5", None),
            ("fast", None),
            ("", None),
            ("0x", None),
            ("99999999999999999999999", None),
        ] {
            assert_eq!(uint32(text.as_bytes()), value, "{text:?}");
        }
    }

    #[test]
    fn booleans_and_flags_convert_as_cli11_did() {
        for (text, value) in [
            ("true", Some(true)),
            ("FALSE", Some(false)),
            ("on", Some(true)),
            ("Off", Some(false)),
            ("2", Some(true)),
            ("-5", Some(false)),
            ("0", Some(false)),
            ("y", Some(true)),
            ("", Some(false)),
            ("maybe", None),
            ("x", None),
            ("99999999999999999999", Some(true)),
        ] {
            assert_eq!(boolean(text.as_bytes()), value, "{text:?}");
        }
        assert_eq!(flag_text(b"", false), b"true");
        assert_eq!(flag_text(b"", true), b"false");
        assert_eq!(flag_text(b"true", true), b"false");
        assert_eq!(flag_text(b"no", true), b"true");
        assert_eq!(flag_text(b"2", true), b"-2");
        assert_eq!(flag_text(b"x", true), b"x");
        assert_eq!(flag_text(b"x", false), b"x");
    }
}
