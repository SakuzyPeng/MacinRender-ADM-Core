//! Room-binaural file renders (formerly the file half of the C++ `room_render_test`, R3 C2) on a
//! moving Cartesian object: the offline render service appends the complete tail and equals the
//! fragmented file stream bit for bit; seeks inside and after the material reproduce it; a window
//! equals its slice; a live mute is immediate and survives a seek; a screen mapping changes the
//! output identically in both paths. Each check runs for both qualities, a distance-dependent HRTF
//! collection, a room with the listener near a wall, and SOFA sets in place of the package HRIR.
//! The switch rules, source growth and resets are `mradm-render`'s `room_runtime.rs`; the room
//! Scene stream through the C ABI is the C++ `room_render_test`.
mod common;
use common::backend::{TestHost, prepared, request as backend_request};
use common::render::*;
use mradm_dsp::room::loading::{self, Package};
use mradm_engine::backend::{Kind, Screens, Sofa};
use mradm_engine::render::{Request, RoomOptions, SofaPath};
use mradm_engine::request::Selection;
use mradm_engine::stream::{self, Body, Settings};
use mradm_render::binaural::Spread;
use mradm_render::routing::{LiveOverride, Override};
use mradm_testkit::wave::{AudioId, Wave};
use std::path::{Path, PathBuf};
use std::sync::Arc;

const SCREEN: &[u8] = br#"{"schema":"mradm.screen-config","version":1,"reproduction":{"coordinates":"polar","centre":[0,0,1],"width":90,"aspect_ratio":1.78}}"#;

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/fixtures")
}

/// The moving object: two Cartesian blocks split at 17913 frames, the second ramping over 3317.
fn moving(path: &Path, input: &[f32], screen_ref: bool) {
    let screen = if screen_ref {
        "<screenRef>1</screenRef>"
    } else {
        ""
    };
    let position = |x: &str, y: &str| {
        format!(
            r#"<position coordinate="X">{x}</position><position coordinate="Y">{y}</position><position coordinate="Z">0</position><cartesian>1</cartesian>"#
        )
    };
    let xml = format!(
        r#"<?xml version="1.0" encoding="UTF-8"?><ebuCoreMain><coreMetadata><format><audioFormatExtended><audioChannelFormat audioChannelFormatID="AC_00031001" audioChannelFormatName="moving" typeLabel="0003"><audioBlockFormat audioBlockFormatID="AB_00031001_00000001" rtime="00:00:00.000000000" duration="00:00:00.373187500">{}{screen}</audioBlockFormat><audioBlockFormat audioBlockFormatID="AB_00031001_00000002" rtime="00:00:00.373187500">{}<jumpPosition interpolationLength="0.0691">0</jumpPosition>{screen}</audioBlockFormat></audioChannelFormat><audioPackFormat audioPackFormatID="AP_00031001" audioPackFormatName="moving" typeLabel="0003"><audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef></audioPackFormat><audioStreamFormat audioStreamFormatID="AS_00031001" audioStreamFormatName="moving" formatDefinition="pcm" formatLabel="0001"><audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef><audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef></audioStreamFormat><audioTrackFormat audioTrackFormatID="AT_00031001_01" audioTrackFormatName="moving" formatDefinition="pcm" formatLabel="0001"><audioStreamFormatIDRef>AS_00031001</audioStreamFormatIDRef></audioTrackFormat><audioTrackUID UID="ATU_00000001"><audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef><audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef></audioTrackUID><audioObject audioObjectID="AO_1001" audioObjectName="moving"><audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef><audioTrackUIDRef>ATU_00000001</audioTrackUIDRef></audioObject><audioContent audioContentID="ACO_1001" audioContentName="C"><audioObjectIDRef>AO_1001</audioObjectIDRef></audioContent><audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="P"><audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme></audioFormatExtended></format></coreMetadata></ebuCoreMain>"#,
        position("0", "1"),
        position("-0.8", "1.3")
    );
    let mut wave = Wave::new(1, 48000, 24);
    wave.chna = Some(vec![AudioId::new(
        1,
        "ATU_00000001",
        "AT_00031001_01",
        "AP_00031001",
    )]);
    wave.axml = Some(xml);
    wave.write(path, input).unwrap();
}

/// A copy of the synthetic room in `dir` with `edits` applied to its manifest, without the files
/// in `remove`.
fn variant(dir: &Path, edits: &[(&str, &str)], remove: &[&str]) -> PathBuf {
    let source = fixtures().join("room/synthetic");
    std::fs::create_dir_all(dir).unwrap();
    for entry in std::fs::read_dir(&source).unwrap() {
        let entry = entry.unwrap();
        std::fs::copy(entry.path(), dir.join(entry.file_name())).unwrap();
    }
    let mut text = std::fs::read_to_string(source.join("room.json")).unwrap();
    for (from, to) in edits {
        assert!(text.contains(from), "{from}");
        text = text.replace(from, to);
    }
    std::fs::write(dir.join("room.json"), text).unwrap();
    for name in remove {
        std::fs::remove_file(dir.join(name)).unwrap();
    }
    dir.join("room.json")
}

