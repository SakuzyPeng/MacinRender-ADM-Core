//! Semantic policy goldens (PCM consistency 4.4). `policy-v1/expected.jsonl` was produced once by
//! the C++ implementation this module replaced (`semantic_policy.cpp` after the 4.4 report fixes,
//! the C++ `06-policy` encoder and nlohmann `dump(2)`) over `policy-v1/policies.jsonl`: the five
//! consistency policies and seeded random policies on 14 ADM scenes; see RUST_INVERSION_POLICY.md.
//! Digests are FNV-1a 64; JSON syntax errors only keep their code and prefix. A golden's optional
//! `nan_words` lists only floating-point words whose arithmetic result is NaN: assert that class
//! and normalize those words to the C++ golden's NaN, whose sign is not portable.
use mradm_json::Value;
use mradm_scene::{Context, policy, report};
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::collections::HashMap;
use std::fs::File;
use std::path::{Path, PathBuf};

fn dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/policy-v1")
}

fn fnv(bytes: &[u8]) -> String {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    for &b in bytes {
        h ^= u64::from(b);
        h = h.wrapping_mul(0x100_0000_01b3);
    }
    format!("{h:016x}")
}

fn checkpoint_bytes(words: &[i32], nan_words: &[usize]) -> Vec<u8> {
    let mut words = words.to_vec();
    for &index in nan_words {
        assert!(
            f32::from_bits(words[index] as u32).is_nan(),
            "checkpoint word {index} must be NaN"
        );
        // The reference's negative quiet NaN. Do not inspect every word as a float:
        // integers such as the unbounded-end sentinel also have NaN bit patterns.
        words[index] = 0xffc0_0000_u32 as i32;
    }
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

#[test]
fn checkpoint_nan_normalization_preserves_other_words() {
    let expected = [0xffff_ffff_u32, 0xffc0_0000, 1.0_f32.to_bits()]
        .into_iter()
        .flat_map(u32::to_le_bytes)
        .collect::<Vec<_>>();
    for nan in [0x7fc0_0000_u32, 0xffc0_0000, 0x7fc0_1234, 0xffc0_1234] {
        let words = [-1, nan as i32, 1.0_f32.to_bits() as i32];
        assert_eq!(checkpoint_bytes(&words, &[1]), expected);
        assert_eq!(
            checkpoint_bytes(&words, &[]),
            words
                .iter()
                .flat_map(|w| w.to_le_bytes())
                .collect::<Vec<_>>()
        );
    }
}

#[test]
fn checkpoint_nan_normalization_rejects_finite_values_and_infinity() {
    for value in [0.0_f32, -0.0, 1.0, f32::INFINITY, f32::NEG_INFINITY] {
        let words = [value.to_bits() as i32];
        assert!(std::panic::catch_unwind(|| checkpoint_bytes(&words, &[0])).is_err());
    }
}

fn fixed<const N: usize>(text: &str) -> [u8; N] {
    let mut out = [b' '; N];
    out[..text.len()].copy_from_slice(text.as_bytes());
    out
}

fn field<'a>(value: &'a Value, key: &str) -> &'a Value {
    value.as_object().unwrap().get(key).unwrap()
}

fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    field(value, key).as_str().unwrap()
}

/// Writes the scene as the generator's 480-frame PCM24 BW64 file with its AXML and CHNA.
fn scene_wav(name: &str, out: &Path) -> PathBuf {
    let meta =
        mradm_json::parse(&std::fs::read(dir().join(format!("scenes/{name}.json"))).unwrap())
            .unwrap();
    let channels = field(&meta, "channels").as_f64().unwrap() as u16;
    let entries = field(&meta, "chna")
        .as_array()
        .unwrap()
        .iter()
        .map(|e| {
            let e = e.as_array().unwrap();
            let mut pack = [0u8; 11];
            pack.copy_from_slice(&fixed::<11>(e[3].as_str().unwrap()));
            ChnaEntry {
                track_index: e[0].as_f64().unwrap() as u16,
                uid: fixed::<12>(e[1].as_str().unwrap()),
                track_format: fixed::<14>(e[2].as_str().unwrap()),
                pack_format: pack,
                pad: 0,
            }
        })
        .collect::<Vec<_>>();
    let path = out.join(format!("{name}.wav"));
    let format = Format {
        channels,
        sample_rate: field(&meta, "sample_rate").as_f64().unwrap() as u32,
        sample_format: SampleFormat::Pcm24,
        channel_mask: None,
    };
    let options = WriterOptions {
        container: None,
        axml: Some(std::fs::read(dir().join(format!("scenes/{name}.xml"))).unwrap()),
        chna: Some(Chna {
            tracks: entries.len() as u16,
            entries,
        }),
    };
    let mut writer = Writer::new(File::create(&path).unwrap(), format, options).unwrap();
    writer
        .write_frames(&vec![0.0; usize::from(channels) * 480])
        .unwrap();
    writer.finish().unwrap();
    path
}

