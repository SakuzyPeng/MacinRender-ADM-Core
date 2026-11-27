//! The room runtime, file stream and Scene stream renderer (PCM consistency 4.6 E): the switch
//! request rules, stale candidates after a reset, the end-of-programme guard, the HRIR path
//! comparison, the preparation thread's join, stepped seeks and the Scene generation checks and
//! warnings moved from the C++ `room_internal::Runtime`, `RoomStream` and `LiveRoomRenderer`.
mod common;

use common::{ObjectSpec, TrackSpec, object_block};
use mradm_dsp::room::loading::{self, Package};
use mradm_dsp::room::quality::Quality;
use mradm_dsp::room::screen::RoomScreens;
use mradm_render::live::{Element, Entry, Frame, Plane, Role, State, Update, code, field};
use mradm_render::room::{self, Prepared, Request, Routing, Screens, Spread};
use mradm_render::room_runtime::{Config, Control, Fault, Prepare, Resources, state};
use mradm_render::room_stream::{FRONT_WARNING, FileStream, Live, SCREEN_WARNING};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

const FRAMES: u64 = 24000;

fn manifest() -> String {
    format!(
        "{}/../../../tests/fixtures/room/synthetic/room.json",
        env!("CARGO_MANIFEST_DIR")
    )
}

fn package() -> Arc<Package> {
    Arc::new(loading::load_package(manifest().as_bytes()).unwrap())
}

fn fault(e: mradm_dsp::Error) -> Fault {
    match e {
        mradm_dsp::Error::InvalidArgument(s) => (1, s),
        mradm_dsp::Error::Unsupported(s) => (2, s),
        mradm_dsp::Error::Io(s) => (3, s),
        mradm_dsp::Error::RenderFailed(s) => (4, s),
    }
}

/// The boundary without the floating-point environment: the tests run in the default one.
fn scope(call: &mut dyn FnMut() -> mradm_dsp::Result<()>) -> Result<(), Fault> {
    call().map_err(fault)
}

fn config(package: &Arc<Package>) -> Config {
    Config {
        package: Arc::clone(package),
        hrir: String::new(),
        metres: 1.,
        quality: Quality::Exact,
    }
}

fn resources(package: &Package) -> Arc<Resources> {
    Arc::new(Resources {
        set: loading::prepare(package, b"", b"", None, Quality::Exact).unwrap(),
        cuda: false,
    })
}

/// A switch preparation that waits for the test to open it and records how often it started.
#[derive(Default)]
struct Gate {
    state: Mutex<(u32, bool)>,
    cv: Condvar,
}

impl Gate {
    fn prepare(self: &Arc<Self>) -> Prepare {
        let gate = Arc::clone(self);
        Arc::new(move |config: &Config, reuse: &Resources| {
            {
                let mut s = gate.state.lock().unwrap();
                s.0 += 1;
                gate.cv.notify_all();
                let _open = gate.cv.wait_while(s, |s| !s.1).unwrap();
            }
            let set = loading::prepare(
                &config.package,
                config.hrir.as_bytes(),
                b"",
                Some(&reuse.set),
                config.quality,
            )
            .map_err(fault)?;
            Ok(Resources { set, cuda: false })
        })
    }
    fn open(&self) {
        self.state.lock().unwrap().1 = true;
        self.cv.notify_all();
    }
    fn wait_started(&self, count: u32) {
        let s = self.state.lock().unwrap();
        let _started = self.cv.wait_while(s, |s| s.0 < count).unwrap();
    }
}

fn open_gate() -> Arc<Gate> {
    let gate = Arc::new(Gate::default());
    gate.open();
    gate
}

fn scene() -> mradm_scene::RenderScene {
    common::scene(
        vec![ObjectSpec {
            id: "AO_1001",
            gain: 1.,
            end: u64::MAX,
            tracks: vec![TrackSpec {
                blocks: vec![object_block(0, 9000, 30.), object_block(9000, FRAMES, -60.)],
                direct: Vec::new(),
            }],
        }],
        FRAMES,
    )
}

fn table(scene: &mradm_scene::RenderScene) -> Arc<Prepared> {
    let request = Request {
        scene,
        sample_rate: 48000,
        layout_id: b"binaural",
        spread: Spread::Automatic,
        routing: Routing::Automatic,
        has_matrix: false,
        default_interp_ms: 5,
        screens: Screens::default(),
    };
    Arc::new(room::prepare(&request, &mut |_, _| {}, loading::polar).unwrap())
}

