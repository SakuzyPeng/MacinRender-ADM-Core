//! The file stream of every backend (PCM consistency 5.3 A): the C++ `open_stream` checks moved
//! into the engine, the per-backend cancellation points of the stepped seek, and the stream's
//! format queries.
use mradm_dsp::room::loading;
use mradm_engine::backend::{Kind, Prepared};
use mradm_engine::stream::{self, Body, Info, Settings, Validator};
use mradm_render::binaural::Spread;
use mradm_render::offline::Error;
use mradm_render::routing::{LiveOverride, Override};
use mradm_scene::RenderScene;
use std::path::Path;
use std::sync::Arc;

mod common;
use common::backend::*;

const FRAMES: usize = 48000;

fn settings<'a>(scene: &RenderScene, path: &'a str) -> Settings<'a> {
    let info = &scene.info;
    Settings {
        path,
        channels: usize::from(info.channels),
        frames: info.frames,
        sample_rate: info.sample_rate,
        default_interp_ms: 5,
        smoothing_frames: 0,
        spread: Spread::Cloud,
        pose: [0.; 3],
    }
}

fn open(p: &Prepared, s: &Settings) -> Result<(Validator, Body, Info), Error> {
    stream::open(p, s, &TestHost)
}

fn opened(p: &Prepared, s: &Settings) -> (Validator, Body, Info) {
    open(p, s).unwrap_or_else(|e| panic!("{:?}: {}", p.kind, e.message))
}

fn rejected(p: &Prepared, s: &Settings) -> (i32, String) {
    match open(p, s) {
        Ok(_) => panic!("{:?} opened", p.kind),
        Err(e) => (e.code, e.message),
    }
}

/// A scene file and its import.
struct Input {
    _dir: Dir,
    path: String,
    scene: RenderScene,
}

fn input(tag: &str, name: &str, rate: u32) -> Input {
    let dir = Dir::new(tag);
    let wav = dir.file(&format!("{name}.wav"));
    scene_wav(name, &wav, FRAMES, rate);
    Input {
        scene: mradm_scene::import(wav.to_str().unwrap().as_bytes(), Default::default()).unwrap(),
        path: wav.to_str().unwrap().to_owned(),
        _dir: dir,
    }
}

fn prepare(i: &Input, kind: Kind, layout: &[u8], d: bool) -> Arc<Prepared> {
    let mut r = request(kind, layout, i.path.as_bytes());
    r.triple_balance_d = d;
    if kind == Kind::Room {
        let manifest = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../tests/fixtures/room/phase2/room.json");
        r.room.package = Some(Arc::new(
            loading::load_package(manifest.to_str().unwrap().as_bytes()).unwrap(),
        ));
    }
    prepared(&i.scene, &r).1
}

/// Each seek call samples the stop token once; `cancel_at` is the call (0 = begin) that sees it
/// requested.
fn seek(body: &mut Body, frame: u64, cancel_at: Option<usize>) -> Result<usize, Error> {
    let mut calls = 0;
    let mut pending = body.seek_begin(frame, cancel_at == Some(0))?;
    while pending {
        calls += 1;
        pending = body.seek_step(cancel_at == Some(calls))?;
    }
    Ok(calls)
}

fn cancel_message(body: &mut Body, frame: u64, at: usize) -> Option<(i32, String)> {
    seek(body, frame, Some(at))
        .err()
        .map(|e| (e.code, e.message))
}

