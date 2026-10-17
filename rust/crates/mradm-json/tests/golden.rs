//! Golden results produced once by nlohmann/json 3.12.0 (the project's former C++ JSON library)
//! from the same splitmix64 input streams generated here; see RUST_INVERSION_POLICY.md.
//! Digests are FNV-1a 64 over each result followed by a 0xFF separator.
use mradm_json::{Value, dump, format_f64, parse};

struct SplitMix(u64);
impl SplitMix {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
}

struct Fnv(u64);
impl Fnv {
    fn new() -> Self {
        Self(0xcbf2_9ce4_8422_2325)
    }
    fn add(&mut self, bytes: &[u8]) {
        for &b in bytes.iter().chain(&[0xFF]) {
            self.0 ^= u64::from(b);
            self.0 = self.0.wrapping_mul(0x100_0000_01b3);
        }
    }
}

fn golden(name: &str) -> String {
    let path = format!("{}/tests/golden/{name}", env!("CARGO_MANIFEST_DIR"));
    std::fs::read_to_string(path).unwrap()
}

fn digest(file: &str, key: &str) -> u64 {
    let line = file
        .lines()
        .find(|l| l.starts_with(key))
        .unwrap_or_else(|| panic!("missing {key}"));
    u64::from_str_radix(line.rsplit(' ').next().unwrap(), 16).unwrap()
}

fn hex(bytes: &[u8]) -> String {
    if bytes.is_empty() {
        return "-".into();
    }
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn unhex(text: &str) -> Vec<u8> {
    if text == "-" {
        return Vec::new();
    }
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap())
        .collect()
}

fn class(v: &Value) -> &'static str {
    match v {
        Value::Unsigned(_) => "u",
        Value::Integer(_) => "i",
        Value::Float(_) => "f",
        Value::Object(_) => "o",
        Value::Array(_) => "a",
        Value::String(_) => "s",
        Value::Bool(_) => "b",
        Value::Null => "n",
    }
}

fn parse_result(text: &[u8]) -> String {
    match parse(text) {
        Ok(v) => format!(
            "ok {} {} {}",
            class(&v),
            hex(dump(&v, None).as_bytes()),
            hex(dump(&v, Some(2)).as_bytes())
        ),
        Err(mradm_json::ParseError::NumberOverflow(token)) => {
            format!("overflow {}", mradm_json::ParseError::NumberOverflow(token))
        }
        Err(_) => "syntax".into(),
    }
}

/// C's nextafter for the two directions the generator used.
fn toward_zero(v: f64) -> f64 {
    if v == 0.0 {
        0.0
    } else {
        f64::from_bits(v.to_bits() - 1)
    }
}
fn toward_infinity(v: f64) -> f64 {
    if v == f64::INFINITY {
        v
    } else if v == 0.0 {
        f64::from_bits(1)
    } else {
        f64::from_bits(v.to_bits() + 1)
    }
}

#[test]
fn doubles_format_as_nlohmann() {
    let file = golden("dtoa-v1.txt");
    let mut rng = SplitMix(0x4D52_414D_4A53_4F4E);
    let mut f64_bits = Fnv::new();
    for _ in 0..2_000_000 {
        f64_bits.add(format_f64(f64::from_bits(rng.next())).as_bytes());
    }
    let mut f32_bits = Fnv::new();
    for _ in 0..2_000_000 {
        let v = f32::from_bits((rng.next() >> 32) as u32);
        f32_bits.add(format_f64(f64::from(v)).as_bytes());
    }
    assert_eq!(f64_bits.0, digest(&file, "f64-bits"));
    assert_eq!(f32_bits.0, digest(&file, "f32-bits"));

    let mut decimal = Fnv::new();
    for e in -330..=310 {
        let base: f64 = format!("1e{e}").parse().unwrap();
        for m in 1..=9 {
            let v = base * f64::from(m);
            let (mut lo, mut hi) = (v, v);
            for _ in 0..4 {
                lo = toward_zero(lo);
                hi = toward_infinity(hi);
                decimal.add(format_f64(lo).as_bytes());
                decimal.add(format_f64(hi).as_bytes());
            }
            decimal.add(format_f64(v).as_bytes());
            decimal.add(format_f64(f64::from(v as f32)).as_bytes());
        }
    }
    assert_eq!(decimal.0, digest(&file, "decimal-neighbours"));

    for line in file.lines().filter(|l| l.starts_with("edge ")) {
        let mut parts = line.splitn(3, ' ').skip(1);
        let bits = u64::from_str_radix(parts.next().unwrap(), 16).unwrap();
        assert_eq!(
            format_f64(f64::from_bits(bits)),
            parts.next().unwrap(),
            "{line}"
        );
    }
}

