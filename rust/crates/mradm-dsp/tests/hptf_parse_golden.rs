//! Golden regression for the AutoEq ParametricEQ text parser.
//!
//! The corpus (fixed cases and every prefix, vocabulary-assembled lines, byte mutations) is the one the
//! frozen C++ differential test used before the reference was retired (user decision 2026-11-14). The
//! digest was recorded from this parser while it still matched that reference on every portable text;
//! it also covers the underflowing and hexadecimal numbers the old differential had to exclude.

use mradm_dsp::hptf::{BandKind, ParseErrorKind, parse_parametric_eq};
use sha2::{Digest, Sha256};

const DIGEST: &str = "6aae07fbbd1496d047d55dcea612101db8901baa5400c3af7391024c98031107";
const TEXTS: usize = 208_731;
const PARSED: usize = 40_077;

struct Random(u64);

impl Random {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, limit: usize) -> usize {
        if limit == 0 {
            0
        } else {
            (self.next() % limit as u64) as usize
        }
    }
    fn pick<'a, T>(&mut self, values: &'a [T]) -> &'a T {
        &values[self.below(values.len())]
    }
}

#[derive(Default)]
struct Golden {
    hash: Sha256,
    texts: usize,
    parsed: usize,
}

impl Golden {
    fn compare(&mut self, text: &[u8]) {
        self.texts += 1;
        match parse_parametric_eq(text) {
            Ok(profile) => {
                self.parsed += 1;
                self.hash.update([0]);
                self.hash.update(profile.preamp_db.to_bits().to_le_bytes());
                self.hash.update((profile.bands.len() as u64).to_le_bytes());
                for band in &profile.bands {
                    let kind = match band.kind {
                        BandKind::Peaking => 0u8,
                        BandKind::LowShelf => 1,
                        BandKind::HighShelf => 2,
                        BandKind::LowPass => 3,
                        BandKind::HighPass => 4,
                        BandKind::BandPass => 5,
                        BandKind::Notch => 6,
                    };
                    self.hash.update([kind, u8::from(band.enabled)]);
                    for v in [band.frequency, band.gain_db, band.q] {
                        self.hash.update(v.to_bits().to_le_bytes());
                    }
                }
            }
            Err(error) => {
                let kind = match error.kind {
                    ParseErrorKind::Preamp => 1u8,
                    ParseErrorKind::Frequency => 2,
                    ParseErrorKind::GainOrQ => 3,
                    ParseErrorKind::Empty => 4,
                };
                self.hash.update([kind]);
                self.hash.update((error.line.start as u64).to_le_bytes());
                self.hash.update((error.line.end as u64).to_le_bytes());
            }
        }
    }
}

const PROFILE: &str = "Preamp: -6.2 dB\n\
                       Filter 1: ON LSC Fc 105 Hz Gain 2.5 dB Q 0.70\n\
                       Filter 2: ON PK Fc 210.5 Hz Gain -3.1 dB Q 1.41\n\
                       Filter 3: OFF HSC Fc 8000 Hz Gain 4 dB Q 0.7\n\
                       Filter 4: ON LP Fc 18000 Hz Q 0.71\n\
                       Filter 5: ON NO Fc 6000 Hz Q 4\n";

fn fixed_cases() -> Vec<Vec<u8>> {
    let mut cases: Vec<Vec<u8>> = [
        PROFILE.as_bytes(),
        b"",
        b"\n",
        b"# comment only\n",
        b"Preamp: 0 dB",
        b"Preamp:",
        b"Preamp: bad dB",
        b"preamp 1e-1",
        b"PREAMP: +.5",
        b"Preamp: -0",
        b"Filter 1: ON",
        b"Filter 1: ON ZZZ Fc 1",
        b"Filter 1: ON PK",
        b"Filter 1: ON PK Fc",
        b"Filter 1: ON PK Fc abc Hz",
        b"Filter 1: ON PK Fc 100 Gain",
        b"Filter 1: ON PK Fc 100 Q 0",
        b"Filter 1: ON PK Fc 0 Gain 1",
        b"Filter Q 3: ON PK Fc 10",
        b"Filter Q 3 : ON PK Fc 10",
        b"Filter 1: on pk fc 10 GAIN 1 q 2",
        b"Filter\x0b1:\x0cON\x0bBP\x0bFc\x0b7",
        b"\r\n  # c\r\n  Preamp: -1.5 dB  \r\nFilter 1: ON PEQ Fc 1000 Hz Gain 3 dB Q 1\r\n",
    ]
    .iter()
    .map(|c| c.to_vec())
    .collect();
    cases.push([b"\xEF\xBB\xBF", PROFILE.as_bytes()].concat());
    cases.push(b"Filter 1: ON MODAL Fc 50 Hz Gain 1 dB Q 9 Fc 70".to_vec());
    cases.push(b"Preamp: 1\0x\n".to_vec());
    cases.push(b"Preamp: 1\xc2\xa0".to_vec());
    for kind in [
        "PK", "PEQ", "MODAL", "LSC", "LS", "LSQ", "HSC", "HS", "HSQ", "LP", "LPQ", "HP", "HPQ",
        "BP", "NO", "lsq", "Pk",
    ] {
        cases.push(format!("Filter 1: ON {kind} Fc 1000 Hz Gain 1 dB Q 1").into_bytes());
    }
    for number in [
        "1.",
        ".5",
        "+2",
        "-0",
        "1E2",
        "1e-400",
        "1e-310",
        "1e",
        "1e+",
        ".",
        "+",
        "1,5",
        "1.2.3",
        "0x10",
        "0x1p3",
        "inf",
        "-inf",
        "+inf",
        "infinity",
        "NaN",
        "nan(1)",
        "1e400",
        "1Hz",
        "--1",
        "+-1",
        "1e5x",
        "007",
        "1e0005",
        "9007199254740993",
        "123456789012345678901234567890.123456789e-10",
        "4.9e-324",
        "2.2250738585072011e-308",
        "1.7976931348623157e308",
        "1.7976931348623159e308",
    ] {
        cases.push(format!("Preamp: {number} dB").into_bytes());
        cases.push(format!("Filter 1: ON PK Fc {number} Hz Gain 1 dB Q 1").into_bytes());
        cases.push(format!("Filter 1: ON PK Fc 100 Hz Gain 1 dB Q {number}").into_bytes());
    }
    let mut many = String::from("Preamp: 0\n");
    let mut too_many = many.clone();
    for i in 1..=40 {
        let state = if i <= 32 { "ON" } else { "OFF" };
        many += &format!("Filter {i}: {state} PK Fc {} Hz Gain 1 dB Q 1\n", i * 100);
        too_many += &format!("Filter {i}: ON PK Fc {} Hz Gain 1 dB Q 1\n", i * 100);
    }
    cases.push(many.into_bytes());
    cases.push(too_many.into_bytes());
    cases
}