#[test]
fn open_checks_follow_the_cpp_shells() {
    let objects = input("stream-open", "objects-room", 48000);
    let s = settings(&objects.scene, &objects.path);

    // HOA compares the plan's input format with the preparation.
    let hoa = prepare(&objects, Kind::Hoa, b"hoa3", false);
    let mut wrong = settings(&objects.scene, &objects.path);
    wrong.channels += 1;
    assert_eq!(
        rejected(&hoa, &wrong),
        (1, "HOA prepared/input format mismatch".to_owned())
    );
    wrong = settings(&objects.scene, &objects.path);
    wrong.sample_rate = 44100;
    assert_eq!(
        rejected(&hoa, &wrong).1,
        "HOA prepared/input format mismatch"
    );
    let (_, _, info) = opened(&hoa, &s);
    assert_eq!(
        info,
        Info {
            outputs: 16,
            sample_rate: 48000,
            binaural: false,
            requires_preroll: false
        }
    );

    // Triple Balance: a D-mode preparation, then the stream format.
    let d = prepare(&objects, Kind::TripleBalance, b"9.1.6", true);
    assert_eq!(
        rejected(&d, &s),
        (
            2,
            "Triple Balance D mode currently supports offline rendering only".to_owned()
        )
    );
    let tb = prepare(&objects, Kind::TripleBalance, b"4+7+0", false);
    wrong = settings(&objects.scene, &objects.path);
    wrong.channels = 0;
    assert_eq!(
        rejected(&tb, &wrong),
        (1, "triple-balance: invalid stream format".to_owned())
    );
    wrong = settings(&objects.scene, &objects.path);
    wrong.sample_rate = 96000;
    assert_eq!(
        rejected(&tb, &wrong).1,
        "triple-balance: invalid stream format"
    );
    let (_, _, info) = opened(&tb, &s);
    assert_eq!(
        (info.outputs, info.sample_rate, info.binaural),
        (12, 48000, false)
    );

    // The room compares frames, channels and its own rate.
    let room = prepare(&objects, Kind::Room, b"binaural", false);
    for change in 0..3 {
        let mut wrong = settings(&objects.scene, &objects.path);
        match change {
            0 => wrong.frames += 1,
            1 => wrong.channels += 1,
            _ => wrong.sample_rate = 44100,
        }
        assert_eq!(
            rejected(&room, &wrong),
            (1, "room-binaural prepared 输入格式不匹配".to_owned())
        );
    }
    let (_, body, info) = opened(&room, &s);
    assert_eq!(
        info,
        Info {
            outputs: 2,
            sample_rate: 48000,
            binaural: true,
            requires_preroll: true
        }
    );
    assert!(body.control().is_some());
    assert!(body.tail_frames() > 0);

    // Loudspeaker streams report the plan's rate and keep the plan's layout.
    for (kind, layout, outputs) in [(Kind::Vbap, &b"4+7+0"[..], 12), (Kind::Ear, b"0+5+0", 6)] {
        let p = prepare(&objects, kind, layout, false);
        let (validator, body, info) = opened(&p, &s);
        assert_eq!(
            info,
            Info {
                outputs,
                sample_rate: 48000,
                binaural: false,
                requires_preroll: false
            }
        );
        assert!(matches!(validator, Validator::None));
        assert!(body.control().is_none() && body.tail_frames() == 0);
    }

    let binaural = prepare(&objects, Kind::Binaural, b"binaural", false);
    let (_, _, info) = opened(&binaural, &s);
    assert_eq!(
        info,
        Info {
            outputs: 2,
            sample_rate: 48000,
            binaural: true,
            requires_preroll: true
        }
    );

    // A missing input is located at its path.
    let absent = format!("{}.absent", objects.path);
    let e = open(&hoa, &settings(&objects.scene, &absent))
        .err()
        .unwrap();
    assert_eq!(e.location, mradm_render::offline::Location::Path);
}

#[test]
fn seek_cancellation_points_follow_the_cpp_shells() {
    let objects = input("stream-cancel", "objects-room", 48000);
    let s = settings(&objects.scene, &objects.path);
    let target = 4800;
    // Every case seeks a freshly opened stream: Triple Balance restores later seeks from its
    // checkpoints with fewer steps.
    let fresh = |p: &Prepared| opened(p, &s).1;
    let cancel = |p: &Prepared, at: usize| cancel_message(&mut fresh(p), target, at);

    // Loudspeaker: the IRenderStream default checks once, before a one-step seek.
    let vbap = prepare(&objects, Kind::Vbap, b"4+7+0", false);
    assert_eq!(seek(&mut fresh(&vbap), target, None).unwrap(), 0);
    assert_eq!(
        cancel(&vbap, 0),
        Some((5, "stream seek cancelled".to_owned()))
    );

    // Triple Balance: before the begin and before every step; the finish follows the last step.
    let tb = prepare(&objects, Kind::TripleBalance, b"4+7+0", false);
    let steps = seek(&mut fresh(&tb), target, None).unwrap();
    assert!(steps > 0, "size tracks render forward from a checkpoint");
    for at in 0..=steps {
        assert_eq!(
            cancel(&tb, at),
            Some((5, "Triple Balance seek cancelled".to_owned())),
            "call {at}"
        );
    }
    assert_eq!(cancel(&tb, steps + 1), None);

    // Room: only between steps; a cancelled begin still seeks.
    let room = prepare(&objects, Kind::Room, b"binaural", false);
    let steps = seek(&mut fresh(&room), target, None).unwrap();
    assert!(steps > 0);
    assert_eq!(cancel(&room, 0), None);
    for at in 1..=steps {
        assert_eq!(
            cancel(&room, at),
            Some((5, "room-binaural 预热已取消".to_owned()))
        );
    }

    // Binaural: before the begin, before every step and once more after the last one.
    let binaural = prepare(&objects, Kind::Binaural, b"binaural", false);
    let calls = seek(&mut fresh(&binaural), target, None).unwrap();
    assert!(calls > 1, "pre-roll steps and the final check");
    for at in 0..=calls {
        assert_eq!(
            cancel(&binaural, at),
            Some((5, "双耳文件流式跳转已取消".to_owned())),
            "call {at}"
        );
    }
    assert_eq!(
        seek(&mut fresh(&binaural), 0, None).unwrap(),
        1,
        "begin, then the final check"
    );
}