struct Fixture {
    package: Arc<Package>,
    resources: Arc<Resources>,
    table: Arc<Prepared>,
    input: common::Temp,
}

impl Fixture {
    fn new() -> Self {
        Self::with_package(package())
    }
    fn zoned() -> Self {
        let path = manifest().replace("/room.json", "/zoned-room.json");
        Self::with_package(Arc::new(loading::load_package(path.as_bytes()).unwrap()))
    }
    fn with_package(package: Arc<Package>) -> Self {
        Self {
            resources: resources(&package),
            package,
            table: table(&scene()),
            input: common::input(&common::sine(FRAMES as usize, 0.5)),
        }
    }
    fn stream_with(&self, config: Config, prepare: Prepare) -> FileStream {
        FileStream::open(
            self.input.path(),
            Arc::clone(&self.table),
            Arc::clone(&self.resources),
            config,
            [0.; 3],
            scope,
            prepare,
        )
        .unwrap()
    }
    fn stream(&self, prepare: Prepare) -> FileStream {
        self.stream_with(config(&self.package), prepare)
    }
}

fn pull(stream: &mut FileStream, frames: usize) -> Vec<f32> {
    let mut out = vec![0.; frames * 2];
    let mut done = 0;
    while done < frames {
        let n = stream.process(&mut out[done * 2..], frames - done).unwrap();
        assert_ne!(n, 0, "the stream ended early");
        done += n;
    }
    out
}

fn seek(stream: &mut FileStream, frame: u64) {
    if stream.seek_begin(frame).unwrap() {
        while stream.seek_step().unwrap() {}
    }
}

/// Pulls blocks until `done` holds for the control's status (the preparation thread runs freely).
fn pull_until(
    stream: &mut FileStream,
    control: &Control,
    done: impl Fn(&mradm_render::room_runtime::Status) -> bool,
) {
    let deadline = Instant::now() + Duration::from_secs(60);
    while !done(&control.status()) {
        assert!(Instant::now() < deadline, "status {:?}", control.status());
        seek_or_pull(stream);
        std::thread::sleep(Duration::from_millis(1));
    }
}

/// Keeps the stream inside its material: rewinds before the programme ends.
fn seek_or_pull(stream: &mut FileStream) {
    let mut out = vec![0.; 1024];
    if stream.process(&mut out, 512).unwrap() < 512 {
        seek(stream, 0);
    }
}

fn message(e: &mradm_render::routing::Error) -> (i32, String) {
    (e.code, String::from_utf8_lossy(&e.message).into_owned())
}

#[test]
fn a_switch_applies_and_a_reset_makes_a_preparing_candidate_stale() {
    let fixture = Fixture::new();
    let gate = Arc::new(Gate::default());
    let mut stream = fixture.stream(gate.prepare());
    let control = stream.control();
    pull(&mut stream, 4096);
    assert_eq!(control.request(&config(&fixture.package)).unwrap(), 1);
    let status = control.status();
    assert_eq!(
        (
            status.state,
            status.requested_revision,
            status.applied_revision
        ),
        (state::PREPARING, 1, 0)
    );
    gate.wait_started(1);
    // The reset bumps the epoch while the candidate is being prepared.
    seek(&mut stream, 0);
    assert_eq!(control.status().state, state::APPLIED);
    gate.open();
    for _ in 0..40 {
        pull(&mut stream, 512);
        std::thread::sleep(Duration::from_millis(2));
    }
    let status = control.status();
    assert_eq!(
        (
            status.state,
            status.applied_revision,
            status.message.as_str()
        ),
        (state::APPLIED, 0, ""),
        "a candidate of an older epoch never applies"
    );
    assert_eq!(control.request(&config(&fixture.package)).unwrap(), 2);
    pull_until(&mut stream, &control, |s| {
        s.applied_revision == 2 && s.state == state::APPLIED
    });
    assert_eq!(control.status().requested_revision, 2);
}

