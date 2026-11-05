//! The monitor's stream opening (PCM consistency 5.3 B): `stream::open_monitor` against the frozen
//! C++ `RealtimeStreamFactory` (monitor-v1), and its order, texts and plan rules.
use mradm_dsp::room::loading;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::Screens;
use mradm_engine::render::{PathText, Request, RoomOptions, SofaPath};
use mradm_engine::request::{Error, Log};
use mradm_engine::stream::{self, Monitor};
use mradm_json::Value;
use mradm_scene::RenderScene;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

mod common;
use common::backend::TestHost;
use common::field;

fn dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/monitor-v1")
}

fn matrices() -> String {
    format!("{}/tests/request-v1/matrices", env!("CARGO_MANIFEST_DIR"))
}

fn fixtures() -> String {
    format!("{}/../../../tests/fixtures", env!("CARGO_MANIFEST_DIR"))
}

fn records(name: &str) -> Vec<Value> {
    std::fs::read_to_string(dir().join(name))
        .unwrap()
        .lines()
        .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
        .collect()
}

fn text(path: &str) -> PathText {
    PathText {
        utf8: path.to_owned(),
        display: path.to_owned(),
    }
}

fn get<'a>(v: &'a Value, key: &str) -> Option<&'a Value> {
    v.as_object().unwrap().get(key)
}

fn flag(v: &Value, key: &str) -> bool {
    get(v, key).and_then(Value::as_bool).unwrap_or(false)
}

fn int(v: &Value, key: &str, default: u32) -> u32 {
    get(v, key)
        .and_then(Value::as_f64)
        .map_or(default, |n| n as u32)
}

const SCREEN: RoomScreenGeometry = RoomScreenGeometry {
    cartesian: 0,
    reserved: 0,
    aspect_ratio: 1.78,
    centre: [0., 0., 1.],
    width: 58.,
};

/// The `RenderOptions` of a monitor-v1 case as the C++ request record carries them.
fn request(case: &Value, input: &str, room: &Arc<loading::Package>) -> Request {
    let mut r = Request {
        input: text(input),
        output: None,
        renderer: int(case, "renderer", 0),
        output_layout: field(case, "layout").as_str().unwrap().to_owned(),
        input_layout: None,
        input_channel_labels: Vec::new(),
        measure_loudness: false,
        loudness_target_lufs: -23.,
        peak_limit: true,
        peak_limit_dbtp: -1.,
        peak_normalize_to_limit: false,
        final_gain_db: 0.,
        output_bit_depth: 0,
        opus_bitrate_per_ch_kbps: 0,
        apac_bitrate_kbps: 0,
        apac_drc_music: true,
        apac_caf: false,
        sofa: None,
        semantic_policy_path: None,
        semantic_policy_json: None,
        matrix_path: None,
        matrix_json: None,
        semantic_report_path: None,
        capture_semantic_report: false,
        default_interp_ms: int(case, "interp", 5),
        object_smoothing_frames: int(case, "smoothing", 0),
        apple_geometry: flag(case, "apple_geometry"),
        routing: int(case, "routing", 0),
        speaker_spread: int(case, "speaker_spread", 0),
        triple_balance_mode: u32::from(flag(case, "d")),
        binaural_spread: int(case, "binaural_spread", 0),
        split_power: flag(case, "split_power"),
        apple_spatial_preset: 0,
        apple_speaker_rendering_flags: false,
        listener_orientation: [0.; 3],
        render_start_sec: 0.,
        render_end_sec: None,
        room: RoomOptions::default(),
        screens: Screens {
            has_reference: flag(case, "screens"),
            reference: SCREEN,
            has_reproduction: flag(case, "reproduction"),
            reproduction: if flag(case, "reproduction") {
                RoomScreenGeometry {
                    aspect_ratio: 1.5,
                    width: 40.,
                    ..SCREEN
                }
            } else {
                SCREEN
            },
        },
        internal_allow_speaker_stereo: flag(case, "allow_speaker_stereo"),
        iamf_mp4: false,
        iamf_layers: Vec::new(),
    };
    if let Some(m) = get(case, "matrix").filter(|m| !matches!(m, Value::Null)) {
        if let Some(name) = get(m, "json") {
            let file = format!("{}/{}", matrices(), name.as_str().unwrap());
            r.matrix_json = Some(std::fs::read(file).unwrap());
        }
        if let Some(name) = get(m, "path") {
            r.matrix_path = Some(text(&format!("{}/{}", matrices(), name.as_str().unwrap())));
        }
    }
    if let Some(o) = get(case, "orientation") {
        let a = o.as_array().unwrap();
        r.listener_orientation = [0, 1, 2].map(|i| a[i].as_f64().unwrap() as f32);
    }
    if let Some(name) = get(case, "sofa") {
        let name = name.as_str().unwrap();
        r.sofa = Some(SofaPath {
            path: text(&format!("{}/sofa/{name}", fixtures())),
            file_name: name.as_bytes().to_vec(),
        });
    }
    if flag(case, "room") {
        r.room.package = Some(Arc::clone(room));
    }
    r
}

