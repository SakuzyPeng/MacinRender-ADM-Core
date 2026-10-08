//! AutoEq ParametricEQ text import. The input is user-supplied text (a file or a GUI paste),
//! decoded as bytes in the C locale: ASCII whitespace and case folding only, `.` decimal point.
//! The accepted grammar is the frozen C++ parser's (`tests/reference/hptf_parse/`); range
//! validation of the parsed bands stays with the caller.
use super::{Band, BandKind};
use std::ops::Range;

const DEFAULT_Q: f64 = 0.707;

#[derive(Clone, Debug, PartialEq)]
pub struct Profile {
    pub preamp_db: f64,
    pub bands: Vec<Band>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParseErrorKind {
    /// A `Preamp` line without a parseable value.
    Preamp,
    /// A filter line whose `Fc` keyword is not followed by a number.
    Frequency,
    /// A filter line whose `Gain` or `Q` keyword is not followed by a number.
    GainOrQ,
    /// No Preamp or usable filter line at all.
    Empty,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParseError {
    pub kind: ParseErrorKind,
    /// The offending line without surrounding whitespace; empty for `Empty`.
    pub line: Range<usize>,
}

/// `isspace` in the C locale, which (unlike `u8::is_ascii_whitespace`) includes `\v`.
fn space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | b'\x0B' | b'\x0C' | b'\r')
}

fn trim(text: &[u8], range: Range<usize>) -> Range<usize> {
    let mut start = range.start;
    let mut end = range.end;
    while start < end && space(text[start]) {
        start += 1;
    }
    while end > start && space(text[end - 1]) {
        end -= 1;
    }
    start..end
}

fn tokens(line: &[u8]) -> Vec<&[u8]> {
    line.split(|&b| space(b))
        .filter(|t| !t.is_empty())
        .collect()
}

/// The whole token must be one finite decimal number; units are separate tokens in AutoEq,
/// so a partial number must not silently change a curve.
fn number(token: &[u8]) -> Option<f64> {
    let text = std::str::from_utf8(token).ok()?;
    // `f64::from_str` also takes inf/NaN spellings; those are non-finite and rejected below,
    // exactly as the old stream extraction failed on them.
    let first = *token.first()?;
    if !(first.is_ascii_digit() || matches!(first, b'+' | b'-' | b'.')) {
        return None;
    }
    let value: f64 = text.parse().ok()?;
    if !value.is_finite() {
        return None;
    }
    // A nonzero literal that rounds to zero or a subnormal is rejected. The old stream
    // extraction disagreed across platforms here: libstdc++ took it as 0, while libc++ and
    // MSVC failed it with ERANGE; rejecting keeps a typo from silently zeroing a parameter.
    let mantissa = token
        .split(|&b| b == b'e' || b == b'E')
        .next()
        .unwrap_or_default();
    if (value == 0. || value.is_subnormal()) && mantissa.iter().any(|b| (b'1'..=b'9').contains(b)) {
        return None;
    }
    Some(value)
}

/// The first number after `key` anywhere on the line, so AutoEq LP/HP/BP lines that omit
/// `Gain` still parse. `Err` when the keyword is present without a number after it.
fn value_after(tokens: &[&[u8]], key: &str, fallback: f64) -> Result<f64, ()> {
    match tokens
        .iter()
        .position(|t| t.eq_ignore_ascii_case(key.as_bytes()))
    {
        Some(i) => tokens.get(i + 1).and_then(|t| number(t)).ok_or(()),
        None => Ok(fallback),
    }
}

fn band_kind(token: &[u8]) -> Option<BandKind> {
    let is = |names: &[&str]| {
        names
            .iter()
            .any(|n| token.eq_ignore_ascii_case(n.as_bytes()))
    };
    Some(if is(&["PK", "PEQ", "MODAL"]) {
        BandKind::Peaking
    } else if is(&["LSC", "LS", "LSQ"]) {
        BandKind::LowShelf
    } else if is(&["HSC", "HS", "HSQ"]) {
        BandKind::HighShelf
    } else if is(&["LP", "LPQ"]) {
        BandKind::LowPass
    } else if is(&["HP", "HPQ"]) {
        BandKind::HighPass
    } else if is(&["BP"]) {
        BandKind::BandPass
    } else if is(&["NO"]) {
        BandKind::Notch
    } else {
        return None;
    })
}