#[test]
fn requests_keep_the_quality_scale_and_hrir_and_stop_at_the_end() {
    let fixture = Fixture::new();
    let dir = manifest().trim_end_matches("room.json").to_owned();
    let base = Config {
        hrir: format!("{dir}hrir.json"),
        ..config(&fixture.package)
    };
    let mut stream = fixture.stream_with(base.clone(), open_gate().prepare());
    let control = stream.control();
    let switch = (1, "房间切换必须保持当前档位、HRIR 和坐标比例".to_owned());
    for (hrir, accepted) in [
        (format!("{dir}hrir.json"), true),
        (format!("{dir}/hrir.json"), true),
        (String::new(), true),
        (format!("{dir}./hrir.json"), false),
        (format!("{dir}hrir.json/"), false),
        (format!("{dir}other.json"), false),
    ] {
        let result = control.request(&Config {
            hrir: hrir.clone(),
            ..base.clone()
        });
        if accepted {
            assert!(result.is_ok(), "{hrir}");
        } else {
            assert_eq!(message(&result.unwrap_err()), switch, "{hrir}");
        }
    }
    for changed in [
        Config {
            quality: Quality::Realtime,
            ..base.clone()
        },
        Config {
            metres: 2.,
            ..base.clone()
        },
    ] {
        assert_eq!(message(&control.request(&changed).unwrap_err()), switch);
    }
    // Through the material: the input ends and the tail starts.
    let mut out = vec![0.; 4096];
    let mut position = 0;
    while position <= FRAMES {
        position += stream.process(&mut out, 2048).unwrap() as u64;
    }
    assert_eq!(
        message(&control.request(&base).unwrap_err()),
        (1, "节目已结束，不能再切换房间".to_owned())
    );
    // A seek reopens the programme.
    seek(&mut stream, 0);
    assert!(control.request(&base).is_ok());
}

#[test]
fn dropping_the_stream_joins_the_preparer_and_the_control_outlives_it() {
    let fixture = Fixture::new();
    let gate = Arc::new(Gate::default());
    let prepare = gate.prepare();
    let marker = Arc::new(());
    let held = Arc::clone(&marker);
    let tracked: Prepare = Arc::new(move |c: &Config, r: &Resources| {
        let _held = &held;
        prepare(c, r)
    });
    let mut stream = fixture.stream(tracked);
    let control = stream.control();
    pull(&mut stream, 2048);
    control.request(&config(&fixture.package)).unwrap();
    gate.wait_started(1);
    let opener = {
        let gate = Arc::clone(&gate);
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(50));
            gate.open();
        })
    };
    drop(stream);
    assert_eq!(
        Arc::strong_count(&marker),
        1,
        "the preparation thread finished before the stream was released"
    );
    opener.join().unwrap();
    assert_eq!(control.status().requested_revision, 1);
    assert_eq!(control.source_position(7).element_id, 7);
}

#[test]
fn an_abandoned_stepped_seek_leaves_the_next_seek_exact() {
    check_abandoned_seek(Fixture::new());
    check_abandoned_seek(Fixture::zoned());
}

fn check_abandoned_seek(fixture: Fixture) {
    let mut reference = fixture.stream(open_gate().prepare());
    let expected = pull(&mut reference, FRAMES as usize);
    let mut stream = fixture.stream(open_gate().prepare());
    assert!(stream.seek_begin(20111).unwrap());
    assert!(stream.seek_step().unwrap());
    for target in [13007u64, 0, 2048, 9001] {
        seek(&mut stream, target);
        let count = (FRAMES - target).min(3001) as usize;
        let actual = pull(&mut stream, count);
        let start = target as usize * 2;
        assert!(
            actual
                .iter()
                .zip(&expected[start..])
                .all(|(a, b)| a.to_bits() == b.to_bits()),
            "seek {target}"
        );
    }
    let tail = stream.tail_frames();
    let error = stream.seek_begin(FRAMES + tail + 1).unwrap_err();
    assert_eq!(
        (error.code, error.message.as_str()),
        (1, "room-binaural seek 超过节目及尾声长度")
    );
    let mut small = [0.; 3];
    let error = stream.process(&mut small, 2).unwrap_err();
    assert_eq!(
        (error.code, error.message.as_str()),
        (1, "room-binaural 输出缓冲区不足")
    );
}