fn tokens(out: &mut Golden, random: &mut Random) {
    const WORDS: [&str; 36] = [
        "Filter", "filter", "1:", "2:", "ON", "OFF", "on", "PK", "LSC", "HSQ", "LP", "HP", "BP",
        "NO", "ZZZ", "Fc", "fc", "Gain", "Q", "Hz", "dB", "Preamp:", "Preamp", "#", "1000", "-3.5",
        ".5", "1e3", "1e", "abc", "0", "-0", "inf", "1e400", "+2", "0x10",
    ];
    const SPACES: [&str; 6] = [" ", "  ", "\t", "\x0b", "\x0c", "\r"];
    for _ in 0..100_000 {
        let mut text = String::new();
        let lines = 1 + random.below(5);
        for _ in 0..lines {
            if random.below(3) != 0 {
                text += if random.below(2) == 0 {
                    "Filter 1: ON PK "
                } else {
                    "Preamp: "
                };
            }
            let count = random.below(9);
            for _ in 0..count {
                text += random.pick(&WORDS);
                text += random.pick(&SPACES);
            }
            text += if random.below(4) == 0 { "\r\n" } else { "\n" };
        }
        out.compare(text.as_bytes());
    }
}

fn bytes(out: &mut Golden, random: &mut Random) {
    const ALPHABET: &[u8] =
        b"Filter ONPKFcGainQHzdB0123456789.-+eE:#\t\n\r\x0b\x0c \x80\xc2\xa0\xff";
    let seeds = fixed_cases();
    for _ in 0..100_000 {
        let mut text = random.pick(&seeds).clone();
        let edits = 1 + random.below(4);
        for _ in 0..edits {
            let at = random.below(text.len() + 1);
            let c = if random.below(16) == 0 {
                random.next() as u8
            } else {
                ALPHABET[random.below(ALPHABET.len())]
            };
            match random.below(3) {
                0 => {
                    if at < text.len() {
                        text[at] = c;
                    }
                }
                1 => {
                    if at < text.len() {
                        text.remove(at);
                    }
                }
                _ => text.insert(at, c),
            }
        }
        out.compare(&text);
    }
}

#[test]
fn parametric_eq_corpus_matches_golden() {
    let mut out = Golden::default();
    for text in fixed_cases() {
        out.compare(&text);
        for size in 0..text.len() {
            out.compare(&text[..size]);
        }
    }
    let mut random = Random(0x5eed_0004);
    tokens(&mut out, &mut random);
    bytes(&mut out, &mut random);
    let hex: String = out
        .hash
        .finalize()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    assert!(out.parsed > 1000, "corpus must exercise successful parses");
    assert_eq!((out.texts, out.parsed), (TEXTS, PARSED));
    assert_eq!(hex, DIGEST);
}

// Underflowing and hexadecimal numbers made the old stream extraction platform-dependent; they are
// rejected everywhere, while an explicit zero still parses.
#[test]
fn platform_dependent_numbers_are_rejected() {
    for literal in [
        "1e-400", "-1e-400", "1e-310", "4.9e-324", "0.1e-320", "0x10", "0X1P3", "-0x1", "+0x1.8p1",
    ] {
        for line in [
            format!("Preamp: {literal} dB"),
            format!("Filter 1: ON PK Fc {literal} Hz"),
            format!("Filter 1: ON PK Fc 100 Hz Gain {literal}"),
            format!("Filter 1: ON PK Fc 100 Hz Q {literal}"),
        ] {
            assert!(
                parse_parametric_eq(line.as_bytes()).is_err(),
                "must be rejected: {line}"
            );
        }
    }
    for zero in ["0e-400", "0.000e-999", "-0.0"] {
        let line = format!("Preamp: {zero}");
        let parsed = parse_parametric_eq(line.as_bytes()).expect(&line);
        assert_eq!(parsed.preamp_db, 0.0, "{line}");
    }
}
