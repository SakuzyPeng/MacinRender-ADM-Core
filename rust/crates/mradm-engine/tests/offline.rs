//! The backend preparation entry and the offline render step (PCM consistency 5.2 C1): the C++
//! shells' checks and logs, and the event sequences, cancellation points and locators of the
//! loudspeaker, binaural and room drivers they replaced.
use mradm_dsp::room::loading::{self, Package};
use mradm_dsp::room::quality::Quality;
use mradm_dsp::room::screen::{Mapping, RoomScreenGeometry, RoomScreens};
use mradm_engine::backend::{
    self, BinauralSpreadMode, Host, Kind, Prepared, RoomRequest, Routing, Screens, SpeakerSpread,
};
use mradm_engine::offline::{Event, Metrics, Progress, Run, Settings, operation, stage};
use mradm_engine::request::{Error, Level};
use mradm_render::offline::Window;
use mradm_render::room::PolarConversion;
use mradm_render::room_runtime::{Config, Prepare, Resources, Scope};
use mradm_scene::RenderScene;
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

mod common;
use common::{field, fixed};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..")
}

fn policy_scenes() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../mradm-scene/tests/policy-v1/scenes")
}

/// A test directory removed on drop.
struct Dir(PathBuf);
impl Dir {
    fn new(tag: &str) -> Self {
        let path = std::env::temp_dir().join(format!("mradm-engine-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn file(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}
impl Drop for Dir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// A policy-v1 scene as a float32 BW64 file of `frames` sine frames at `rate`.
fn scene_wav(name: &str, path: &Path, frames: usize, rate: u32) {
    let dir = policy_scenes();
    let meta =
        mradm_json::parse(&std::fs::read(dir.join(format!("{name}.json"))).unwrap()).unwrap();
    let channels = field(&meta, "channels").as_f64().unwrap() as u16;
    let entries = field(&meta, "chna")
        .as_array()
        .unwrap()
        .iter()
        .map(|e| {
            let e = e.as_array().unwrap();
            ChnaEntry {
                track_index: e[0].as_f64().unwrap() as u16,
                uid: fixed::<12>(e[1].as_str().unwrap()),
                track_format: fixed::<14>(e[2].as_str().unwrap()),
                pack_format: fixed::<11>(e[3].as_str().unwrap()),
                pad: 0,
            }
        })
        .collect::<Vec<_>>();
    let format = Format {
        channels,
        sample_rate: rate,
        sample_format: SampleFormat::Float32,
        channel_mask: None,
    };
    let options = WriterOptions {
        container: None,
        axml: Some(std::fs::read(dir.join(format!("{name}.xml"))).unwrap()),
        chna: Some(Chna {
            tracks: entries.len() as u16,
            entries,
        }),
    };
    let mut writer = Writer::new(File::create(path).unwrap(), format, options).unwrap();
    let samples: Vec<f32> = (0..frames * usize::from(channels))
        .map(|i| 0.25 * ((i as f32) * 0.013).sin())
        .collect();
    writer.write_frames(&samples).unwrap();
    writer.finish().unwrap();
}

fn import(path: &Path) -> RenderScene {
    mradm_scene::import(path.to_str().unwrap().as_bytes(), Default::default()).unwrap()
}

fn scope(call: &mut dyn FnMut() -> mradm_dsp::Result<()>) -> Result<(), (i32, &'static str)> {
    call().map_err(|e| match e {
        mradm_dsp::Error::InvalidArgument(s) => (1, s),
        mradm_dsp::Error::Unsupported(s) => (2, s),
        mradm_dsp::Error::Io(s) => (3, s),
        mradm_dsp::Error::RenderFailed(s) => (4, s),
    })
}

/// The platform layer without the floating-point scope and CUDA.
struct TestHost;
impl Host for TestHost {
    fn validate_screens(&self, screens: &RoomScreens) -> Result<(), (i32, String)> {
        scope(&mut || Mapping::new(*screens).map(drop)).map_err(|(c, m)| (c, m.to_owned()))
    }
    fn prepare_room(
        &self,
        package: &Package,
        hrir: &[u8],
        sofa: &[u8],
        quality: u32,
        _metres: f64,
    ) -> Result<Resources, (i32, String)> {
        let quality = Quality::try_from(quality).map_err(|_| (1, "quality".to_owned()))?;
        loading::prepare(package, hrir, sofa, None, quality)
            .map(|set| Resources { set, cuda: false })
            .map_err(|e| (1, e.to_string()))
    }
    fn room_polar(&self) -> PolarConversion {
        loading::polar
    }
    fn room_scope(&self) -> Scope {
        scope
    }
    fn room_switch(&self) -> Prepare {
        Arc::new(|_: &Config, reuse: &Resources| {
            Ok(Resources {
                set: Arc::clone(&reuse.set),
                cuda: false,
            })
        })
    }
}

fn request<'a>(kind: Kind, layout: &'a [u8], input: &'a [u8]) -> backend::Request<'a> {
    backend::Request {
        kind,
        layout_id: layout,
        apple_geometry: false,
        routing: Routing::Automatic,
        matrix: None,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread: BinauralSpreadMode::Automatic,
        triple_balance_d: false,
        split_power: false,
        smoothing_frames: 0,
        default_interp_ms: 5,
        input_path: input,
        sofa: None,
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry {
                cartesian: 0,
                reserved: 0,
                aspect_ratio: 1.78,
                centre: [0., 0., 1.],
                width: 58.,
            },
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        room: RoomRequest {
            package: None,
            hrir: b"",
            metres: 0.,
            quality: 0,
        },
    }
}

fn prepared(scene: &RenderScene, r: &backend::Request) -> (Vec<(Level, String)>, Arc<Prepared>) {
    let p = backend::prepare(scene, r, &TestHost);
    let logs = p
        .outcome
        .logs
        .iter()
        .map(|l| (l.level, l.message.clone()))
        .collect();
    (logs, p.outcome.result.unwrap())
}

fn rejection(scene: &RenderScene, r: &backend::Request) -> (Vec<String>, Error) {
    let p = backend::prepare(scene, r, &TestHost);
    let logs = p.outcome.logs.iter().map(|l| l.message.clone()).collect();
    let Err(e) = p.outcome.result else {
        panic!(
            "{:?} {} prepared",
            r.kind,
            String::from_utf8_lossy(r.layout_id)
        );
    };
    (logs, e)
}

fn settings<'a>(
    scene: &RenderScene,
    input: &'a [u8],
    output: &'a [u8],
    layout: &'a [u8],
) -> Settings<'a> {
    Settings {
        input_path: input,
        output_path: output,
        input_display: input,
        output_display: output,
        output_layout: layout,
        inputs: usize::from(scene.info.channels),
        frames: scene.info.frames,
        sample_rate: scene.info.sample_rate,
        default_interp_ms: 5,
        smoothing_frames: 0,
        window: None,
        meter_window: None,
        pose: [0.; 3],
    }
}

/// All events of a run, cancelling from the `cancel_at`-th call on.
fn drive(run: &mut Run, cancel_at: Option<usize>) -> (Vec<Event>, Result<Metrics, Error>) {
    let mut events = Vec::new();
    for call in 0.. {
        match run.next(cancel_at.is_some_and(|n| call >= n), &TestHost) {
            Ok(Event::Done(m)) => return (events, Ok(m)),
            Ok(e) => events.push(e),
            Err(e) => return (events, Err(e)),
        }
    }
    unreachable!()
}

fn progress(events: &[Event]) -> Vec<Progress> {
    events
        .iter()
        .filter_map(|e| match e {
            Event::Progress(p) => Some(*p),
            _ => None,
        })
        .collect()
}

fn logs(events: &[Event]) -> Vec<String> {
    events
        .iter()
        .filter_map(|e| match e {
            Event::Log(l) => Some(format!("[{}] {}", l.module, l.message)),
            _ => None,
        })
        .collect()
}

fn read(path: &Path) -> Vec<f32> {
    let mut reader =
        mradm_wav::Reader::new(std::io::BufReader::new(File::open(path).unwrap())).unwrap();
    let info = reader.info();
    let mut out = vec![0.; info.frames as usize * usize::from(info.format.channels)];
    reader.read_frames(&mut out).unwrap();
    out
}

#[test]
fn backend_checks_and_logs_follow_the_cpp_shells() {
    let dir = Dir::new("backend");
    let wav = dir.file("objects-point.wav");
    scene_wav("objects-point", &wav, 4800, 48000);
    let scene = import(&wav);
    let input = wav.to_str().unwrap().as_bytes();

    let (logs, e) = rejection(&scene, &request(Kind::Vbap, b"5.1.9", input));
    assert!(logs.is_empty());
    assert_eq!(
        (e.code, e.message.as_str(), e.context.as_str()),
        (
            2,
            "unsupported VBAP output layout '5.1.9' for speaker geometry 'standard'",
            ""
        )
    );
    let mut apple = request(Kind::Vbap, b"nope", input);
    apple.apple_geometry = true;
    assert_eq!(
        rejection(&scene, &apple).1.message,
        "unsupported VBAP output layout 'nope' for speaker geometry 'apple'"
    );
    let (logs, p) = prepared(&scene, &request(Kind::Vbap, b"4+7+0", input));
    assert_eq!(
        logs[0],
        (Level::Info, "speaker geometry: standard".to_owned())
    );
    assert_eq!(
        logs[1],
        (Level::Info, "DirectSpeakers routing: label".to_owned())
    );
    assert_eq!(
        (p.outputs, p.inputs, p.sample_rate),
        (12, usize::from(scene.info.channels), 48000)
    );
    let mut position = request(Kind::Vbap, b"0+5+0", input);
    position.routing = Routing::Position;
    assert_eq!(
        prepared(&scene, &position).0[1].1,
        "DirectSpeakers routing: position"
    );

    for (kind, message) in [
        (
            Kind::Hoa,
            "HOA renderer does not support explicit DirectSpeakers routing; use automatic",
        ),
        (
            Kind::Ear,
            "EAR renderer supports only automatic or matrix DirectSpeakers routing",
        ),
        (
            Kind::Binaural,
            "SAF binaural renderer does not support explicit DirectSpeakers routing; use automatic",
        ),
    ] {
        let mut r = request(kind, b"hoa3", input);
        r.routing = Routing::Label;
        let (logs, e) = rejection(&scene, &r);
        assert!(logs.is_empty());
        assert_eq!((e.code, e.message.as_str()), (2, message));
    }

    let (logs, p) = prepared(&scene, &request(Kind::Binaural, b"binaural", input));
    assert!(
        logs.iter()
            .any(|(_, l)| l.starts_with("HRTF source: ") && l.ends_with(" @ 48000 Hz)"))
    );
    assert_eq!(p.outputs, 2);
    let low = dir.file("objects-point-44k.wav");
    scene_wav("objects-point", &low, 4410, 44100);
    let low_scene = import(&low);
    let low_input = low.to_str().unwrap().as_bytes();
    let e = rejection(&low_scene, &request(Kind::Binaural, b"binaural", low_input)).1;
    assert_eq!(
        (e.code, e.message.as_str(), e.context),
        (
            1,
            "binaural renderer requires 48000 Hz input, got 44100",
            format!("input={}", low.display())
        )
    );

    // The screens are checked before the room package.
    let mut room = request(Kind::Room, b"binaural", input);
    room.screens.reference.width = -1.;
    let e = rejection(&scene, &room).1;
    assert_eq!(e.context, "screen");
    let e = rejection(&scene, &request(Kind::Room, b"binaural", input)).1;
    assert_eq!(
        (e.code, e.message.as_str(), e.context.as_str()),
        (1, "room-binaural 需要房间包", "")
    );

    // The Triple Balance report also describes a rejection.
    let p = backend::prepare(
        &scene,
        &request(Kind::TripleBalance, b"0+5+0", input),
        &TestHost,
    );
    assert!(p.outcome.result.is_err());
    assert!(p.report.is_some_and(|r| r.contains("triple-balance")));
}

#[test]
fn speaker_runs_report_the_cpp_events() {
    let dir = Dir::new("speaker");
    let wav = dir.file("objects-point.wav");
    scene_wav("objects-point", &wav, 5000, 48000);
    let scene = import(&wav);
    let input = wav.to_str().unwrap().as_bytes();
    for (kind, layout, begin, step, outputs) in [
        (
            Kind::Vbap,
            &b"4+7+0"[..],
            "rendering audio",
            "rendering",
            12,
        ),
        (Kind::Hoa, b"hoa3", "encoding HOA", "encoding", 16),
        (Kind::Ear, b"0+5+0", "rendering audio", "rendering", 6),
    ] {
        let (_, p) = prepared(&scene, &request(kind, layout, input));
        let out = dir.file("speaker.wav");
        let output = out.to_str().unwrap().as_bytes();
        let mut run = Run::new(Arc::clone(&p), &settings(&scene, input, output, layout));
        let (events, metrics) = drive(&mut run, None);
        let metrics = metrics.unwrap();
        assert!(
            metrics.peak_dbtp.is_some(),
            "short input: no gated loudness"
        );
        let tracks = p.bindings.len();
        let log = logs(&events);
        let module = kind.module();
        let begin_log = match kind {
            Kind::Hoa => format!(
                "[{module}] encoding {tracks} input channels (Objects + DirectSpeakers) → HOA3 (16 ch), 5000 frames"
            ),
            Kind::Ear => format!("[{module}] rendering {tracks} tracks → 6 channels, 5000 frames"),
            _ => format!(
                "[{module}] rendering {tracks} tracks (Objects + DirectSpeakers) → 12 channels, 5000 frames"
            ),
        };
        assert_eq!(
            log,
            vec![
                begin_log,
                format!("[{module}] wrote 5000 frames to {}", out.display())
            ]
        );
        assert_eq!(
            events[0],
            Event::Log(mradm_engine::request::Log {
                level: Level::Info,
                module,
                message: log[0][module.len() + 3..].to_owned(),
            })
        );
        let p = progress(&events);
        assert_eq!(
            (p[0].fraction, p[0].stage_fraction, p[0].message),
            (0.3, 0., begin)
        );
        assert!(
            p[1..p.len() - 1]
                .iter()
                .all(|e| e.message == step && e.stage == stage::RENDERING)
        );
        assert_eq!(p.len(), 2 + 5);
        let last = p.last().unwrap();
        assert_eq!(
            (last.stage, last.operation, last.fraction, last.message),
            (stage::FINISHED, operation::FINISH, 1., "done")
        );
        assert_eq!(read(&out).len(), 5000 * outputs);
        // The run is spent after its result.
        assert!(run.next(false, &TestHost).is_err());
    }
}

#[test]
fn speaker_windows_cancellation_and_writers() {
    let dir = Dir::new("windows");
    let wav = dir.file("objects-point.wav");
    scene_wav("objects-point", &wav, 9000, 48000);
    let scene = import(&wav);
    let input = wav.to_str().unwrap().as_bytes();
    let (_, p) = prepared(&scene, &request(Kind::Vbap, b"0+5+0", input));
    let full = dir.file("full.wav");
    let full_path = full.to_str().unwrap().as_bytes();
    drive(
        &mut Run::new(
            Arc::clone(&p),
            &settings(&scene, input, full_path, b"0+5+0"),
        ),
        None,
    )
    .1
    .unwrap();
    let full = read(&full);

    let out = dir.file("window.wav");
    let output = out.to_str().unwrap().as_bytes();
    let mut s = settings(&scene, input, output, b"0+5+0");
    s.window = Some(Window {
        start: 2500,
        count: 1_000_000,
    });
    let (events, result) = drive(&mut Run::new(Arc::clone(&p), &s), None);
    result.unwrap();
    assert_eq!(
        logs(&events).last().unwrap(),
        &format!(
            "[saf-vbap] wrote 6500 frames to {} (window [2500, 9000) of 9000 frames)",
            out.display()
        )
    );
    assert_eq!(read(&out), full[2500 * 6..]);

    // Cancellation is sampled before each block only: from the opening call on it fails at the
    // first block, with the output locator.
    for at in 0..3 {
        let (events, result) = drive(
            &mut Run::new(Arc::clone(&p), &settings(&scene, input, output, b"0+5+0")),
            Some(at),
        );
        let e = result.unwrap_err();
        assert_eq!(
            (e.code, e.message.as_str(), e.context),
            (5, "render cancelled", format!("output={}", out.display()))
        );
        assert_eq!(
            progress(&events).len(),
            1,
            "cancelled before the first block"
        );
    }

    // CAF for `.caf` (any case), with the layout's tag; FLAC is never a render target.
    let caf = dir.file("out.CAF");
    let caf_path = caf.to_str().unwrap().as_bytes();
    drive(
        &mut Run::new(Arc::clone(&p), &settings(&scene, input, caf_path, b"0+5+0")),
        None,
    )
    .1
    .unwrap();
    assert_eq!(&std::fs::read(&caf).unwrap()[..4], b"caff");
    let flac = dir.file("out.flac");
    let e = drive(
        &mut Run::new(
            Arc::clone(&p),
            &settings(&scene, input, flac.to_str().unwrap().as_bytes(), b"0+5+0"),
        ),
        None,
    )
    .1
    .unwrap_err();
    assert_eq!((e.code, e.context), (2, format!("path={}", flac.display())));
    let missing = dir.file("missing/out.wav");
    let e = drive(
        &mut Run::new(
            Arc::clone(&p),
            &settings(
                &scene,
                input,
                missing.to_str().unwrap().as_bytes(),
                b"0+5+0",
            ),
        ),
        None,
    )
    .1
    .unwrap_err();
    assert_eq!(
        (e.code, e.context),
        (3, format!("path={}", missing.display()))
    );
    let absent = dir.file("absent.wav");
    let e = drive(
        &mut Run::new(
            Arc::clone(&p),
            &settings(
                &scene,
                absent.to_str().unwrap().as_bytes(),
                output,
                b"0+5+0",
            ),
        ),
        None,
    )
    .1
    .unwrap_err();
    assert_eq!(e.context, format!("path={}", absent.display()));
}

#[test]
fn binaural_runs_report_the_cpp_events() {
    let dir = Dir::new("binaural");
    let wav = dir.file("objects-extent.wav");
    scene_wav("objects-extent", &wav, 6000, 48000);
    let scene = import(&wav);
    let input = wav.to_str().unwrap().as_bytes();
    let (_, p) = prepared(&scene, &request(Kind::Binaural, b"binaural", input));
    let out = dir.file("binaural.wav");
    let output = out.to_str().unwrap().as_bytes();
    let mut s = settings(&scene, input, output, b"binaural");
    s.window = Some(Window {
        start: 1000,
        count: u64::MAX,
    });
    let (events, result) = drive(&mut Run::new(Arc::clone(&p), &s), None);
    result.unwrap();
    let p0 = progress(&events);
    assert_eq!((p0[0].fraction, p0[0].message), (0.2, "rendering"));
    assert_eq!(p0.last().unwrap().message, "done");
    // The end wraps like the C++ unsigned sum before its clamp.
    assert_eq!(
        logs(&events).last().unwrap(),
        &format!(
            "[binaural] wrote 5000 frames to {} (window [1000, 999) of 6000 frames)",
            out.display()
        )
    );
    let e = drive(
        &mut Run::new(
            Arc::clone(&p),
            &settings(&scene, input, output, b"binaural"),
        ),
        Some(2),
    )
    .1
    .unwrap_err();
    assert_eq!((e.code, e.message.as_str()), (5, "render cancelled"));
}

#[test]
fn room_runs_report_the_cpp_events() {
    let dir = Dir::new("room");
    let wav = dir.file("objects-point.wav");
    scene_wav("objects-point", &wav, 6000, 48000);
    let scene = import(&wav);
    let input = wav.to_str().unwrap().as_bytes();
    let manifest = repo().join("tests/fixtures/room/phase2/room.json");
    let package = Arc::new(loading::load_package(manifest.to_str().unwrap().as_bytes()).unwrap());
    let mut r = request(Kind::Room, b"binaural", input);
    r.room.package = Some(package);
    let (_, p) = prepared(&scene, &r);
    let tail = 6000 + p.room.as_ref().map_or(0, |_| 0);
    let out = dir.file("room.wav");
    let output = out.to_str().unwrap().as_bytes();
    let (events, result) = drive(
        &mut Run::new(
            Arc::clone(&p),
            &settings(&scene, input, output, b"binaural"),
        ),
        None,
    );
    result.unwrap();
    assert!(logs(&events).is_empty());
    let progress = progress(&events);
    let count = progress.last().unwrap().total;
    assert!(
        count > tail as u64,
        "the offline render appends the room tail"
    );
    assert!(
        progress
            .iter()
            .all(|e| e.message == "room-binaural" && e.fraction == e.stage_fraction)
    );
    assert_eq!(progress.last().unwrap().current, count);
    assert_eq!(read(&out).len() as u64, count * 2);

    // A window seeks with pre-roll steps, each checked for cancellation.
    let mut s = settings(&scene, input, output, b"binaural");
    s.window = Some(Window {
        start: 4000,
        count: 100,
    });
    let e = drive(&mut Run::new(Arc::clone(&p), &s), Some(1))
        .1
        .unwrap_err();
    assert_eq!(
        (e.code, e.message.as_str(), e.context.as_str()),
        (5, "room-binaural 预热已取消", "")
    );
    let (events, result) = drive(&mut Run::new(Arc::clone(&p), &s), None);
    result.unwrap();
    assert_eq!(progress_of(&events), vec![(100, 100)]);
    let mut s = settings(&scene, input, output, b"binaural");
    s.frames += 1;
    let e = drive(&mut Run::new(Arc::clone(&p), &s), None)
        .1
        .unwrap_err();
    assert_eq!(e.message, "room-binaural prepared 输入格式不匹配");
}

fn progress_of(events: &[Event]) -> Vec<(u64, u64)> {
    progress(events)
        .iter()
        .map(|p| (p.current, p.total))
        .collect()
}

/// Runs to the end, cancelling once a progress event reports its last frame.
fn drive_cancel_after_last_block(run: &mut Run) -> Result<Vec<Event>, Error> {
    let mut events = Vec::new();
    let mut cancelled = false;
    loop {
        match run.next(cancelled, &TestHost)? {
            Event::Done(_) => return Ok(events),
            e => {
                if let Event::Progress(p) = &e {
                    cancelled |= p.total != 0 && p.current == p.total;
                }
                events.push(e);
            }
        }
    }
}

#[test]
fn the_binaural_spreader_tail_ignores_cancellation() {
    let dir = Dir::new("tail");
    let wav = dir.file("objects-extent.wav");
    scene_wav("objects-extent", &wav, 6000, 48000);
    let scene = import(&wav);
    let input = wav.to_str().unwrap().as_bytes();
    let mut r = request(Kind::Binaural, b"binaural", input);
    r.binaural_spread = BinauralSpreadMode::SafSpreader;
    let (logs, p) = prepared(&scene, &r);
    assert!(logs.iter().any(|(_, l)| l.contains("spreader")), "{logs:?}");
    let out = dir.file("tail.wav");
    let output = out.to_str().unwrap().as_bytes();
    let events = drive_cancel_after_last_block(&mut Run::new(
        Arc::clone(&p),
        &settings(&scene, input, output, b"binaural"),
    ))
    .unwrap();
    assert_eq!(progress(&events).last().unwrap().message, "done");
    assert_eq!(read(&out).len(), 6000 * 2, "the tail completes the file");
}

#[test]
fn room_meter_windows_meter_only_the_window() {
    let dir = Dir::new("meter");
    let wav = dir.file("objects-point.wav");
    scene_wav("objects-point", &wav, 24000, 48000);
    let scene = import(&wav);
    let input = wav.to_str().unwrap().as_bytes();
    let manifest = repo().join("tests/fixtures/room/phase2/room.json");
    let package = Arc::new(loading::load_package(manifest.to_str().unwrap().as_bytes()).unwrap());
    let mut r = request(Kind::Room, b"binaural", input);
    r.room.package = Some(package);
    let (_, p) = prepared(&scene, &r);
    let out = dir.file("meter.wav");
    let output = out.to_str().unwrap().as_bytes();
    let window = Window {
        start: 3000,
        count: 19000,
    };
    let full = drive(
        &mut Run::new(
            Arc::clone(&p),
            &settings(&scene, input, output, b"binaural"),
        ),
        None,
    )
    .1
    .unwrap();
    let mut metered = settings(&scene, input, output, b"binaural");
    metered.meter_window = Some(window);
    let metered = drive(&mut Run::new(Arc::clone(&p), &metered), None)
        .1
        .unwrap();
    let mut windowed = settings(&scene, input, output, b"binaural");
    windowed.window = Some(window);
    let windowed = drive(&mut Run::new(Arc::clone(&p), &windowed), None)
        .1
        .unwrap();
    assert_eq!(metered, windowed);
    assert_ne!(metered, full);
}
