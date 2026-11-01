//! Replays `request-v1`, the renderer resolution, DirectSpeakers routing and matrix validation and
//! the `{:.6g}` formatting the C++ engine produced before PCM consistency 5.2 B moved them to Rust
//! (see `request-v1/README.md`).
use common::{adm_wav, bed_wav, field};
use mradm_engine::request::{self, DsRouting, MatrixSource, Selection};
use mradm_json::Value;
use mradm_wav::SampleFormat;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

mod common;

fn dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/request-v1")
}

fn scenes(set: &str) -> PathBuf {
    if set == "request-v1" {
        return dir().join("scenes");
    }
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../mradm-scene/tests")
        .join(set)
        .join("scenes")
}

const SCENES: [(&str, &str); 20] = [
    ("policy-v1", "directspeakers"),
    ("policy-v1", "directspeakers-lfe"),
    ("policy-v1", "hoa"),
    ("policy-v1", "hoa3"),
    ("policy-v1", "objects-cartesian"),
    ("policy-v1", "objects-extent"),
    ("policy-v1", "objects-point"),
    ("policy-v1", "objects-room"),
    ("policy-v1", "params"),
    ("policy-v1", "policy-objects"),
    ("policy-v1", "types-from-ids"),
    ("export-v1", "common-bed"),
    ("export-v1", "shared-block"),
    ("request-v1", "ds-unlabelled"),
    ("request-v1", "ds-two-labels"),
    ("request-v1", "ds-lowpass-lfe"),
    ("request-v1", "ds-aliases"),
    ("policy-v1", "objects-extent-multi"),
    ("policy-v1", "hoa-pack-attributes"),
    ("policy-v1", "objects-stream-only"),
];

/// The matrix-validation scenes, written into `out`; returns the names in replay order.
pub fn write_inputs(out: &Path) -> Vec<String> {
    let mut names = Vec::new();
    for (set, name) in SCENES {
        adm_wav(
            &scenes(set),
            name,
            &out.join(format!("{name}.wav")),
            SampleFormat::Pcm24,
        );
        names.push(name.to_owned());
    }
    for layout in mradm_scene::input::input_layouts() {
        if layout.wave_channel_mask == 0 {
            continue;
        }
        let label = format!("bed-{}", layout.id);
        let channels = layout.wave_mask_channels.len() as u16;
        bed_wav(
            &out.join(format!("{label}.wav")),
            channels,
            48000,
            SampleFormat::Pcm16,
            Some(layout.wave_channel_mask),
            480,
        );
        names.push(label);
    }
    names
}

#[test]
fn general_format_matches_fmt() {
    let text = std::fs::read_to_string(dir().join("general.txt")).unwrap();
    let mut mismatches = Vec::new();
    for line in text.lines() {
        let (bits, expected) = line.split_once('\t').unwrap();
        let value = f64::from_bits(u64::from_str_radix(bits, 16).unwrap());
        let actual = mradm_engine::fmt::general(value, 6);
        if actual != expected {
            mismatches.push(format!("{bits} {value:e}: {actual} != {expected}"));
        }
    }
    assert!(
        mismatches.is_empty(),
        "{} mismatches:\n{}",
        mismatches.len(),
        mismatches[..mismatches.len().min(20)].join("\n")
    );
}

fn records(name: &str) -> Vec<Value> {
    std::fs::read_to_string(dir().join(name))
        .unwrap()
        .lines()
        .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
        .collect()
}

fn num(value: &Value, key: &str) -> u32 {
    field(value, key).as_f64().unwrap() as u32
}

fn string<'a>(value: &'a Value, key: &str) -> &'a str {
    field(value, key).as_str().unwrap()
}

fn error_json(e: &request::Error) -> String {
    format!("{} {} {}", e.code, e.message, e.context)
}

fn expected_error(record: &Value) -> Option<String> {
    record.as_object().unwrap().get("error").map(|e| {
        format!(
            "{} {} {}",
            num(e, "code"),
            string(e, "message"),
            string(e, "context")
        )
    })
}

#[test]
fn layout_aliases_match_the_cpp_factory() {
    let rows = records("layouts.jsonl");
    assert_eq!(rows.len(), 49);
    for r in &rows {
        assert_eq!(
            request::normalize_output_layout(string(r, "input")),
            string(r, "output"),
            "{:?}",
            string(r, "input")
        );
    }
}

