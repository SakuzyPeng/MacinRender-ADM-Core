//! Routing goldens (PCM consistency 4.5). `routing-v1/expected.jsonl` was produced once by the
//! C++ `render_common` rules this module replaced (before 4.5 A) for every input of
//! `routing-v1/cases.jsonl`: label keys, target indices against the standard layouts and
//! synthetic tables, label positions, LFE targets, DirectSpeakers matrices with their block
//! routes, live overrides and the 22.2 LFE routing with its log lines. Bytes are hex, floats are
//! their bit patterns.
use mradm_json::{Map, Value};
use mradm_render::routing::{self, Level, LfeTarget, MatrixRoute, Override, Target};
use mradm_scene::layouts::{Geometry, layouts};
use std::path::Path;

fn lines(name: &str) -> Vec<Value> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/routing-v1")
        .join(name);
    std::fs::read_to_string(path)
        .unwrap()
        .lines()
        .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
        .collect()
}

fn get<'a>(v: &'a Value, key: &str) -> &'a Value {
    &v.as_object().unwrap()[key]
}

fn text(v: &Value) -> &[u8] {
    v.as_str().unwrap().as_bytes()
}

fn hex(bytes: &[u8]) -> Value {
    bytes
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>()
        .into()
}

fn bits(v: f32) -> Value {
    format!("{:08x}", v.to_bits()).into()
}

fn object(fields: Vec<(&str, Value)>) -> Value {
    Value::Object(
        fields
            .into_iter()
            .map(|(k, v)| (k.to_owned(), v))
            .collect::<Map>(),
    )
}

fn uint(v: usize) -> Value {
    Value::Unsigned(v as u64)
}

fn error(e: &routing::Error) -> Value {
    object(vec![
        ("code", uint(e.code as usize)),
        ("message", hex(&e.message)),
        ("context", hex(&e.context)),
    ])
}

fn labels(v: &Value) -> Vec<&[u8]> {
    v.as_array().unwrap().iter().map(text).collect()
}

fn targets(case: &Value) -> Option<Vec<Target<'_>>> {
    let map = case.as_object().unwrap();
    if let Some(index) = map.get("layout") {
        let layout = layouts(Geometry::Standard).get(index.as_f64().unwrap() as usize)?;
        return Some(
            layout
                .speakers
                .iter()
                .map(|s| Target {
                    label: s.label.as_bytes(),
                    is_lfe: s.is_lfe,
                })
                .collect(),
        );
    }
    Some(
        map["targets"]
            .as_array()
            .unwrap()
            .iter()
            .map(|t| {
                let t = t.as_array().unwrap();
                Target {
                    label: text(&t[0]),
                    is_lfe: t[1].as_bool().unwrap(),
                }
            })
            .collect(),
    )
}