#[test]
fn parse_acceptance_matches_nlohmann() {
    let file = golden("parse-v1.txt");
    let mut checked = 0;
    for line in file.lines() {
        if let Some(rest) = line.strip_prefix("deep-100000 ") {
            let deep = [vec![b'['; 100_000], vec![b']'; 100_000]].concat();
            let result = if parse(&deep).is_ok() { "ok" } else { "syntax" };
            assert_eq!(result, rest);
            continue;
        }
        let (input, expected) = line.split_once(' ').unwrap();
        let input = unhex(input);
        assert_eq!(
            parse_result(&input),
            expected,
            "input {:?}",
            String::from_utf8_lossy(&input)
        );
        checked += 1;
    }
    assert!(checked > 90);
}

#[test]
fn fuzzed_inputs_match_nlohmann() {
    let file = golden("fuzz-v1.txt");
    let seeds: [&[u8]; 5] = [
        br#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"gain_db":-3.5,"mute":false}}}"#,
        br#"{"objects":[{"id":"AO_1001","extent":{"scale":0.25,"max":1}},{"all":true}]}"#,
        br#"[1,-2,3.5e10,"\u00e9\n",null,true,{"a":[]},{}]"#,
        br#"{"routes":[{"source_label":"M+030","targets":[{"label":"M+030","weight":1e-3}]}]}"#,
        b"\"\xE2\x82\xAC\\ud83d\\ude00\"",
    ];
    let alphabet: &[u8] = b"{}[]\",:.-+eE0123456789 \t\nabtrufnl\\u/\xC3\xA9\xFF";
    let mut rng = SplitMix(0x4655_5A5A_4A53_4F4E);
    let mut fuzz = Fnv::new();
    let mut accepted = 0;
    for _ in 0..200_000 {
        let mut text = seeds[(rng.next() % seeds.len() as u64) as usize].to_vec();
        let ops = 1 + rng.next() % 4;
        for _ in 0..ops {
            let op = rng.next() % 3;
            let pos = if text.is_empty() {
                0
            } else {
                rng.next() % (text.len() as u64 + u64::from(op == 1))
            } as usize;
            let c = alphabet[(rng.next() % alphabet.len() as u64) as usize];
            if op == 0 && !text.is_empty() {
                let at = pos % text.len();
                text[at] = c;
            } else if op == 1 {
                text.insert(pos, c);
            } else if !text.is_empty() {
                let at = pos % text.len();
                text.remove(at);
            }
        }
        let result = parse_result(&text);
        accepted += usize::from(result.starts_with("ok"));
        fuzz.add(result.as_bytes());
    }
    let mutations = file.lines().next().unwrap();
    assert_eq!(
        mutations,
        format!("mutations 200000 accepted {accepted} {:x}", fuzz.0)
    );

    let mut numbers = Fnv::new();
    for _ in 0..300_000 {
        let mut text = String::new();
        if rng.next().is_multiple_of(2) {
            text.push('-');
        }
        for _ in 0..1 + rng.next() % 25 {
            text.push(char::from(b'0' + (rng.next() % 10) as u8));
        }
        if rng.next().is_multiple_of(2) {
            text.push('.');
            for _ in 0..1 + rng.next() % 20 {
                text.push(char::from(b'0' + (rng.next() % 10) as u8));
            }
        }
        if rng.next().is_multiple_of(2) {
            text.push(if rng.next().is_multiple_of(2) {
                'e'
            } else {
                'E'
            });
            match rng.next() % 3 {
                1 => text.push('-'),
                2 => text.push('+'),
                _ => {}
            }
            text.push_str(&(rng.next() % 400).to_string());
        }
        numbers.add(parse_result(text.as_bytes()).as_bytes());
    }
    assert_eq!(numbers.0, digest(&file, "numbers"));
}

#[test]
fn nested_values_drop_without_recursion() {
    let mut value = Value::Null;
    for _ in 0..1_000_000 {
        value = Value::Array(vec![value]);
    }
    drop(value);
}
