//! DirectSpeakers matrix and screen configuration goldens (PCM consistency 4.4).
//! `config-v1/expected.jsonl` was produced once by the C++ parsers these modules replaced
//! (`parse_direct_speakers_matrix(json, "<memory>")` and `parse_screen_config_json`, before 4.4 F)
//! for every input of `config-v1/cases.jsonl`: the parsed values with float bit patterns, or the
//! error code, message and context. JSON syntax errors compare only the code and message prefix.
use mradm_dsp::room::screen::{Mapping, RoomScreenGeometry, RoomScreens};
use mradm_json::Value;
use mradm_scene::{Context, direct_matrix, screen};
use std::path::Path;

fn lines(name: &str) -> Vec<Value> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/config-v1")
        .join(name);
    std::fs::read_to_string(path)
        .unwrap()
        .lines()
        .map(|line| mradm_json::parse(line.as_bytes()).unwrap())
        .collect()
}

fn get<'a>(value: &'a Value, key: &str) -> Option<&'a Value> {
    value.as_object().unwrap().get(key)
}

fn input(case: &Value) -> Vec<u8> {
    let mut bytes = match get(case, "text") {
        Some(text) => text.as_str().unwrap().as_bytes().to_vec(),
        None => {
            let hex = get(case, "hex").unwrap().as_str().unwrap();
            (0..hex.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
                .collect()
        }
    };
    let pad = get(case, "pad").map_or(0, |p| p.as_f64().unwrap() as usize);
    bytes.resize(bytes.len() + pad, b' ');
    bytes
}

fn error(code: i32, message: &str, context: &str) -> Value {
    let mut map = mradm_json::Map::new();
    map.insert("code".into(), Value::Unsigned(code as u64));
    map.insert("message".into(), message.into());
    map.insert("context".into(), context.into());
    Value::Object(map)
}

fn geometry(g: Option<RoomScreenGeometry>) -> Value {
    g.map_or(Value::Null, |g| {
        Value::Array(vec![
            Value::Bool(g.cartesian != 0),
            format!("{:016x}", g.aspect_ratio.to_bits()).into(),
            format!("{:016x}", g.centre[0].to_bits()).into(),
            format!("{:016x}", g.centre[1].to_bits()).into(),
            format!("{:016x}", g.centre[2].to_bits()).into(),
            format!("{:016x}", g.width.to_bits()).into(),
        ])
    })
}

fn ok(fields: Vec<(&str, Value)>) -> Value {
    let mut map = mradm_json::Map::new();
    map.insert(
        "ok".into(),
        Value::Object(fields.into_iter().map(|(k, v)| (k.into(), v)).collect()),
    );
    Value::Object(map)
}

fn matrix(bytes: &[u8]) -> Value {
    match direct_matrix::parse(bytes) {
        Err(e) => {
            assert_eq!(e.context, Context::Label);
            error(e.code, &e.message, "<memory>")
        }
        Ok(m) => {
            let routes = m.routes.iter().map(|r| {
                let targets = r.targets.iter().map(|t| {
                    Value::Array(vec![
                        t.label.as_str().into(),
                        format!("{:08x}", t.weight.to_bits()).into(),
                        format!("{:08x}", t.gain.to_bits()).into(),
                    ])
                });
                let mut map = mradm_json::Map::new();
                map.insert("source_label".into(), r.source_label.as_str().into());
                map.insert("mute".into(), r.mute.into());
                map.insert("targets".into(), Value::Array(targets.collect()));
                Value::Object(map)
            });
            ok(vec![
                ("output_layout", m.output_layout.as_str().into()),
                ("routes", Value::Array(routes.collect())),
            ])
        }
    }
}

/// `parse_screen_config_json`: the parse, then `validate_screen_config` with context `screen`.
fn screens(bytes: &[u8]) -> Value {
    match screen::parse(bytes) {
        Err(e) => {
            assert_eq!(e.context, Context::None);
            error(e.code, &e.message, "")
        }
        Ok(s) => {
            let reference = s.reference.unwrap_or_default();
            let checked = Mapping::new(RoomScreens {
                enabled: 1,
                reserved: 0,
                reference,
                reproduction: s.reproduction.unwrap_or(reference),
            });
            match checked {
                Err(mradm_dsp::Error::InvalidArgument(m)) => error(1, m, "screen"),
                Err(other) => panic!("unexpected validation error {other}"),
                Ok(_) => ok(vec![
                    ("reference", geometry(s.reference)),
                    ("reproduction", geometry(s.reproduction)),
                ]),
            }
        }
    }
}

#[test]
fn parsers_reproduce_the_cpp_goldens() {
    let cases = lines("cases.jsonl");
    let expected = lines("expected.jsonl");
    assert_eq!(cases.len(), expected.len());
    let (mut parsed, mut failed) = (0, 0);
    for (index, (case, want)) in cases.iter().zip(&expected).enumerate() {
        let bytes = input(case);
        let mut got = match get(case, "kind").unwrap().as_str().unwrap() {
            "matrix" => matrix(&bytes),
            _ => screens(&bytes),
        };
        let mut want = want.clone();
        if let Value::Object(map) = &mut want
            && let Some(Value::Bool(syntax)) = map.remove("syntax")
        {
            failed += 1;
            if syntax {
                let message = get(&got, "message").unwrap().as_str().unwrap();
                assert!(message.contains("JSON 语法错误"), "case {index}: {message}");
                let prefix = get(&want, "message").unwrap().as_str().unwrap();
                let prefix = prefix.to_owned();
                if let Value::Object(map) = &mut got {
                    let message = map["message"].as_str().unwrap();
                    assert!(message.starts_with(&prefix), "case {index}: {message}");
                    map.insert("message".into(), prefix.into());
                }
            }
        } else {
            parsed += 1;
        }
        assert_eq!(got, want, "case {index}");
    }
    assert_eq!((parsed, failed), (227, 731));
}