fn run(case: &Value) -> Value {
    let skip = || object(vec![("skip", true.into())]);
    match get(case, "kind").as_str().unwrap() {
        "label" => {
            let raw = case.as_object().unwrap().get("hex").map_or_else(
                || text(get(case, "label")).to_vec(),
                |h| {
                    let h = h.as_str().unwrap();
                    (0..h.len())
                        .step_by(2)
                        .map(|i| u8::from_str_radix(&h[i..i + 2], 16).unwrap())
                        .collect()
                },
            );
            object(vec![
                ("key", hex(&routing::label_key(&raw))),
                ("canonical", hex(&routing::canonical_label(&raw))),
                ("direct", hex(&routing::direct_label(&raw))),
                ("lfe", routing::is_lfe(&raw).into()),
            ])
        }
        "index" => match targets(case) {
            None => skip(),
            Some(t) => object(vec![(
                "index",
                routing::target_index(&t, &labels(get(case, "labels"))).map_or(Value::Null, uint),
            )]),
        },
        "position" => object(vec![(
            "position",
            routing::label_position(&labels(get(case, "labels")))
                .map_or(Value::Null, |[a, e]| Value::Array(vec![bits(a), bits(e)])),
        )]),
        "lfe_target" => object(vec![(
            "target",
            uint(routing::lfe_target(
                &labels(get(case, "labels")),
                get(case, "low_pass").as_bool().unwrap(),
            ) as usize),
        )]),
        "matrix" => {
            let Some(t) = targets(case) else {
                return skip();
            };
            let routes: Vec<MatrixRoute> = get(case, "routes")
                .as_array()
                .unwrap()
                .iter()
                .map(|r| {
                    let r = r.as_array().unwrap();
                    MatrixRoute {
                        source_label: text(&r[0]),
                        mute: r[1].as_bool().unwrap(),
                        targets: r[2]
                            .as_array()
                            .unwrap()
                            .iter()
                            .map(|t| {
                                let t = t.as_array().unwrap();
                                (text(&t[0]), t[1].as_f32().unwrap())
                            })
                            .collect(),
                    }
                })
                .collect();
            match routing::resolve_matrix(&routes, &t, text(get(case, "layout_id"))) {
                Err(e) => object(vec![("error", error(&e))]),
                Ok(resolved) => {
                    let routes = resolved.iter().map(|r| {
                        Value::Array(vec![
                            hex(&r.source_label),
                            hex(&r.source_key),
                            r.mute.into(),
                            Value::Array(
                                r.targets
                                    .iter()
                                    .map(|t| Value::Array(vec![uint(t.output), bits(t.gain)]))
                                    .collect(),
                            ),
                        ])
                    });
                    let blocks = get(case, "blocks").as_array().unwrap().iter().map(|b| {
                        match routing::matrix_route(&resolved, &labels(b)) {
                            Ok(index) => uint(index),
                            Err(e) => error(&e),
                        }
                    });
                    object(vec![
                        ("routes", Value::Array(routes.collect())),
                        ("blocks", Value::Array(blocks.collect())),
                    ])
                }
            }
        }
        "live" => {
            let overrides: Vec<Override> = get(case, "overrides")
                .as_array()
                .unwrap()
                .iter()
                .map(|o| {
                    let o = o.as_array().unwrap();
                    Override {
                        object_id: text(&o[0]),
                        speaker_label: text(&o[1]),
                        mute: o[2].as_bool().unwrap(),
                        gain_db: o[3].as_f32().unwrap(),
                        head_locked: o[4].as_bool(),
                    }
                })
                .collect();
            let (id, key) = (text(get(case, "object_id")), text(get(case, "key")));
            object(vec![
                (
                    "gain",
                    routing::live_channel_gain(&overrides, id, key).map_or(Value::Null, bits),
                ),
                (
                    "head_locked",
                    routing::live_head_locked(&overrides, id, key).map_or(Value::Null, Value::Bool),
                ),
            ])
        }
        _ => {
            let blocks: Vec<LfeTarget> = get(case, "blocks")
                .as_array()
                .unwrap()
                .iter()
                .map(|b| {
                    let b = b.as_array().unwrap();
                    routing::lfe_target(&labels(&b[0]), b[1].as_bool().unwrap())
                })
                .collect();
            let mut logs = Vec::new();
            let result = routing::lfe_routing(
                get(case, "split_power").as_bool().unwrap(),
                text(get(case, "output_layout")),
                blocks,
                &mut |level, message| {
                    // C++ LogLevel: debug, info, warning, error.
                    let level = match level {
                        Level::Info => 1,
                        Level::Warning => 2,
                    };
                    logs.push(Value::Array(vec![
                        uint(level),
                        "mod".into(),
                        message.into(),
                    ]));
                },
            );
            let mut fields = vec![("logs", Value::Array(logs))];
            match result {
                Err(e) => fields.push(("error", error(&e))),
                Ok(r) => {
                    let all = [LfeTarget::None, LfeTarget::Lfe1, LfeTarget::Lfe2];
                    let gains = all
                        .iter()
                        .flat_map(|&i| all.iter().map(move |&o| bits(r.gain(i, o))))
                        .collect();
                    fields.push((
                        "plan",
                        Value::Array(vec![
                            r.applies_to_22_2.into(),
                            r.has_lfe1.into(),
                            r.has_lfe2.into(),
                            Value::Array(gains),
                        ]),
                    ));
                }
            }
            object(fields)
        }
    }
}

#[test]
fn routing_reproduces_the_cpp_goldens() {
    let cases = lines("cases.jsonl");
    let expected = lines("expected.jsonl");
    assert_eq!(cases.len(), expected.len());
    for (index, (case, want)) in cases.iter().zip(&expected).enumerate() {
        assert_eq!(
            &run(case),
            want,
            "case {index}: {}",
            mradm_json::dump(case, None)
        );
    }
    assert_eq!(cases.len(), 4550);
}