#[test]
fn renderer_resolution_matches_the_cpp_factory() {
    let rows = records("renderers.jsonl");
    assert_eq!(rows.len(), 432);
    for r in &rows {
        let selection = Selection::from_u32(num(r, "selection")).unwrap();
        let allow = field(r, "allow_speaker_stereo").as_bool().unwrap();
        let actual = request::resolve_renderer(selection, string(r, "layout"), allow, false);
        match (expected_error(r), actual) {
            (Some(expected), Err(e)) => assert_eq!(error_json(&e), expected),
            (None, Ok(resolved)) => {
                assert_eq!(resolved.selected as u32, num(r, "selected"));
                assert_eq!(resolved.backend as u32, num(r, "backend"));
                assert_eq!(
                    resolved.effective_output_layout,
                    string(r, "effective_output_layout")
                );
                let diagnostics: Vec<(u32, &str)> = field(r, "diagnostics")
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(|d| {
                        let d = d.as_array().unwrap();
                        (d[0].as_f64().unwrap() as u32, d[1].as_str().unwrap())
                    })
                    .collect();
                let actual: Vec<(u32, &str)> = resolved
                    .diagnostics
                    .iter()
                    .map(|(l, m)| (*l as u32, m.as_str()))
                    .collect();
                assert_eq!(actual, diagnostics);
            }
            (expected, actual) => panic!(
                "{selection:?} {}: {expected:?} vs {actual:?}",
                string(r, "layout")
            ),
        }
    }
}

#[test]
fn routing_validation_matches_the_cpp_factory() {
    let rows = records("routing.jsonl");
    assert_eq!(rows.len(), 864);
    for r in &rows {
        let backend = Selection::from_u32(num(r, "backend")).unwrap();
        let mode = DsRouting::from_u32(num(r, "mode")).unwrap();
        let actual = request::validate_direct_speakers_routing(backend, string(r, "layout"), mode);
        assert_eq!(
            actual.err().map(|e| error_json(&e)),
            expected_error(r),
            "{backend:?} {mode:?}"
        );
    }
}

#[test]
fn matrix_resolution_matches_the_cpp_factory() {
    let out = std::env::temp_dir().join(format!("mradm-engine-request-{}", std::process::id()));
    std::fs::create_dir_all(&out).unwrap();
    write_inputs(&out);
    let matrices = dir().join("matrices");
    let missing = matrices.join("missing.json");
    let mut scenes = HashMap::new();
    let rows = records("matrix.jsonl");
    assert_eq!(rows.len(), 4550);
    let (mut ok, mut failed) = (0, 0);
    for r in &rows {
        let name = string(r, "scene");
        let scene = scenes.entry(name.to_owned()).or_insert_with(|| {
            let path = out.join(format!("{name}.wav"));
            mradm_scene::import(path.to_str().unwrap().as_bytes(), Default::default()).unwrap()
        });
        let file = matrices.join(format!("{}.json", string(r, "matrix")));
        let json = std::fs::read(&file).unwrap();
        let source_kind = string(r, "source");
        let path = match source_kind {
            "path" | "both" => Some(file.clone()),
            "missing" => Some(missing.clone()),
            _ => None,
        };
        let path_text = path.as_ref().map(|p| p.to_str().unwrap().to_owned());
        let label = path_text
            .as_deref()
            .unwrap_or("")
            .replace(matrices.to_str().unwrap(), "<dir>");
        let source = MatrixSource {
            json: matches!(source_kind, "json" | "both").then_some(json.as_slice()),
            path: path_text.as_deref(),
            path_label: &label,
        };
        let mode = DsRouting::from_u32(num(r, "mode")).unwrap();
        let outcome =
            request::resolve_direct_speakers_matrix(mode, source, scene, string(r, "layout"));
        let logs: Vec<String> = outcome
            .logs
            .iter()
            .map(|l| format!("{} {} {}", l.level as u32, l.module, l.message))
            .collect();
        let expected_logs: Vec<String> = field(r, "logs")
            .as_array()
            .unwrap()
            .iter()
            .map(|l| {
                let l = l.as_array().unwrap();
                format!(
                    "{} {} {}",
                    l[0].as_f64().unwrap(),
                    l[1].as_str().unwrap(),
                    l[2].as_str().unwrap()
                )
            })
            .collect();
        let what = format!(
            "{name} {} {source_kind} {mode:?} {}",
            string(r, "matrix"),
            string(r, "layout")
        );
        assert_eq!(logs, expected_logs, "{what}");
        match (expected_error(r), outcome.result) {
            (Some(expected), Err(e)) => {
                assert_eq!(error_json(&e), expected, "{what}");
                failed += 1;
            }
            (None, Ok(matrix)) => {
                let expected = field(r, "result");
                match matrix {
                    None => assert_eq!(expected, &Value::Null, "{what}"),
                    Some(m) => {
                        assert_eq!(m.output_layout, string(expected, "output_layout"), "{what}");
                        let routes = field(expected, "routes").as_array().unwrap();
                        assert_eq!(m.routes.len(), routes.len());
                        for (route, e) in m.routes.iter().zip(routes) {
                            let e = e.as_array().unwrap();
                            assert_eq!(route.source_label, e[0].as_str().unwrap());
                            assert_eq!(route.mute, e[1].as_bool().unwrap());
                            let targets = e[2].as_array().unwrap();
                            assert_eq!(route.targets.len(), targets.len());
                            for (t, e) in route.targets.iter().zip(targets) {
                                let e = e.as_array().unwrap();
                                assert_eq!(t.label, e[0].as_str().unwrap());
                                assert_eq!(t.weight.to_bits(), e[1].as_f64().unwrap() as u32);
                                assert_eq!(t.gain.to_bits(), e[2].as_f64().unwrap() as u32);
                            }
                        }
                        ok += 1;
                    }
                }
            }
            (expected, actual) => panic!("{what}: {expected:?} vs {actual:?}"),
        }
    }
    assert_eq!((ok, failed), (219, 3681));
    std::fs::remove_dir_all(&out).unwrap();
}