/// An abandoned seek leaves the next one exact, for every backend.
#[test]
fn seeks_after_cancellation_match_fresh_streams() {
    let objects = input("stream-resume", "objects-room", 48000);
    let s = settings(&objects.scene, &objects.path);
    for (kind, layout) in [
        (Kind::Vbap, &b"4+7+0"[..]),
        (Kind::Hoa, b"hoa3"),
        (Kind::Ear, b"0+5+0"),
        (Kind::TripleBalance, b"4+7+0"),
        (Kind::Binaural, b"binaural"),
        (Kind::Room, b"binaural"),
    ] {
        let p = prepare(&objects, kind, layout, false);
        let render = |body: &mut Body, outputs: usize| {
            seek(body, 2500, None).unwrap();
            let mut out = vec![0.; 700 * outputs];
            assert_eq!(body.process(&mut out, 700).unwrap(), 700);
            out.into_iter().map(f32::to_bits).collect::<Vec<_>>()
        };
        let (_, mut fresh, info) = opened(&p, &s);
        let expected = render(&mut fresh, info.outputs);
        let (_, mut body, _) = opened(&p, &s);
        let _ = seek(&mut body, 4800, Some(1));
        assert_eq!(render(&mut body, info.outputs), expected, "{kind:?}");
    }
}

#[test]
fn validation_and_edits_by_backend() {
    let objects = input("stream-edits", "objects-room", 48000);
    let s = settings(&objects.scene, &objects.path);
    let id = b"AO_1001".to_vec();
    let edit = |height: f32| LiveOverride {
        base: Override {
            object_id: &id,
            speaker_label: b"",
            mute: false,
            gain_db: -6.,
            head_locked: None,
        },
        extent_scale: 0.5,
        extent_width_scale: 1.,
        extent_height_scale: height,
        extent_depth_scale: 1.,
        diffuse_scale: 1.,
        divergence_scale: 1.,
    };
    let tb = prepare(&objects, Kind::TripleBalance, b"4+7+0", false);
    let (validator, _, _) = opened(&tb, &s);
    validator.validate(&[edit(1.)]).unwrap();
    let (code, message, context) = validator.validate(&[edit(2.)]).unwrap_err();
    assert_eq!(
        (code, message.as_str(), context.as_slice()),
        (
            2,
            "Triple Balance live editing supports gain, mute and linked isotropic size only",
            id.as_slice()
        )
    );
    let room = prepare(&objects, Kind::Room, b"binaural", false);
    let (validator, mut body, _) = opened(&room, &s);
    assert_eq!(
        validator.validate(&[edit(-1.)]).unwrap_err(),
        (1, "房间 extent 比例必须有限且非负".to_owned(), Vec::new())
    );
    body.set_overrides(&[edit(1.)]).unwrap();
    for (kind, layout) in [
        (Kind::Vbap, &b"4+7+0"[..]),
        (Kind::Hoa, b"hoa3"),
        (Kind::Ear, b"0+5+0"),
        (Kind::Binaural, b"binaural"),
    ] {
        let p = prepare(&objects, kind, layout, false);
        let (validator, mut body, info) = opened(&p, &s);
        validator.validate(&[edit(-1.)]).unwrap();
        body.set_overrides(&[edit(1.)]).unwrap();
        body.set_pose([30., 0., 0.]);
        let mut out = vec![0.; 64 * info.outputs];
        assert_eq!(body.process(&mut out, 64).unwrap(), 64, "{kind:?}");
    }
}