#[derive(Clone)]
struct Case {
    manifest: PathBuf,
    hrir: String,
    sofa: Option<PathBuf>,
    metres: f64,
    quality: u32,
}

impl Case {
    fn package(&self) -> Arc<Package> {
        Arc::new(loading::load_package(self.manifest.to_str().unwrap().as_bytes()).unwrap())
    }
}

fn screens(screened: bool) -> Screens {
    let mut s = Screens {
        has_reference: false,
        reference: Default::default(),
        has_reproduction: false,
        reproduction: Default::default(),
    };
    if screened {
        let parsed = mradm_scene::screen::parse(SCREEN).unwrap();
        if let Some(g) = parsed.reproduction {
            s.has_reproduction = true;
            s.reproduction = g;
        }
    }
    s
}

/// The offline render service's output, or its error code.
fn offline(
    case: &Case,
    input: &Path,
    dir: &Dir,
    screened: bool,
    window: Option<(usize, usize)>,
) -> Result<Audio, i32> {
    let out = dir.file(&format!("offline-{}.wav", window.map_or(0, |w| w.0)));
    let mut r: Request = request(input, &out, Selection::RoomBinaural, "binaural");
    r.peak_limit = false;
    r.room = RoomOptions {
        package: Some(case.package()),
        hrir: case.hrir.clone().into_bytes(),
        metres: case.metres,
        quality: case.quality,
    };
    r.sofa = case.sofa.as_ref().map(|p| SofaPath {
        path: text(p),
        file_name: p
            .file_name()
            .unwrap()
            .to_string_lossy()
            .into_owned()
            .into_bytes(),
    });
    r.screens = screens(screened);
    if let Some((start, count)) = window {
        r.render_start_sec = start as f64 / 48000.;
        r.render_end_sec = Some((start + count) as f64 / 48000.);
    }
    let result = run(r);
    if !result.ok() {
        return Err(result.code());
    }
    Ok(Audio::read(&out))
}

fn open(case: &Case, input: &Path, screened: bool) -> (stream::Validator, Body) {
    let scene = common::backend::import(input);
    let path = input.to_str().unwrap();
    let package = case.package();
    let sofa_display = case
        .sofa
        .as_ref()
        .map(|p| p.to_string_lossy().into_owned().into_bytes());
    let sofa_name = case.sofa.as_ref().map(|p| {
        p.file_name()
            .unwrap()
            .to_string_lossy()
            .into_owned()
            .into_bytes()
    });
    let mut r = backend_request(Kind::Room, b"binaural", path.as_bytes());
    r.room.package = Some(package);
    r.room.hrir = case.hrir.as_bytes();
    r.room.metres = case.metres;
    r.room.quality = case.quality;
    if let (Some(display), Some(name)) = (&sofa_display, &sofa_name) {
        r.sofa = Some(Sofa {
            path: display,
            file_name: name,
        });
    }
    r.screens = screens(screened);
    let (_, p) = prepared(&scene, &r);
    let settings = Settings {
        path,
        channels: 1,
        frames: scene.info.frames,
        sample_rate: 48000,
        default_interp_ms: 5,
        smoothing_frames: 0,
        spread: Spread::Cloud,
        pose: [0.; 3],
    };
    let (validator, body, _) =
        stream::open(&p, &settings, &TestHost).unwrap_or_else(|e| panic!("{}", e.message));
    (validator, body)
}

fn pull(body: &mut Body, sizes: &[usize], limit: usize) -> Vec<f32> {
    let mut out = Vec::new();
    let mut buffer = vec![0f32; 2 * sizes.iter().max().unwrap()];
    for &size in sizes.iter().cycle() {
        let want = size.min(limit - out.len() / 2);
        if want == 0 {
            break;
        }
        let got = body.process(&mut buffer, want).unwrap();
        if got == 0 {
            break;
        }
        out.extend_from_slice(&buffer[..2 * got]);
    }
    out
}

fn seek(body: &mut Body, frame: u64) {
    let mut pending = body.seek_begin(frame, false).unwrap();
    while pending {
        pending = body.seek_step(false).unwrap();
    }
}