/// The record the generator wrote for an outcome.
fn outcome(
    logs: &[Log],
    result: &Result<Monitor, Error>,
    normalize: &dyn Fn(&str) -> String,
) -> (Vec<(u32, String, String)>, String) {
    let lines = logs
        .iter()
        .map(|l| (l.level as u32, l.module.to_owned(), normalize(&l.message)))
        .collect();
    let rest = match result {
        Err(e) => format!(
            "error {} {} | {}",
            e.code,
            normalize(&e.message),
            normalize(&e.context)
        ),
        Ok(Monitor::Stream {
            body, info, layout, ..
        }) => format!(
            "stream {} {} {} {} {} {}",
            info.outputs,
            info.sample_rate,
            if info.binaural { "binaural" } else { layout },
            info.requires_preroll,
            body.tail_frames(),
            body.control().is_some()
        ),
        Ok(Monitor::Apple { layout, .. }) => format!("apple {layout}"),
    };
    (lines, rest)
}

fn expected(row: &Value) -> (Vec<(u32, String, String)>, String) {
    let lines = field(row, "logs")
        .as_array()
        .unwrap()
        .iter()
        .map(|l| {
            let l = l.as_array().unwrap();
            (
                l[0].as_f64().unwrap() as u32,
                l[1].as_str().unwrap().to_owned(),
                l[2].as_str().unwrap().to_owned(),
            )
        })
        .collect();
    let rest = if let Some(e) = get(row, "error") {
        format!(
            "error {} {} | {}",
            field(e, "code").as_f64().unwrap(),
            field(e, "message").as_str().unwrap(),
            field(e, "context").as_str().unwrap()
        )
    } else {
        let s = field(row, "stream");
        format!(
            "stream {} {} {} {} {} {}",
            field(s, "channels").as_f64().unwrap(),
            field(s, "rate").as_f64().unwrap(),
            field(s, "layout").as_str().unwrap(),
            field(s, "preroll").as_bool().unwrap(),
            field(s, "tail").as_f64().unwrap(),
            field(s, "room_control").as_bool().unwrap()
        )
    };
    (lines, rest)
}

#[test]
fn monitor_opening_matches_the_cpp_factory() {
    let inputs = std::env::temp_dir().join(format!("mradm-engine-monitor-{}", std::process::id()));
    common::monitor::write_inputs(&inputs);
    let room = Arc::new(
        loading::load_package(format!("{}/room/phase2/room.json", fixtures()).as_bytes()).unwrap(),
    );
    let input_dir = inputs.to_str().unwrap().to_owned();
    let normalize = |s: &str| {
        s.replace(&input_dir, "<IN>")
            .replace(&matrices(), "<MATRICES>")
            .replace(&fixtures(), "<FIXTURES>")
    };
    let cases = records("cases.jsonl");
    let rows = records("expected.jsonl");
    assert_eq!((cases.len(), rows.len()), (2960, 2960));
    let mut scenes: HashMap<String, RenderScene> = HashMap::new();
    let (mut opened, mut sofa_skipped) = (0, 0);
    for (case, row) in cases.iter().zip(&rows) {
        let rate = int(case, "rate", 48000);
        let name = field(case, "scene").as_str().unwrap();
        let input = format!("{input_dir}/{name}-{rate}.wav");
        let scene = scenes
            .entry(input.clone())
            .or_insert_with(|| mradm_scene::import_adm(input.as_bytes()).unwrap());
        let r = request(case, &input, &room);
        let o = stream::open_monitor(
            &r,
            flag(case, "system_spatial"),
            Some(scene),
            false,
            &TestHost,
        );
        // A crate test without the workspace's `sofa` feature cannot load SOFA fixtures.
        if let Err(e) = &o.result
            && get(case, "sofa").is_some()
            && e.message
                .starts_with("SOFA loading is disabled in this build")
        {
            sofa_skipped += 1;
            continue;
        }
        opened += usize::from(o.result.is_ok());
        assert_eq!(
            outcome(&o.logs, &o.result, &normalize),
            expected(row),
            "case {}",
            field(case, "id").as_f64().unwrap()
        );
    }
    if sofa_skipped == 0 {
        assert_eq!(opened, 282);
    } else {
        assert_eq!(sofa_skipped, 4);
    }
    let _ = std::fs::remove_dir_all(&inputs);
}