fn trim(start: f64, end: Option<f64>) -> Result<Option<(u64, u64)>, String> {
    let outcome = request::trim_window(start, end, 48000, 24000);
    outcome
        .result
        .map(|t| t.map(|t| (t.start_frame, t.frame_count)))
        .map_err(|e| e.message)
}

#[test]
fn trim_window_rules_and_messages() {
    assert_eq!(trim(0.0, None), Ok(None));
    assert_eq!(trim(0.1, None), Ok(Some((4800, 19200))));
    assert_eq!(trim(0.0, Some(0.25)), Ok(Some((0, 12000))));
    assert_eq!(
        trim(0.1, Some(9.0)),
        Ok(Some((4800, 19200))),
        "end clamps to the input"
    );
    let logs = request::trim_window(0.1, Some(0.2), 48000, 24000).logs;
    assert_eq!(
        logs[0].message,
        "output trim: frames [4800, 9600) of 24000 @ 48000Hz"
    );
    assert_eq!(logs[0].module, "engine");
    assert_eq!(
        trim(-1.0, None),
        Err("--start must be finite and >= 0 (got -1s)".into())
    );
    assert_eq!(
        trim(f64::NAN, None),
        Err("--start must be finite and >= 0 (got nans)".into())
    );
    assert_eq!(
        trim(0.0, Some(f64::INFINITY)),
        Err("--end must be finite and >= 0 (got infs)".into())
    );
    assert_eq!(
        trim(0.3, Some(0.2)),
        Err("--end (0.2s) must be greater than --start (0.3s)".into())
    );
    assert_eq!(
        trim(0.5, None),
        Err("--start (0.5s) is at or beyond the input duration (0.5s)".into())
    );
    assert_eq!(
        trim(0.1, Some(0.1000001)),
        Err("trim window [0.1s, 0.1s) is shorter than one sample frame at 48000Hz".into())
    );
    let unknown = request::trim_window(0.1, None, 0, 24000)
        .result
        .unwrap_err();
    assert_eq!(
        unknown.message,
        "cannot apply time-range trim: input sample rate is unknown"
    );
    assert_eq!(unknown.code, 1);
}

fn target<'a>(
    ext: &'a str,
    layout: &'a str,
    edit: impl FnOnce(&mut request::OutputRequest<'a>),
) -> request::Outcome<request::OutputTarget> {
    let mut r = request::OutputRequest {
        extension: ext,
        layout,
        bit_depth: request::BitDepth::F32,
        apac_caf: false,
        iamf_mp4: false,
        iamf_layers: &[],
        channels: None,
        iamf_available: false,
        packager: request::Packager::None,
        ffmpeg_major: -1,
    };
    edit(&mut r);
    request::output_target(&r)
}

fn message<T: std::fmt::Debug>(outcome: request::Outcome<T>) -> (i32, String) {
    let e = outcome.result.unwrap_err();
    (e.code, e.message)
}