fn report_options(value: &Value) -> report::ReportOptions {
    let caps = field(value, "capabilities").as_array().unwrap();
    report::ReportOptions {
        renderer: text(value, "renderer").to_owned(),
        policy_path: text(value, "policy_path").to_owned(),
        supports_channel_lock: caps[0].as_bool().unwrap(),
        supports_object_divergence: caps[1].as_bool().unwrap(),
        supports_diffuse: caps[2].as_bool().unwrap(),
        hrtf_sources: caps[3]
            .as_array()
            .unwrap()
            .iter()
            .map(|s| s.as_str().unwrap().to_owned())
            .collect(),
        renderer_effective: text(value, "renderer_effective").to_owned(),
    }
}

#[test]
fn policies_reproduce_the_cpp_goldens() {
    let temp = std::env::temp_dir().join(format!("mradm-scene-policy-{}", std::process::id()));
    std::fs::create_dir_all(&temp).unwrap();
    let mut wavs = HashMap::new();
    let expected = std::fs::read_to_string(dir().join("expected.jsonl")).unwrap();
    let mut expected_cases = HashMap::new();
    let mut templates = HashMap::new();
    for line in expected.lines() {
        let record = mradm_json::parse(line.as_bytes()).unwrap();
        let map = record.as_object().unwrap();
        if let Some(name) = map.get("template") {
            templates.insert(name.as_str().unwrap().to_owned(), record.clone());
        } else {
            expected_cases.insert(field(&record, "i").as_f64().unwrap() as usize, record);
        }
    }

    let corpus = std::fs::read_to_string(dir().join("policies.jsonl")).unwrap();
    let (mut applied, mut rejected) = (0, 0);
    for (index, line) in corpus.lines().enumerate() {
        let case = mradm_json::parse(line.as_bytes()).unwrap();
        let name = text(&case, "scene").to_owned();
        let wav = wavs
            .entry(name.clone())
            .or_insert_with(|| scene_wav(&name, &temp))
            .clone();
        let path = wav.to_str().unwrap().as_bytes();
        let mut scene = mradm_scene::import_adm(path).unwrap();
        if let Some(template) = templates.remove(&name) {
            let t = report::template(&scene);
            assert_eq!(
                (t.len(), fnv(t.as_bytes())),
                (
                    field(&template, "len").as_f64().unwrap() as usize,
                    text(&template, "fnv").to_owned()
                ),
                "template of {name}"
            );
        }
        let want = &expected_cases[&index];
        let label = text(&case, "label");
        if let Some(text_value) = field(&case, "policy").as_str() {
            match policy::parse(text_value.as_bytes()) {
                Err(error) => {
                    let want = field(want, "error");
                    let message = text(want, "message");
                    assert_eq!(error.context, Context::Label, "case {index}");
                    assert_eq!(
                        i64::from(error.code),
                        field(want, "code").as_f64().unwrap() as i64,
                        "case {index}"
                    );
                    assert_eq!(text(want, "context"), label, "case {index}");
                    if message.contains("[json.exception.parse_error") {
                        assert!(
                            error
                                .message
                                .starts_with("invalid semantic policy: JSON 语法错误"),
                            "case {index}: {}",
                            error.message
                        );
                    } else {
                        assert_eq!(error.message, message, "case {index}");
                    }
                    rejected += 1;
                    continue;
                }
                Ok(parsed) => {
                    assert!(
                        want.as_object().unwrap().get("error").is_none(),
                        "case {index} must be rejected: {}",
                        text(field(want, "error"), "message")
                    );
                    scene.apply_policy(parsed);
                    applied += 1;
                }
            }
        }
        let warnings: Vec<_> = field(want, "warnings")
            .as_array()
            .unwrap()
            .iter()
            .map(|w| w.as_str().unwrap().to_owned())
            .collect();
        assert_eq!(scene.policy_warnings, warnings, "case {index} warnings");
        let words = mradm_scene::checkpoint::encode(&scene);
        let nan_words: Vec<usize> = want
            .as_object()
            .unwrap()
            .get("nan_words")
            .map(|indices| {
                indices
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(|i| i.as_unsigned_u32().unwrap() as usize)
                    .collect()
            })
            .unwrap_or_default();
        let bytes = checkpoint_bytes(&words, &nan_words);
        assert_eq!(
            (words.len(), fnv(&bytes)),
            (
                field(want, "words").as_f64().unwrap() as usize,
                text(want, "words_fnv").to_owned()
            ),
            "case {index} ({name}, {label}) 06-policy words"
        );
        let report = report::report(&scene, &report_options(field(&case, "report"))).unwrap();
        assert_eq!(
            (report.len(), fnv(report.as_bytes())),
            (
                field(want, "report_len").as_f64().unwrap() as usize,
                text(want, "report_fnv").to_owned()
            ),
            "case {index} ({name}, {label}) report"
        );
    }
    assert!(templates.is_empty());
    assert!(applied > 500 && rejected > 100, "{applied} {rejected}");
    std::fs::remove_dir_all(&temp).unwrap();
}