fn file_checks(case: &Case, name: &str) {
    let dir = scratch("room");
    let input: Vec<f32> = (0..29011).map(|i| ((i % 89) - 44) as f32 / 4096.).collect();
    let path = dir.file("input.wav");
    moving(&path, &input, false);
    let reference = offline(case, &path, &dir, false, None)
        .unwrap_or_else(|c| panic!("{name}: {c}"))
        .samples;
    assert!(
        reference.len() / 2 > input.len(),
        "{name}: the offline render appends the tail"
    );
    let (validator, mut body) = open(case, &path, false);
    let streamed = pull(&mut body, &[1, 173, 4097, 255, 19, 2048], usize::MAX);
    assert_eq!(
        streamed, reference,
        "{name}: fragmented stream equals offline"
    );
    for start in [20111usize, 28333, input.len() + 371] {
        seek(&mut body, start as u64);
        assert_eq!(
            pull(&mut body, &[701], 701),
            reference[start * 2..(start + 701) * 2],
            "{name}: seek {start}"
        );
    }
    let window = offline(case, &path, &dir, false, Some((20111, 877)))
        .unwrap()
        .samples;
    assert_eq!(
        window,
        reference[40222..40222 + 1754],
        "{name}: exact window"
    );

    let edit = LiveOverride {
        base: Override {
            object_id: b"AO_1001",
            speaker_label: b"",
            mute: true,
            gain_db: 0.,
            head_locked: Some(true),
        },
        extent_scale: 1.,
        extent_width_scale: 1.,
        extent_height_scale: 1.,
        extent_depth_scale: 1.,
        diffuse_scale: 1.,
        divergence_scale: 1.,
    };
    validator.validate(&[edit]).unwrap();
    let (_, mut edited) = open(case, &path, false);
    edited.set_overrides(&[edit]).unwrap();
    assert!(
        pull(&mut edited, &[2048], 2048).iter().all(|v| *v == 0.),
        "{name}: an initial mute is immediate"
    );
    seek(&mut edited, 17011);
    assert!(
        pull(&mut edited, &[701], 701).iter().all(|v| *v == 0.),
        "{name}: the mute survives a seek"
    );

    let screened_path = dir.file("input-screen.wav");
    moving(&screened_path, &input, true);
    let expected = offline(case, &screened_path, &dir, true, None)
        .unwrap()
        .samples;
    assert_ne!(
        expected, reference,
        "{name}: the screen mapping moves the off-centre source"
    );
    let (_, mut screened) = open(case, &screened_path, true);
    assert_eq!(
        pull(&mut screened, &[349], usize::MAX),
        expected,
        "{name}: screened stream equals offline"
    );
    seek(&mut screened, 20111);
    assert_eq!(
        pull(&mut screened, &[349], 349),
        expected[40222..40222 + 698],
        "{name}: screened seek"
    );
}

fn synthetic() -> PathBuf {
    fixtures().join("room/synthetic/room.json")
}

#[test]
fn files_equal_their_streams_for_both_qualities() {
    for quality in [0, 1] {
        let case = Case {
            manifest: synthetic(),
            hrir: String::new(),
            sofa: None,
            metres: 1.5,
            quality,
        };
        file_checks(&case, &format!("synthetic q{quality}"));
    }
}

/// The SOFA-backed cases need the `sofa` feature (on in the workspace build); without it the
/// preparation reports unsupported and they are skipped.
fn sofa_available(case: &Case) -> bool {
    let dir = scratch("room-probe");
    let path = dir.file("probe.wav");
    moving(&path, &[0.; 4800], false);
    match offline(case, &path, &dir, false, None) {
        Ok(_) => true,
        Err(2) => false,
        Err(code) => panic!("probe failed with {code}"),
    }
}

#[test]
fn files_equal_their_streams_with_distance_and_sofa_sets() {
    let sofa = fixtures().join("sofa");
    let collection = sofa
        .join("distance/distance_index.json")
        .to_string_lossy()
        .into_owned();
    let probe = Case {
        manifest: synthetic(),
        hrir: collection.clone(),
        sofa: None,
        metres: 0.5,
        quality: 0,
    };
    if !sofa_available(&probe) {
        eprintln!("SOFA support is not built; skipped");
        return;
    }
    let near_dir = std::env::temp_dir().join(format!("mradm-room-near-{}", std::process::id()));
    let near = variant(
        &near_dir,
        &[
            (
                r#""listener": [3.4, 3, 1.2]"#,
                r#""listener": [0.4, 3, 1.2]"#,
            ),
            (r#""minimum_distance": 0.25"#, r#""minimum_distance": 0.2"#),
        ],
        &[],
    );
    let stored_dir = std::env::temp_dir().join(format!("mradm-room-stored-{}", std::process::id()));
    let stored = variant(&stored_dir, &[], &["hrir.json"]);
    let case =
        |manifest: &PathBuf, hrir: &str, sofa: Option<PathBuf>, metres: f64, quality: u32| Case {
            manifest: manifest.clone(),
            hrir: hrir.to_owned(),
            sofa,
            metres,
            quality,
        };
    for (name, c) in [
        (
            "collection q0",
            case(&synthetic(), &collection, None, 0.5, 0),
        ),
        (
            "collection q1",
            case(&synthetic(), &collection, None, 0.5, 1),
        ),
        ("near q0", case(&near, &collection, None, 0.3, 0)),
        ("near q1", case(&near, &collection, None, 0.3, 1)),
        (
            "collection far.sofa",
            case(
                &synthetic(),
                &collection,
                Some(sofa.join("distance/far.sofa")),
                1.5,
                0,
            ),
        ),
        (
            "simple.sofa q0",
            case(&stored, "", Some(sofa.join("simple.sofa")), 1.5, 0),
        ),
        (
            "simple.sofa q1",
            case(&stored, "", Some(sofa.join("simple.sofa")), 1.5, 1),
        ),
    ] {
        file_checks(&c, name);
    }
    let _ = std::fs::remove_dir_all(near_dir);
    let _ = std::fs::remove_dir_all(stored_dir);
}