#[test]
fn output_target_decisions() {
    let t = target(".FLAC", "0+5+0", |_| {}).result.unwrap();
    assert!(t.flac && t.lossy());
    let t = target(".wav", "4+7+0", |_| {}).result.unwrap();
    assert!(!t.lossy());
    assert!(target(".M4A", "4+7+0", |_| {}).result.unwrap().apac_mpeg4);
    assert!(
        target(".caf", "4+7+0", |r| r.apac_caf = true)
            .result
            .unwrap()
            .apac_caf
    );
    assert_eq!(
        message(target("", "4+7+0", |r| r.apac_caf = true)),
        (
            1,
            "--apac-container caf requires output path with .caf extension; got '<none>'".into()
        )
    );
    assert_eq!(
        message(target(".caf", "4+7+0", |r| r.bit_depth = request::BitDepth::I24)),
        (
            1,
            "--output-bit-depth i24 is not supported for .caf output (CAF is float32 only)".into()
        )
    );
    assert_eq!(
        message(target(".flac", "4+7+0", |_| {})),
        (
            2,
            "FLAC output supports only non-height layouts (binaural, 5.1, 7.1); layout '4+7+0' is not supported".into()
        )
    );
    assert_eq!(
        message(target(".flac", "wav71", |r| r.channels = Some(9))),
        (
            2,
            "FLAC supports 1-8 channels; layout 'wav71' renders 9 channels".into()
        )
    );
    // IAMF builds: layers, availability and the MP4 packager.
    let layers = ["5.1".to_owned(), "7.1.4".to_owned()];
    let ok = target(".iamf", "4+7+0", |r| {
        r.iamf_available = true;
        r.iamf_layers = &layers;
    });
    assert_eq!(ok.result.as_ref().unwrap().iamf_layers, ["0+5+0", "4+7+0"]);
    assert_eq!(ok.logs.len(), 1);
    assert!(
        ok.logs[0]
            .message
            .starts_with("IAMF scalable layers include flat layer '5.1' for height output '7.1.4'")
    );
    let reversed = ["7.1.4".to_owned(), "5.1".to_owned()];
    assert_eq!(
        message(target(".iamf", "0+5+0", |r| {
            r.iamf_available = true;
            r.iamf_layers = &reversed;
        })),
        (
            1,
            "IAMF scalable layers must be monotonic; '5.1' cannot follow '7.1.4'".into()
        )
    );
    assert_eq!(
        message(target(".iamf", "4+7+0", |r| {
            r.iamf_available = true;
            r.iamf_layers = &reversed;
        })),
        (
            1,
            "last IAMF scalable layer must match output layout '7.1.4'; got '5.1'".into()
        )
    );
    let ignored = target(".wav", "4+7+0", |r| r.iamf_layers = &layers);
    assert_eq!(
        ignored.logs[0].message,
        "ignoring IAMF scalable layers for non-IAMF output"
    );
    assert_eq!(
        message(target(".iamf", "9.1.6", |r| r.iamf_available = true)).0,
        2
    );
    assert!(
        message(target(".iamf", "4+7+0", |_| {}))
            .1
            .starts_with("IAMF output requires a build")
    );
    let mp4 = |packager, major| {
        target(".mp4", "4+7+0", move |r| {
            r.iamf_mp4 = true;
            r.iamf_available = true;
            r.packager = packager;
            r.ffmpeg_major = major;
        })
    };
    assert!(
        message(mp4(request::Packager::None, -1))
            .1
            .ends_with("install GPAC: https://gpac.io")
    );
    let old = mp4(request::Packager::Ffmpeg, 6);
    assert!(old.result.as_ref().unwrap().iamf_mp4 && !old.result.as_ref().unwrap().apac_mpeg4);
    assert!(old.logs[0].message.starts_with("ffmpeg 6 detected"));
    assert!(mp4(request::Packager::Ffmpeg, 7).logs.is_empty());
    assert!(mp4(request::Packager::Mp4box, -1).logs.is_empty());
    assert_eq!(
        message(target(".m4a", "4+7+0", |r| {
            r.iamf_mp4 = true;
            r.iamf_available = true;
        })),
        (
            1,
            "--iamf-container mp4 requires output path with .mp4 extension; got '.m4a'".into()
        )
    );
    assert_eq!(
        message(target(".mp4", "4+7+0", |r| r.iamf_mp4 = true)),
        (
            2,
            "--iamf-container mp4 requires a build configured with MR_ADM_ENABLE_IAMF=ON".into()
        )
    );
}
