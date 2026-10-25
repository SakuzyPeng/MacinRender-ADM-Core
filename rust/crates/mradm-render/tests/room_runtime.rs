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
        "{}/../../../tests/fixtures/room/phase2/room.json",
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
        let package = package();
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
    let fixture = Fixture::new();
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
    let fixture = Fixture::new();
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