fn element(id: u64, role: Role, label: &'static str) -> Element<'static> {
    Element {
        id,
        role: Some(role),
        speaker_label: label.as_bytes(),
        object_id: b"",
        has_position: false,
        position: [0.25, 0.5, 0.],
    }
}

fn live(fixture: &Fixture, sink: bool) -> Live {
    Live::new(
        Arc::clone(&fixture.resources),
        config(&fixture.package),
        true,
        RoomScreens::default(),
        sink,
        scope,
        open_gate().prepare(),
    )
    .unwrap()
}

fn object_state() -> State {
    State {
        valid_fields: field::POSITION
            | field::ACTIVE
            | field::LINEAR_GAIN
            | field::SCREEN_REFERENCE,
        active: true,
        linear_gain: 1.,
        position: [0., 1., 0.],
        screen_reference: true,
        ..State::default()
    }
}

fn render(
    r: &mut Live,
    generation: u64,
    ids: &[u64],
    initial: &[Entry],
    updates: &[Update],
) -> Result<Vec<f32>, mradm_render::routing::Error> {
    let ones = vec![0.5f32; 256];
    let planes: Vec<Plane> = ids
        .iter()
        .map(|&element_id| Plane {
            element_id,
            has_signal: true,
            samples: &ones,
        })
        .collect();
    let mut output = vec![0.; 512];
    r.render(
        &Frame {
            generation,
            duration: 256,
            planes: &planes,
            initial,
            updates,
            trace: b"",
        },
        &mut output,
    )?;
    Ok(output)
}

#[test]
fn scene_generations_check_their_sources_and_warn_as_cpp_did() {
    check_scene_generations(Fixture::new());
    check_scene_generations(Fixture::zoned());
}

fn check_scene_generations(fixture: Fixture) {
    let mut r = live(&fixture, true);
    let elements = [
        element(1, Role::Object, ""),
        element(2, Role::DirectSpeaker, "M+030"),
        element(3, Role::DirectSpeaker, "X-unknown"),
    ];
    r.configure(5, &elements).unwrap();
    let warnings: Vec<_> = r
        .diagnostics()
        .iter()
        .map(|d| (d.code, d.element_id, d.field, d.message))
        .collect();
    assert_eq!(
        warnings,
        [(
            code::DIRECT_SPEAKER_FALLBACK,
            3,
            field::POSITION,
            FRONT_WARNING
        )]
    );
    let initial = [Entry {
        element_id: 1,
        state: object_state(),
    }];
    let text = |e: mradm_render::routing::Error| message(&e).1;
    assert_eq!(
        text(render(&mut r, 4, &[1], &[], &[]).unwrap_err()),
        "room-binaural generation 不匹配"
    );
    assert_eq!(
        text(render(&mut r, 5, &[9], &[], &[]).unwrap_err()),
        "room-binaural 收到未知声源 PCM"
    );
    let unknown = [Entry {
        element_id: 9,
        state: object_state(),
    }];
    assert_eq!(
        text(render(&mut r, 5, &[1], &unknown, &[]).unwrap_err()),
        "room-binaural 收到未知声源状态"
    );
    // The screenRef warning is raised once per renderer, before the frame's source checks.
    assert_eq!(
        r.diagnostics()
            .iter()
            .map(|d| (d.code, d.element_id, d.field, d.message))
            .collect::<Vec<_>>(),
        [(
            code::SEMANTIC_DEGRADED,
            9,
            field::SCREEN_REFERENCE,
            SCREEN_WARNING
        )]
    );
    let update = Update {
        element_id: 9,
        offset: 0,
        ramp: 0,
        jump: false,
        changed: field::LINEAR_GAIN,
        cleared: 0,
        state: object_state(),
    };
    assert_eq!(
        text(render(&mut r, 5, &[1], &[], &[update]).unwrap_err()),
        "room-binaural 收到未知声源控制"
    );
    assert!(r.diagnostics().is_empty());
    render(&mut r, 5, &[1, 2, 3], &initial, &[]).unwrap();
    assert!(r.diagnostics().is_empty());
    r.reset();
    r.configure(6, &elements[..1]).unwrap();
    assert!(r.diagnostics().is_empty());
    render(&mut r, 6, &[1], &initial, &[]).unwrap();
    assert!(
        r.diagnostics().is_empty(),
        "no second screenRef warning after a reset"
    );
    // After the end of input, frames carry no sources to check.
    assert_eq!(r.end_input().unwrap(), r.tail_frames());
    render(&mut r, 6, &[9], &unknown, &[update]).unwrap();
}

#[test]
fn without_a_sink_an_unknown_label_keeps_its_position_silently() {
    let fixture = Fixture::new();
    let elements = [element(1, Role::DirectSpeaker, "X-unknown")];
    let initial = [Entry {
        element_id: 1,
        state: State {
            valid_fields: field::ACTIVE | field::LINEAR_GAIN,
            active: true,
            linear_gain: 1.,
            ..State::default()
        },
    }];
    let mut outputs = Vec::new();
    for sink in [true, false] {
        let mut r = live(&fixture, sink);
        r.configure(1, &elements).unwrap();
        assert_eq!(r.diagnostics().len(), usize::from(sink));
        let mut pcm = render(&mut r, 1, &[1], &initial, &[]).unwrap();
        for _ in 0..8 {
            pcm.extend(render(&mut r, 1, &[1], &[], &[]).unwrap());
        }
        outputs.push(pcm);
    }
    assert_ne!(
        outputs[0], outputs[1],
        "the element's own position, not the front"
    );
}

/// The switch rules of the C++ `room_render_test` (R3 C2): a failed candidate keeps the current
/// room, the latest request wins, asking again for the incoming or the current room does not
/// restart its transition, and a failed newest request does not revive a superseded pending room.
#[test]
fn failed_candidates_and_repeated_requests_keep_the_transition() {
    let fixture = Fixture::new();
    let dir = std::env::temp_dir().join(format!("mradm-room-variants-{}", std::process::id()));
    let second = variant(
        &dir,
        "second.json",
        &[
            (
                r#""dimensions": [7, 6, 3.2]"#,
                r#""dimensions": [8, 7, 3.6]"#,
            ),
            (r#""id": "synthetic""#, r#""id": "second""#),
        ],
    );
    let incompatible = variant(
        &dir,
        "incompatible.json",
        &[
            (
                r#""alignment_guard_samples": 40"#,
                r#""alignment_guard_samples": 41"#,
            ),
            (r#""id": "synthetic""#, r#""id": "incompatible-guard""#),
        ],
    );
    let (current, other, bad) = (
        config(&fixture.package),
        config(&second),
        config(&incompatible),
    );
    // The preparation rejects the incompatible alignment guard.
    let mut renderer = live(&fixture, false);
    let mut placed = element(7, Role::Object, "");
    placed.has_position = true;
    renderer.configure(1, &[placed]).unwrap();
    let initial = [Entry {
        element_id: 7,
        state: State {
            valid_fields: field::KNOWN,
            ..State::default()
        },
    }];
    render(&mut renderer, 1, &[7], &initial, &[]).unwrap();
    let control = renderer.control();
    let samples = [0.5f32; 256];
    let planes = [Plane {
        element_id: 7,
        has_signal: true,
        samples: &samples,
    }];
    let mut out = [0f32; 512];
    let mut wait = |done: &dyn Fn(&mradm_render::room_runtime::Status) -> bool, advance: bool| {
        for _ in 0..2000 {
            renderer
                .render(
                    &Frame {
                        generation: 1,
                        duration: if advance { 256 } else { 0 },
                        planes: &planes,
                        initial: &[],
                        updates: &[],
                        trace: b"",
                    },
                    &mut out,
                )
                .unwrap();
            assert!(out.iter().all(|sample| sample.is_finite()));
            if done(&control.status()) {
                return;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        panic!("status {:?}", control.status());
    };

    let failed = control.request(&bad).unwrap();
    wait(
        &|s| s.state == state::FAILED && s.requested_revision == failed,
        true,
    );
    assert_eq!(
        control.status().applied_revision,
        0,
        "a failed candidate keeps the current room"
    );
    control.request(&other).unwrap();
    let latest = control.request(&current).unwrap();
    wait(
        &|s| s.state == state::APPLIED && s.applied_revision == latest,
        true,
    );

    let change = control.request(&other).unwrap();
    wait(
        &|s| s.state == state::TRANSITIONING && s.requested_revision == change,
        true,
    );
    let sample = control.status().transition_sample;
    control.request(&current).unwrap();
    let last = control.request(&other).unwrap();
    wait(
        &|s| s.state == state::APPLIED && s.applied_revision == last,
        true,
    );
    assert_eq!(
        control.status().transition_sample,
        sample,
        "the incoming room does not restart"
    );
    let same = control.request(&other).unwrap();
    wait(
        &|s| s.state == state::APPLIED && s.applied_revision == same,
        true,
    );
    assert_eq!(
        control.status().transition_sample,
        sample,
        "the current room does not restart"
    );

    let retained = control.request(&current).unwrap();
    wait(
        &|s| s.state == state::TRANSITIONING && s.requested_revision == retained,
        true,
    );
    let pending = control.request(&other).unwrap();
    // Pump the candidate into the session's pending slot without finishing the active transition.
    wait(
        &|s| s.state == state::TRANSITIONING && s.requested_revision == pending,
        false,
    );
    let newest = control.request(&bad).unwrap();
    wait(
        &|s| s.state == state::FAILED && s.requested_revision == newest,
        false,
    );
    wait(&|s| s.applied_revision == retained, true);
    for _ in 0..100 {
        wait(&|_| true, true);
    }
    assert_eq!(
        control.status().applied_revision,
        retained,
        "the superseded room stays dropped"
    );
    let restored = control.request(&other).unwrap();
    wait(
        &|s| s.state == state::APPLIED && s.applied_revision == restored,
        true,
    );
    drop(renderer);
    let _ = std::fs::remove_dir_all(&dir);
}

/// A copy of the synthetic room in `dir` with `edits` applied to its manifest text (the C++
/// fixture's `second.json`: other dimensions; `incompatible.json`: another alignment guard).
fn variant(dir: &std::path::Path, name: &str, edits: &[(&str, &str)]) -> Arc<Package> {
    let source = std::path::PathBuf::from(manifest());
    let source = source.parent().unwrap();
    std::fs::create_dir_all(dir).unwrap();
    for entry in std::fs::read_dir(source).unwrap() {
        let entry = entry.unwrap();
        std::fs::copy(entry.path(), dir.join(entry.file_name())).unwrap();
    }
    let mut text = std::fs::read_to_string(source.join("room.json")).unwrap();
    for (from, to) in edits {
        assert!(text.contains(from), "{from}");
        text = text.replace(from, to);
    }
    let path = dir.join(name);
    std::fs::write(&path, text).unwrap();
    Arc::new(loading::load_package(path.to_str().unwrap().as_bytes()).unwrap())
}

/// Source growth during a switch (formerly part of the C++ `room_render_test`): a candidate
/// prepared for fewer sources is resized when a later generation adds one, before the zero-source
/// session's first frame and while a room is being prepared, without failing the switch.
#[test]
fn source_growth_resizes_the_candidate_without_failing_the_switch() {
    let fixture = Fixture::new();
    let dir = std::env::temp_dir().join(format!("mradm-room-growth-{}", std::process::id()));
    let second = variant(
        &dir,
        "second.json",
        &[
            (
                r#""dimensions": [7, 6, 3.2]"#,
                r#""dimensions": [8, 7, 3.6]"#,
            ),
            (r#""id": "synthetic""#, r#""id": "second""#),
        ],
    );
    let mut r = live(&fixture, false);
    let control = r.control();
    let mut elements = Vec::new();
    for generation in 1..=2u64 {
        let target = if generation == 1 {
            config(&second)
        } else {
            config(&fixture.package)
        };
        let request = control.request(&target).unwrap();
        std::thread::sleep(Duration::from_millis(100));
        let mut e = element(generation, Role::Object, "");
        e.has_position = true;
        elements.push(e);
        r.configure(generation, &elements).unwrap();
        let ids: Vec<u64> = (1..=generation).collect();
        for _ in 0..2000 {
            if control.status().applied_revision == request {
                break;
            }
            render(&mut r, generation, &ids, &[], &[]).unwrap();
            assert_ne!(
                control.status().state,
                state::FAILED,
                "{:?}",
                control.status()
            );
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(control.status().applied_revision, request);
    }
    drop(r);
    let _ = std::fs::remove_dir_all(&dir);
}

/// An epoch reset keeps the applied room and clears the history (formerly part of the C++
/// `room_render_test`): after switching to another room, a reset renderer equals a fresh one
/// opened on that room.
#[test]
fn a_reset_keeps_the_applied_room_and_clears_history() {
    let fixture = Fixture::new();
    let dir = std::env::temp_dir().join(format!("mradm-room-reset-{}", std::process::id()));
    let second = variant(
        &dir,
        "second.json",
        &[
            (
                r#""dimensions": [7, 6, 3.2]"#,
                r#""dimensions": [8, 7, 3.6]"#,
            ),
            (r#""id": "synthetic""#, r#""id": "second""#),
        ],
    );
    let mut placed = element(7, Role::Object, "");
    placed.has_position = true;
    let initial = [Entry {
        element_id: 7,
        state: State {
            valid_fields: field::KNOWN,
            ..State::default()
        },
    }];
    let mut r = live(&fixture, false);
    r.configure(1, &[placed]).unwrap();
    render(&mut r, 1, &[7], &initial, &[]).unwrap();
    let control = r.control();
    let request = control.request(&config(&second)).unwrap();
    for _ in 0..2000 {
        let s = control.status();
        if s.state == state::APPLIED && s.applied_revision == request {
            break;
        }
        render(&mut r, 1, &[7], &[], &[]).unwrap();
        std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(control.status().applied_revision, request);
    r.reset();
    r.configure(2, &[placed]).unwrap();
    let mut fresh = Live::new(
        resources(&second),
        config(&second),
        true,
        RoomScreens::default(),
        false,
        scope,
        open_gate().prepare(),
    )
    .unwrap();
    fresh.configure(2, &[placed]).unwrap();
    for i in 0..20 {
        let states: &[Entry] = if i == 0 { &initial } else { &[] };
        assert_eq!(
            render(&mut r, 2, &[7], states, &[]).unwrap(),
            render(&mut fresh, 2, &[7], states, &[]).unwrap(),
            "frame {i}"
        );
    }
    drop((r, fresh));
    let _ = std::fs::remove_dir_all(&dir);
}

fn speaker_package() -> Arc<Package> {
    let path = manifest().replace("/room.json", "/speaker-room.json");
    Arc::new(loading::load_package(path.as_bytes()).unwrap())
}

fn speaker_file(input: &common::Temp, package: &Arc<Package>, layout: &str) -> FileStream {
    let scene = mradm_scene::import::import(
        input.path().as_bytes(),
        mradm_scene::import::ImportOptions {
            input_layout: Some(layout),
            input_channel_labels: &[],
        },
    )
    .unwrap();
    FileStream::open(
        input.path(),
        table(&scene),
        resources(package),
        Config {
            metres: 0.,
            ..config(package)
        },
        [0.; 3],
        scope,
        open_gate().prepare(),
    )
    .unwrap()
}

fn near(a: &[f32], b: &[f32]) {
    assert_eq!(a.len(), b.len());
    let peak = b.iter().copied().map(f32::abs).fold(0., f32::max);
    let err = a
        .iter()
        .zip(b)
        .map(|(x, y)| (x - y).abs())
        .fold(0., f32::max);
    assert!(
        peak > 0. && err < peak * 1e-5,
        "peak {peak}, difference {err}"
    );
}

#[test]
fn fixed_speaker_calibration_matches_electrical_feeds_in_file_and_scene_streams() {
    const N: usize = 6103;
    for layout in ["7.1.4", "9.1.6", "22.2"] {
        let preset = mradm_scene::input::find_input_layout(layout).unwrap();
        let apple =
            mradm_scene::layouts::find(preset.internal_id, mradm_scene::layouts::Geometry::Apple)
                .unwrap();
        let ch = preset.channels.len();
        let cal = speaker_package();
        let mut x = vec![0.; N * ch];
        let mut reference = vec![0.; (N + 512) * ch];
        let mut extra = 0;
        for (i, c) in preset.channels.iter().enumerate() {
            let delay = (i * 37 + 5) % 480;
            let gain = if i % 2 == 0 { 0.5 } else { 1. };
            extra = extra.max(delay + if c.is_lfe { 3 } else { 0 });
            for (at, sample) in [(0, 0.063), (256 - i, 0.31), (2680 + i, -0.2), (N - 1, 0.08)] {
                let v = sample * (1. + i as f32 / 32.);
                x[at * ch + i] = v;
                reference[(at + delay) * ch + i] += v * gain;
                if c.is_lfe {
                    reference[(at + delay + 3) * ch + i] += v * gain * 0.05;
                }
            }
        }
        reference.truncate((N + extra) * ch);
        let src = common::input_channels(&x, ch as u16);
        let ref_src = common::input_channels(&reference, ch as u16);
        let mut file = speaker_file(&src, &cal, layout);
        let mut uncalibrated = speaker_file(&ref_src, &package(), layout);
        let tail = file.tail_frames() as usize;
        assert_eq!(tail, uncalibrated.tail_frames() as usize + extra);
        let want = pull(&mut uncalibrated, N + tail);
        let actual = pull(&mut file, N + tail);
        near(&actual, &want);
        for at in [79, N - 4, N + 31] {
            seek(&mut file, at as u64);
            assert_eq!(pull(&mut file, 113), actual[at * 2..(at + 113) * 2]);
        }
        // The Scene host can enumerate the bus in any order and use the Apple
        // speaker geometry. A calibrated room maps those buses to its measured array.
        let elements: Vec<_> = preset
            .channels
            .iter()
            .enumerate()
            .rev()
            .map(|(i, c)| {
                let a = apple
                    .speakers
                    .iter()
                    .find(|a| a.label == c.speaker_label)
                    .unwrap();
                Element {
                    has_position: true,
                    position: loading::polar(a.azimuth, a.elevation, 1.).unwrap(),
                    ..element(
                        i as u64 + 1,
                        if c.is_lfe {
                            Role::Lfe
                        } else {
                            Role::DirectSpeaker
                        },
                        c.speaker_label,
                    )
                }
            })
            .collect();
        let mut live = Live::new(
            resources(&cal),
            Config {
                metres: 0.,
                ..config(&cal)
            },
            false,
            RoomScreens::default(),
            true,
            scope,
            open_gate().prepare(),
        )
        .unwrap();
        live.configure(17, &elements).unwrap();
        let planar: Vec<Vec<f32>> = (0..ch)
            .map(|i| x.iter().skip(i).step_by(ch).copied().collect())
            .collect();
        let mut stream = Vec::new();
        let mut at = 0;
        while at < N + tail {
            if at == N {
                assert_eq!(live.end_input().unwrap() as usize, tail);
            }
            let n = 181.min(if at < N { N - at } else { N + tail - at });
            let planes: Vec<_> = if at < N {
                (0..ch)
                    .map(|i| Plane {
                        element_id: i as u64 + 1,
                        has_signal: true,
                        samples: &planar[i][at..at + n],
                    })
                    .collect()
            } else {
                Vec::new()
            };
            let mut out = vec![0.; 2 * n];
            live.render(
                &Frame {
                    generation: 17,
                    duration: n as u32,
                    planes: &planes,
                    initial: &[],
                    updates: &[],
                    trace: b"",
                },
                &mut out,
            )
            .unwrap();
            stream.extend(out);
            at += n;
        }
        near(&stream, &want);
    }
}

#[test]
fn fixed_speaker_calibration_bypasses_objects_and_explicit_distance_scale() {
    let cal = speaker_package();
    let uncal = package();
    let fixture = common::input(&common::sine(3600, 0.25));
    let scene = common::scene(
        vec![ObjectSpec {
            id: "AO_1001",
            gain: 1.,
            end: 3600,
            tracks: vec![TrackSpec {
                blocks: vec![object_block(0, 3600, 65.)],
                direct: vec![],
            }],
        }],
        3600,
    );
    let open = |pack: &Arc<Package>, input: &common::Temp, t: Arc<Prepared>, metres| {
        FileStream::open(
            input.path(),
            t,
            resources(pack),
            Config {
                metres,
                ..config(pack)
            },
            [0.; 3],
            scope,
            open_gate().prepare(),
        )
        .unwrap()
    };
    let mut a = open(&cal, &fixture, table(&scene), 0.);
    let mut b = open(&uncal, &fixture, table(&scene), 0.);
    assert_eq!(a.tail_frames(), b.tail_frames());
    let n = 3600 + a.tail_frames() as usize;
    assert_eq!(pull(&mut a, n), pull(&mut b, n));
    let x = common::input_channels(&common::sine(12 * 3200, 0.1), 12);
    let scene = mradm_scene::import::import(
        x.path().as_bytes(),
        mradm_scene::import::ImportOptions {
            input_layout: Some("7.1.4"),
            input_channel_labels: &[],
        },
    )
    .unwrap();
    let mut a = open(&cal, &x, table(&scene), 1.5);
    let mut b = open(&uncal, &x, table(&scene), 1.5);
    assert_eq!(a.tail_frames(), b.tail_frames());
    let n = 3200 + a.tail_frames() as usize;
    assert_eq!(pull(&mut a, n), pull(&mut b, n));
}