#[test]
fn readable_samples_match() {
    let corpus = std::fs::read_to_string(dir().join("policies.jsonl")).unwrap();
    let temp = std::env::temp_dir().join(format!("mradm-scene-sample-{}", std::process::id()));
    std::fs::create_dir_all(&temp).unwrap();
    let wav = scene_wav("policy-objects", &temp);
    for (label, sample) in [
        (
            "policy-identity.json",
            "report-policy-objects-identity.json",
        ),
        (
            "policy-position.json",
            "report-policy-objects-position.json",
        ),
    ] {
        let case = corpus
            .lines()
            .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
            .find(|c| text(c, "scene") == "policy-objects" && text(c, "label") == label)
            .unwrap();
        let mut scene = mradm_scene::import_adm(wav.to_str().unwrap().as_bytes()).unwrap();
        scene.apply_policy(policy::parse(text(&case, "policy").as_bytes()).unwrap());
        let report = report::report(&scene, &report_options(field(&case, "report"))).unwrap();
        assert_eq!(
            report,
            std::fs::read_to_string(dir().join("samples").join(sample)).unwrap()
        );
    }
    for name in ["policy-objects", "hoa"] {
        let wav = scene_wav(name, &temp);
        let scene = mradm_scene::import_adm(wav.to_str().unwrap().as_bytes()).unwrap();
        assert_eq!(
            report::template(&scene),
            std::fs::read_to_string(dir().join(format!("samples/template-{name}.json"))).unwrap()
        );
    }
    std::fs::remove_dir_all(&temp).unwrap();
}

/// The neutral template, applied unmodified, changes nothing except one behaviour kept from the
/// C++ template: its `extent.max` of 1 clamps extents above 1, although polar extents are in
/// degrees (three of the 14 scenes lose extent this way).
#[test]
fn applying_the_template_is_an_identity_up_to_the_extent_clamp() {
    let temp = std::env::temp_dir().join(format!("mradm-scene-identity-{}", std::process::id()));
    std::fs::create_dir_all(&temp).unwrap();
    let (mut scenes, mut clamped) = (0, Vec::new());
    for entry in std::fs::read_dir(dir().join("scenes")).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().is_none_or(|e| e != "json") {
            continue;
        }
        let name = path.file_stem().unwrap().to_str().unwrap().to_owned();
        let wav = scene_wav(&name, &temp);
        let mut scene = mradm_scene::import_adm(wav.to_str().unwrap().as_bytes()).unwrap();
        let mut expected = scene.snapshot.object_blocks.clone();
        for b in &mut expected {
            if b.width > 1. || b.height > 1. || b.depth > 1. {
                (b.width, b.height, b.depth) = (b.width.min(1.), b.height.min(1.), b.depth.min(1.));
                if !clamped.contains(&name) {
                    clamped.push(name.clone());
                }
            }
        }
        let before = mradm_scene::checkpoint::encode(&scene);
        let template = report::template(&scene);
        scene.apply_policy(policy::parse(template.as_bytes()).unwrap());
        assert!(scene.policy_warnings.is_empty(), "{name}");
        assert_eq!(scene.snapshot.object_blocks, expected, "{name}");
        if !clamped.contains(&name) {
            assert_eq!(mradm_scene::checkpoint::encode(&scene), before, "{name}");
        }
        scenes += 1;
    }
    clamped.sort();
    assert_eq!(
        (scenes, clamped),
        (
            14,
            vec![
                "objects-extent".to_owned(),
                "objects-extent-multi".to_owned(),
                "policy-objects".to_owned()
            ]
        )
    );
    std::fs::remove_dir_all(&temp).unwrap();
}