/// One opened stream's first 4096 frames.
fn render_bits(o: Monitor) -> Vec<u32> {
    let Monitor::Stream { mut body, info, .. } = o else {
        panic!("not a stream");
    };
    let mut out = vec![0.; 4096 * info.outputs];
    assert_eq!(body.process(&mut out, 4096).unwrap(), 4096);
    out.into_iter().map(f32::to_bits).collect()
}

#[test]
fn system_spatial_monitoring_opens_orientation_neutral() {
    let dir = common::backend::Dir::new("monitor-pose");
    let wav = dir.file("objects-point.wav");
    common::backend::scene_wav("objects-point", &wav, 48000, 48000);
    let input = wav.to_str().unwrap();
    let scene = mradm_scene::import_adm(input.as_bytes()).unwrap();
    let room = Arc::new(
        loading::load_package(format!("{}/room/phase2/room.json", fixtures()).as_bytes()).unwrap(),
    );
    let case = |orientation: &str| {
        mradm_json::parse(
            format!(r#"{{"scene":"objects-point","renderer":8,"layout":"binaural","room":true,"orientation":{orientation}}}"#)
                .as_bytes(),
        )
        .unwrap()
    };
    let open = |c: &Value, system_spatial: bool| {
        let o = stream::open_monitor(
            &request(c, input, &room),
            system_spatial,
            Some(&scene),
            false,
            &TestHost,
        );
        render_bits(o.result.unwrap())
    };
    let turned = case("[90.0, 0.0, 0.0]");
    let neutral = open(&case("[0.0, 0.0, 0.0]"), false);
    assert_ne!(
        open(&turned, false),
        neutral,
        "the orientation reaches the room"
    );
    assert_eq!(open(&turned, true), neutral);
}

#[test]
fn the_apple_backend_is_an_island() {
    let inputs =
        std::env::temp_dir().join(format!("mradm-engine-monitor-apple-{}", std::process::id()));
    common::monitor::write_inputs(&inputs);
    let input = format!("{}/directspeakers-48000.wav", inputs.to_str().unwrap());
    let scene = mradm_scene::import_adm(input.as_bytes()).unwrap();
    let room = Arc::new(
        loading::load_package(format!("{}/room/phase2/room.json", fixtures()).as_bytes()).unwrap(),
    );
    let open = |text: &str, apple: bool| {
        let c = mradm_json::parse(text.as_bytes()).unwrap();
        stream::open_monitor(
            &request(&c, &input, &room),
            false,
            Some(&scene),
            apple,
            &TestHost,
        )
    };
    // Unavailable: the request rules reject it (the C++ RendererSelection value in the text).
    let o = open(r#"{"renderer":4,"layout":"binaural"}"#, false);
    assert_eq!(
        o.result.err().unwrap().message,
        "renderer '4' is not available in this build"
    );
    let o = open(r#"{"renderer":4,"layout":"5.1"}"#, true);
    let Ok(Monitor::Apple { layout, matrix }) = o.result else {
        panic!("the Apple backend opens as an island");
    };
    assert_eq!((layout.as_str(), matrix.is_none()), ("0+5+0", true));
    // The matrix is resolved for the island; screens still reject a non-room backend first.
    let o = open(
        r#"{"renderer":4,"layout":"5.1","routing":3,"matrix":{"json":"full51.json"}}"#,
        true,
    );
    let Ok(Monitor::Apple { matrix, .. }) = o.result else {
        panic!("the Apple backend opens as an island");
    };
    assert!(matrix.is_some());
    let o = open(r#"{"renderer":4,"layout":"binaural","screens":true}"#, true);
    assert_eq!(
        o.result.err().unwrap().message,
        "屏幕配置当前仅支持 room-binaural"
    );
    // A missing scene is rejected after the routing checks.
    let c = mradm_json::parse(br#"{"renderer":2,"layout":"5.1"}"#).unwrap();
    let o = stream::open_monitor(&request(&c, &input, &room), false, None, false, &TestHost);
    assert_eq!(
        o.result.err().unwrap().message,
        "monitor requires the Rust render scene"
    );
    let _ = std::fs::remove_dir_all(&inputs);
}