pub fn parse(text: &[u8]) -> Result<Profile, ParseError> {
    let mut profile = Profile {
        preamp_db: 0.,
        bands: Vec::new(),
    };
    let mut saw_any_line = false;
    let mut start = 0;
    for raw in text.split(|&b| b == b'\n') {
        let range = trim(text, start..start + raw.len());
        start += raw.len() + 1;
        let line = &text[range.clone()];
        if line.is_empty() || line[0] == b'#' {
            continue;
        }
        let fail = |kind| {
            Err(ParseError {
                kind,
                line: range.clone(),
            })
        };
        let tokens = tokens(line);
        let first = tokens[0];
        if first.eq_ignore_ascii_case(b"Preamp:") || first.eq_ignore_ascii_case(b"Preamp") {
            match tokens.get(1).and_then(|t| number(t)) {
                Some(value) => profile.preamp_db = value,
                None => return fail(ParseErrorKind::Preamp),
            }
            saw_any_line = true;
            continue;
        }
        if !first.eq_ignore_ascii_case(b"Filter") {
            continue; // unknown line
        }
        // Filter N: ON|OFF TYPE Fc <f> Hz Gain <g> dB Q <q>
        let Some(switch) = tokens[1..]
            .iter()
            .position(|t| t.eq_ignore_ascii_case(b"ON") || t.eq_ignore_ascii_case(b"OFF"))
            .map(|i| i + 1)
        else {
            continue; // no ON/OFF: not a filter line
        };
        // AutoEq writes empty slots as a bare ON/OFF; its type vocabulary drifts, so unknown
        // types skip the band instead of failing the profile.
        let Some(kind) = tokens.get(switch + 1).and_then(|t| band_kind(t)) else {
            continue;
        };
        let Ok(frequency) = value_after(&tokens, "Fc", 0.) else {
            return fail(ParseErrorKind::Frequency);
        };
        let (Ok(gain_db), Ok(q)) = (
            value_after(&tokens, "Gain", 0.),
            value_after(&tokens, "Q", DEFAULT_Q),
        ) else {
            return fail(ParseErrorKind::GainOrQ);
        };
        profile.bands.push(Band {
            kind,
            enabled: tokens[switch].eq_ignore_ascii_case(b"ON"),
            frequency,
            gain_db,
            q,
        });
        saw_any_line = true;
    }
    if !saw_any_line {
        return Err(ParseError {
            kind: ParseErrorKind::Empty,
            line: 0..0,
        });
    }
    Ok(profile)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bands(text: &str) -> Vec<(u32, bool, f64, f64, f64)> {
        parse(text.as_bytes())
            .unwrap()
            .bands
            .iter()
            .map(|b| (b.kind as u32, b.enabled, b.frequency, b.gain_db, b.q))
            .collect()
    }
    fn error(text: &str) -> (ParseErrorKind, String) {
        let e = parse(text.as_bytes()).unwrap_err();
        (
            e.kind,
            String::from_utf8(text.as_bytes()[e.line].to_vec()).unwrap(),
        )
    }

    #[test]
    fn parses_autoeq_profiles() {
        let profile = parse(
            b"\xEF\xBB\xBF# AutoEq\r\n  Preamp: -6.2 dB \r\nFilter 1: ON LSC Fc 105 Hz Gain 2.5 dB Q 0.70\r\n\
Filter 2: OFF PK Fc 105.5 Hz Gain -3 dB Q 1.41\r\nFilter 3: ON LP Fc 8000 Hz Q 0.71\r\n\
Filter 4: ON\nFilter 5: ON ZZZ Fc 1 Hz\nFilter 6 ON no Fc 50\nPreamp 1e-1\nunknown line\n",
        )
        .unwrap();
        assert_eq!(profile.preamp_db, 0.1);
        let got: Vec<_> = profile
            .bands
            .iter()
            .map(|b| (b.kind as u32, b.enabled, b.frequency, b.gain_db, b.q))
            .collect();
        assert_eq!(
            got,
            [
                (1, true, 105., 2.5, 0.70),
                (0, false, 105.5, -3., 1.41),
                (3, true, 8000., 0., 0.71),
                (6, true, 50., 0., DEFAULT_Q),
            ]
        );
    }

    #[test]
    fn keyword_search_spans_the_whole_line() {
        assert_eq!(
            bands("Filter 1: on pk fc 10 GAIN 1 q 2"),
            [(0, true, 10., 1., 2.)]
        );
        assert_eq!(bands("Filter Q 3 : ON PK Fc 10"), [(0, true, 10., 0., 3.)]);
        assert_eq!(error("Filter Q 3: ON PK Fc 10").0, ParseErrorKind::GainOrQ);
        assert_eq!(bands("Filter 1: ON HP"), [(4, true, 0., 0., DEFAULT_Q)]);
        assert_eq!(
            bands("Filter\x0B1: ON\x0BBP\x0BFc\x0B7"),
            [(5, true, 7., 0., DEFAULT_Q)]
        );
    }

    #[test]
    fn numbers_are_whole_finite_c_locale_tokens() {
        for (token, value) in [
            ("1.", 1.),
            (".5", 0.5),
            ("+2", 2.),
            ("-0", -0.),
            ("1E2", 100.),
            ("0e-400", 0.),
            ("0.000", 0.),
            ("2.2250738585072014e-308", f64::MIN_POSITIVE),
        ] {
            assert_eq!(number(token.as_bytes()), Some(value), "{token}");
        }
        for token in [
            "", "1e", "1e+", ".", "+", "1,5", "1.2.3", "0x10", "inf", "-inf", "infinity", "NaN",
            "1e400", "1Hz", "１", "1e-400", "-1e-400", "1e-310", "4.9e-324",
        ] {
            assert_eq!(number(token.as_bytes()), None, "{token}");
        }
    }

    #[test]
    fn reports_the_failing_line() {
        assert_eq!(
            error("Preamp: bad dB"),
            (ParseErrorKind::Preamp, "Preamp: bad dB".into())
        );
        assert_eq!(
            error("  Preamp:  \r\n"),
            (ParseErrorKind::Preamp, "Preamp:".into())
        );
        assert_eq!(
            error("Preamp: 0\n Filter 1: ON PK Fc abc Hz \n"),
            (
                ParseErrorKind::Frequency,
                "Filter 1: ON PK Fc abc Hz".into()
            )
        );
        assert_eq!(
            error("Filter 1: ON PK Fc 1 Gain"),
            (ParseErrorKind::GainOrQ, "Filter 1: ON PK Fc 1 Gain".into())
        );
        assert_eq!(
            error("Filter 1: ON PK Fc 1 Q 1e400").0,
            ParseErrorKind::GainOrQ
        );
        for empty in ["", "\n\n", "# only\n", "Filter 1: ON\n", "Filter 1: ON ZZZ"] {
            assert_eq!(
                parse(empty.as_bytes()).unwrap_err().kind,
                ParseErrorKind::Empty,
                "{empty:?}"
            );
        }
    }

    #[test]
    fn arbitrary_bytes_never_panic() {
        let mut state = 0x2545_f491_4f6c_dd1du64;
        let alphabet = b"Filter PreampONOFFPKLSCFcGainQHzdB0123456789.-+eE: \t\r\n\x0B#\xff\xc3";
        for _ in 0..50_000 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let len = (state % 64) as usize;
            let text: Vec<u8> = (0..len)
                .map(|i| alphabet[((state >> (i % 48)) as usize + i * 7) % alphabet.len()])
                .collect();
            let _ = parse(&text);
        }
    }
}
