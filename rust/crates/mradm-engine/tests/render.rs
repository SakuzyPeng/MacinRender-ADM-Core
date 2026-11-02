//! The offline render service (PCM consistency 5.2 C2): the C++ `RenderService::render` event
//! order, messages, cancellation points, temporary files and island requests, driven with mock
//! islands; and the fmt fixed-point corpus.
use mradm_dsp::room::loading::{self, Package};
use mradm_dsp::room::quality::Quality;
use mradm_dsp::room::screen::{Mapping, RoomScreenGeometry, RoomScreens};
use mradm_engine::backend::{Host, Screens};
use mradm_engine::catalog::Platform;
use mradm_engine::fmt::fixed;
use mradm_engine::offline::Progress;
use mradm_engine::render::{
    Event, Island, PathText, Preimported, Render, Reply, Request, RoomOptions, operation, stage,
    utc_date,
};
use mradm_engine::request::{Error, Level, Packager};
use mradm_render::room::PolarConversion;
use mradm_render::room_runtime::{Config, Prepare, Resources, Scope};
use mradm_wav::{Chna, ChnaEntry, Format, Reader, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

mod common;
use common::{field, fixed as fixed_text};

/// A test directory removed on drop.
struct Dir(PathBuf);
impl Dir {
    fn new(tag: &str) -> Self {
        let path =
            std::env::temp_dir().join(format!("mradm-engine-render-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn file(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
    /// The file names in the directory but the input.
    fn names(&self) -> Vec<String> {
        let mut names: Vec<String> = std::fs::read_dir(&self.0)
            .unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .filter(|n| n != "in.wav")
            .collect();
        names.sort();
        names
    }
}
impl Drop for Dir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// A policy-v1 scene as a float32 BW64 file of `frames` sine frames at 48 kHz.
fn scene_wav(name: &str, path: &Path, frames: usize) {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../mradm-scene/tests/policy-v1/scenes");
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
                uid: fixed_text::<12>(e[1].as_str().unwrap()),
                track_format: fixed_text::<14>(e[2].as_str().unwrap()),
                pack_format: fixed_text::<11>(e[3].as_str().unwrap()),
                pad: 0,
            }
        })
        .collect::<Vec<_>>();
    let format = Format {
        channels,
        sample_rate: 48_000,
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

fn path_text(path: &Path) -> PathText {
    let text = path.to_str().unwrap().to_owned();
    PathText {
        utf8: text.clone(),
        display: text,
    }
}

const SAF: u32 = 2;
const SAF_BINAURAL: u32 = 6;
const TRIPLE_BALANCE: u32 = 7;

/// The C++ `RenderOptions` defaults with a VBAP 7.1.4 render of `input` to `output`.
fn request(input: &Path, output: &Path) -> Request {
    let screen = RoomScreenGeometry {
        cartesian: 0,
        reserved: 0,
        aspect_ratio: 1.78,
        centre: [0., 0., 1.],
        width: 58.,
    };
    Request {
        input: path_text(input),
        output: Some(path_text(output)),
        renderer: SAF,
        output_layout: "7.1.4".to_owned(),
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
        default_interp_ms: 5,
        object_smoothing_frames: 0,
        apple_geometry: false,
        routing: 0,
        speaker_spread: 0,
        triple_balance_mode: 0,
        binaural_spread: 0,
        split_power: false,
        apple_spatial_preset: 0,
        apple_speaker_rendering_flags: false,
        listener_orientation: [0.; 3],
        render_start_sec: 0.,
        render_end_sec: None,
        room: RoomOptions::default(),
        screens: Screens {
            has_reference: false,
            reference: screen,
            has_reproduction: false,
            reproduction: screen,
        },
        internal_allow_speaker_stereo: false,
        iamf_mp4: false,
        iamf_layers: Vec::new(),
    }
}

/// One render driven to its end: every event (islands included), the islands answered by
/// `answer`, cancellation raised from event `cancel_from` on.
struct Driven {
    events: Vec<Event>,
    render: Render,
}

fn drive_with(
    mut render: Render,
    cancel_from: Option<usize>,
    mut answer: impl FnMut(&Island) -> Reply,
) -> Driven {
    let mut events = Vec::new();
    loop {
        let cancelled = cancel_from.is_some_and(|k| events.len() >= k);
        let event = render.next(cancelled, &TestHost);
        match &event {
            Event::Island(island) => {
                let reply = answer(island);
                render.resume(reply);
            }
            Event::Done => {
                events.push(event);
                break;
            }
            _ => {}
        }
        events.push(event);
        assert!(events.len() < 100_000, "the render does not end");
    }
    Driven { events, render }
}

fn drive(r: Request, platform: Platform, cancel_from: Option<usize>) -> Driven {
    drive_with(Render::new(r, platform, None), cancel_from, ok)
}

fn ok(_: &Island) -> Reply {
    Reply::default()
}

fn failure(code: i32, message: &str, context: &str) -> Reply {
    Reply {
        error: Some(Error::new(code, message, context)),
        ..Reply::default()
    }
}

impl Driven {
    fn progress(&self) -> Vec<&Progress> {
        self.events
            .iter()
            .filter_map(|e| match e {
                Event::Progress(p) => Some(p),
                _ => None,
            })
            .collect()
    }
    fn messages(&self) -> Vec<&'static str> {
        self.progress().iter().map(|p| p.message).collect()
    }
    fn logs(&self) -> Vec<(Level, &str, String)> {
        self.events
            .iter()
            .filter_map(|e| match e {
                Event::Log(l) => Some((l.level, l.module, l.message.clone())),
                _ => None,
            })
            .collect()
    }
    fn log_texts(&self) -> Vec<String> {
        self.logs().into_iter().map(|(_, _, m)| m).collect()
    }
    fn islands(&self) -> Vec<&Island> {
        self.events
            .iter()
            .filter_map(|e| match e {
                Event::Island(i) => Some(i),
                _ => None,
            })
            .collect()
    }
    fn error(&self) -> Option<&Error> {
        self.render.outcome().unwrap().error.as_ref()
    }
    fn result_logs(&self) -> Vec<(Level, String)> {
        self.render.outcome().unwrap().logs.clone()
    }
}

fn reader(path: &Path) -> Reader<std::io::BufReader<File>> {
    Reader::new(std::io::BufReader::new(File::open(path).unwrap())).unwrap()
}

fn bits(path: &Path) -> u16 {
    reader(path).info().format.sample_format.bits()
}

fn samples(path: &Path) -> Vec<u32> {
    let mut reader = reader(path);
    let info = reader.info();
    let mut out = vec![0f32; info.frames as usize * usize::from(info.format.channels)];
    reader.read_frames(&mut out).unwrap();
    out.iter().map(|v| v.to_bits()).collect()
}

fn is_temp(name: &str, purpose: &str, ext: &str) -> bool {
    let Some((stem, rest)) = name.split_once(&format!(".{purpose}.")) else {
        return false;
    };
    let Some(hex) = rest.strip_suffix(ext) else {
        return false;
    };
    stem == "out" && hex.len() == 16 && hex.bytes().all(|b| b.is_ascii_hexdigit())
}

#[test]
fn wav_renders_report_the_cpp_events() {
    let dir = Dir::new("wav");
    let input = dir.file("in.wav");
    scene_wav("objects-point", &input, 48_000);
    let output = dir.file("out.wav");
    let mut r = request(&input, &output);
    r.measure_loudness = true;
    r.loudness_target_lufs = -20.;
    r.final_gain_db = 3.;
    r.output_bit_depth = 1;
    let run = drive(r, Platform::default(), None);
    assert_eq!(run.error(), None);
    assert_eq!(
        run.result_logs(),
        [(Level::Info, "render completed".to_owned())]
    );
    let messages = run.messages();
    assert_eq!(
        messages[..6],
        [
            "validating request",
            "probing input",
            "importing scene",
            "applying semantic policy",
            "planning render",
            "preparing backend",
        ]
    );
    assert_eq!(
        messages[messages.len() - 4..],
        [
            "WAV channel layout finalized",
            "writing metadata",
            "metadata written",
            "done"
        ]
    );
    let progress = run.progress();
    assert!(progress.windows(2).all(|w| w[0].fraction <= w[1].fraction));
    let finished = progress.last().unwrap();
    assert_eq!(
        (finished.stage, finished.operation, finished.fraction),
        (stage::FINISHED, operation::FINISH, 1.)
    );
    for p in progress.iter().filter(|p| p.stage == stage::RENDERING) {
        assert!((0.15..=0.88).contains(&p.fraction), "{p:?}");
    }
    let gain = progress
        .iter()
        .find(|p| p.operation == operation::APPLY_GAIN)
        .unwrap();
    assert_eq!(gain.stage, stage::POST_PROCESSING);
    assert!((0.885..=0.89).contains(&gain.fraction));
    let logs = run.log_texts();
    assert_eq!(logs[0], format!("render request: {}", input.display()));
    assert!(logs.iter().any(|l| l.starts_with("scene: 1 programmes, ")));
    let ordered = [
        "measured loudness: ",
        "measured true peak: ",
        "loudness target -20.0 LUFS → gain ",
        "final gain +3.00 dB (unconstrained, bypasses peak limit)",
        "applying total gain ",
        "converting to 24-bit integer PCM",
        "finalizing machine-readable WAV channel layout",
    ];
    let mut at = 0;
    for prefix in ordered {
        at += logs[at..]
            .iter()
            .position(|l| l.starts_with(prefix))
            .unwrap_or_else(|| panic!("{prefix} after {at}: {logs:#?}"));
    }
    // The returned metrics include the applied gain, like the file metadata.
    let metrics = run.render.outcome().unwrap().metrics.unwrap();
    assert!(metrics.lufs.is_some() && metrics.peak_dbtp.is_some());
    assert_eq!(dir.names(), ["out.wav"]);
    assert_eq!(bits(&output), 24);
}

#[test]
fn request_errors_keep_the_cpp_texts_and_logs() {
    let dir = Dir::new("errors");
    let input = dir.file("in.wav");
    scene_wav("objects-point", &input, 4800);
    let output = dir.file("out.wav");
    let error = |r: Request| {
        let run = drive(r, Platform::default(), None);
        let e = run.error().cloned().unwrap();
        (e.code, e.message, e.context, run.result_logs())
    };
    let mut r = request(Path::new(""), &output);
    r.input = PathText::default();
    assert_eq!(
        error(r),
        (1, "input path is required".into(), String::new(), vec![])
    );
    let mut r = request(&input, &output);
    r.triple_balance_mode = 2;
    assert_eq!(
        error(r),
        (
            1,
            "invalid Triple Balance mode".into(),
            String::new(),
            vec![]
        )
    );
    let mut r = request(&input, &output);
    r.triple_balance_mode = 1;
    assert_eq!(
        error(r),
        (
            1,
            "D mode requires the Triple Balance renderer".into(),
            String::new(),
            vec![]
        )
    );
    let mut r = request(&input, &output);
    r.peak_limit = false;
    r.peak_normalize_to_limit = true;
    let msg = "peak normalization to limit requires peak limiting to be enabled";
    assert_eq!(
        error(r),
        (
            1,
            msg.into(),
            String::new(),
            vec![(Level::Error, msg.into())]
        )
    );
    let mut r = request(&input, &output);
    r.final_gain_db = f64::NAN;
    let msg = "final gain must be finite";
    assert_eq!(
        error(r),
        (
            1,
            msg.into(),
            String::new(),
            vec![(Level::Error, msg.into())]
        )
    );
    let mut r = request(&input, &output);
    r.sofa = Some(Default::default());
    let msg = "--sofa is valid only for binaural output";
    assert_eq!(
        error(r),
        (
            1,
            msg.into(),
            "layout=4+7+0".into(),
            vec![(Level::Error, msg.into())]
        )
    );
    let mut r = request(&input, &output);
    r.renderer = SAF_BINAURAL;
    r.output_layout = "binaural".into();
    r.apple_spatial_preset = 1;
    let msg = "--apple-spatial-preset headphone-default requires --renderer apple";
    assert_eq!(
        error(r),
        (
            1,
            msg.into(),
            String::new(),
            vec![(Level::Error, msg.into())]
        )
    );
    assert_eq!(dir.names(), Vec::<String>::new());
}

#[test]
fn the_semantic_report_rides_every_later_error() {
    let dir = Dir::new("report");
    let input = dir.file("in.wav");
    scene_wav("objects-point", &input, 4800);
    let report = |r: &Driven| run_report(r);
    fn run_report(r: &Driven) -> Option<String> {
        r.render.outcome().unwrap().semantic_report.clone()
    }
    let base = |out: &str| {
        let mut r = request(&input, &dir.file(out));
        r.capture_semantic_report = true;
        r
    };
    // Before the report: the SOFA check.
    let mut r = base("out.wav");
    r.sofa = Some(Default::default());
    assert_eq!(report(&drive(r, Platform::default(), None)), None);
    // The output target (FLAC above 8 channels), the screens check and a backend failure.
    let run = drive(base("out.flac"), Platform::default(), None);
    assert_eq!(run.error().unwrap().code, 2);
    let text = report(&run).unwrap();
    assert!(text.contains("mradm.semantic-report.v1"));
    let mut r = base("out.wav");
    r.screens.has_reproduction = true;
    let run = drive(r, Platform::default(), None);
    assert_eq!(
        run.error().unwrap().message,
        "屏幕配置当前仅支持 room-binaural"
    );
    assert_eq!(report(&run).as_deref(), Some(text.as_str()));
    let mut r = base("out.wav");
    r.output_layout = "9+10+3".into();
    r.renderer = TRIPLE_BALANCE;
    r.triple_balance_mode = 1;
    let run = drive(r, Platform::default(), None);
    assert!(run.error().is_some());
    assert!(report(&run).unwrap().contains("renderer_effective"));
    // Cancellation after the preparation and an encoder failure.
    let run = drive_cancel_at(base("out.wav"), "preparing backend");
    assert_eq!(run.error().unwrap().code, 5);
    assert_eq!(
        run.result_logs(),
        [(Level::Info, "render cancelled".to_owned())]
    );
    assert!(report(&run).is_some());
    let mut r = base("out.flac");
    r.output_layout = "5.1".into();
    let run = drive_with(Render::new(r, Platform::default(), None), None, |_| {
        failure(3, "boom", "x")
    });
    assert_eq!(run.error().unwrap().message, "boom");
    assert!(report(&run).is_some());
    assert_eq!(dir.names(), Vec::<String>::new());
}

/// Cancellation raised from the event after the first progress event with `message`.
fn drive_cancel_at(r: Request, message: &str) -> Driven {
    let full = drive(r.clone(), Platform::default(), None);
    let at = full
        .events
        .iter()
        .position(|e| matches!(e, Event::Progress(p) if p.message == message))
        .unwrap();
    let _ = std::fs::remove_file(&r.output.as_ref().unwrap().utf8);
    drive(r, Platform::default(), Some(at + 1))
}

#[test]
fn cancellation_stops_at_the_cpp_points() {
    let dir = Dir::new("cancel");
    let input = dir.file("in.wav");
    scene_wav("objects-point", &input, 48_000);
    let mut r = request(&input, &dir.file("out.wav"));
    r.measure_loudness = true;
    r.output_bit_depth = 2;
    let full = drive(r.clone(), Platform::default(), None);
    std::fs::remove_file(dir.file("out.wav")).unwrap();
    // After the import, the preparation, the render and each pass, a cancelled render emits nothing
    // more and leaves no output.
    // The render's own last event is the driver's "wrote ... frames" log.
    let last_render = full
        .events
        .iter()
        .position(|e| matches!(e, Event::Log(l) if l.message.starts_with("wrote ")))
        .unwrap();
    let mut points = vec![last_render];
    for message in ["gain applied", "bit depth converted"] {
        points.push(
            full.events
                .iter()
                .position(|e| matches!(e, Event::Progress(p) if p.message == message))
                .unwrap(),
        );
    }
    let import_logs = full
        .events
        .iter()
        .position(|e| matches!(e, Event::Log(l) if l.message.starts_with("scene: ")))
        .unwrap();
    points.push(import_logs);
    for at in points {
        let run = drive(r.clone(), Platform::default(), Some(at + 1));
        assert_eq!(run.events.len(), at + 2, "{:?}", &run.events[at..]);
        assert_eq!(run.error().unwrap().code, 5);
        assert_eq!(
            run.result_logs(),
            [(Level::Info, "render cancelled".to_owned())]
        );
        assert_eq!(dir.names(), Vec::<String>::new());
    }
    // Every other point ends cancelled or, after the last check, completes.
    for at in 0..full.events.len() {
        let run = drive(r.clone(), Platform::default(), Some(at));
        match run.error() {
            Some(e) => {
                assert_eq!(e.code, 5, "{at}");
                assert_eq!(dir.names(), Vec::<String>::new(), "{at}");
            }
            None => assert_eq!(dir.names(), ["out.wav"], "{at}"),
        }
        let _ = std::fs::remove_file(dir.file("out.wav"));
    }
}

#[test]
fn lossy_outputs_request_islands_and_clean_up() {
    let dir = Dir::new("lossy");
    let input = dir.file("in.wav");
    scene_wav("objects-point", &input, 48_000);
    let output = dir.file("out.flac");
    let mut r = request(&input, &output);
    r.output_layout = "5.1".into();
    r.measure_loudness = true;
    r.final_gain_db = -2.;
    let names = std::cell::RefCell::new(Vec::new());
    let encoder = |island: &Island| {
        names.borrow_mut().push(dir.names());
        match island {
            Island::EncodeFlac { dst, .. } => {
                std::fs::write(&dst.utf8, b"flac").unwrap();
                Reply::default()
            }
            _ => Reply::default(),
        }
    };
    let run = drive_with(
        Render::new(r.clone(), Platform::default(), None),
        None,
        encoder,
    );
    assert_eq!(run.error(), None);
    let islands = run.islands();
    assert_eq!(islands.len(), 2);
    let Island::EncodeFlac { src, dst } = islands[0] else {
        panic!("{:?}", islands[0]);
    };
    let src_name = Path::new(&src.utf8).file_name().unwrap().to_str().unwrap();
    let dst_name = Path::new(&dst.utf8).file_name().unwrap().to_str().unwrap();
    assert!(is_temp(src_name, "render_tmp", ".wav"), "{src_name}");
    assert!(is_temp(dst_name, "output_tmp", ".flac"), "{dst_name}");
    assert_eq!(
        names.borrow()[0].len(),
        1,
        "only the render staging exists while encoding"
    );
    let Island::WriteMetadata { path, fields } = islands[1] else {
        panic!("{:?}", islands[1]);
    };
    assert_eq!(path.utf8, output.to_str().unwrap());
    assert_eq!(fields.encoder, "MacinRender ADM Core Alpha");
    assert_eq!(fields.output_layout, "0+5+0");
    let metrics = run.render.outcome().unwrap().metrics.unwrap();
    assert_eq!(
        (fields.lufs, fields.peak_dbtp),
        (metrics.lufs, metrics.peak_dbtp)
    );
    let date = fields.date_utc.as_bytes();
    assert_eq!(
        (date.len(), date[4], date[10], date[19]),
        (20, b'-', b'T', b'Z')
    );
    assert_eq!(std::fs::read(&output).unwrap(), b"flac");
    assert_eq!(dir.names(), ["out.flac"]);
    let logs = run.log_texts();
    assert!(logs.contains(&"encoding float32 render to FLAC (24-bit)".to_owned()));
    std::fs::remove_file(&output).unwrap();

    // A failed encoder: its error, no files.
    let run = drive_with(
        Render::new(r.clone(), Platform::default(), None),
        None,
        |i| match i {
            Island::EncodeFlac { dst, .. } => {
                std::fs::write(&dst.utf8, b"partial").unwrap();
                failure(3, "boom", "path=x")
            }
            _ => Reply::default(),
        },
    );
    let e = run.error().unwrap();
    assert_eq!(
        (e.code, e.message.as_str(), e.context.as_str()),
        (3, "boom", "path=x")
    );
    assert_eq!(run.result_logs(), [(Level::Error, "boom".to_owned())]);
    assert_eq!(dir.names(), Vec::<String>::new());

    // Cancelled while encoding: checked before the installation.
    let at = drive_with(
        Render::new(r.clone(), Platform::default(), None),
        None,
        encoder,
    )
    .events
    .iter()
    .position(|e| matches!(e, Event::Island(Island::EncodeFlac { .. })))
    .unwrap();
    std::fs::remove_file(&output).unwrap();
    let run = drive_with(
        Render::new(r.clone(), Platform::default(), None),
        Some(at + 1),
        encoder,
    );
    assert_eq!(run.error().unwrap().code, 5);
    assert_eq!(dir.names(), Vec::<String>::new());

    // A failed metadata write only warns.
    let run = drive_with(
        Render::new(r.clone(), Platform::default(), None),
        None,
        |i| match i {
            Island::EncodeFlac { dst, .. } => {
                std::fs::write(&dst.utf8, b"flac").unwrap();
                Reply::default()
            }
            Island::WriteMetadata { .. } => failure(3, "no tags", ""),
            _ => Reply::default(),
        },
    );
    assert_eq!(run.error(), None);
    assert!(run.logs().contains(&(
        Level::Warning,
        "engine",
        "metadata write failed: no tags".to_owned()
    )));
    assert_eq!(dir.names(), ["out.flac"]);
    std::fs::remove_file(&output).unwrap();

    // Opus: the layout and bitrate.
    let mut opus = r.clone();
    opus.output = Some(path_text(&dir.file("out.mka")));
    opus.opus_bitrate_per_ch_kbps = 48;
    let run = drive_with(
        Render::new(opus, Platform::default(), None),
        None,
        |i| match i {
            Island::EncodeOpus { dst, .. } => {
                std::fs::write(&dst.utf8, b"mka").unwrap();
                Reply::default()
            }
            _ => Reply::default(),
        },
    );
    assert_eq!(run.error(), None);
    let Island::EncodeOpus {
        layout, bitrate, ..
    } = run.islands()[0]
    else {
        panic!();
    };
    assert_eq!((layout.as_str(), *bitrate), ("0+5+0", 48));
    assert!(run.messages().contains(&"Opus encoded"));
    assert_eq!(dir.names(), ["out.mka"]);
}

#[test]
fn iamf_outputs_stage_32_bit_pcm_and_package_mp4() {
    let dir = Dir::new("iamf");
    let input = dir.file("in.wav");
    scene_wav("objects-point", &input, 9600);
    let mut r = request(&input, &dir.file("out.mp4"));
    r.output_layout = "5.1".into();
    r.measure_loudness = true;
    r.iamf_mp4 = true;
    let platform = Platform {
        iamf: true,
        ..Platform::default()
    };
    let depth = std::cell::Cell::new(0);
    let run = drive_with(Render::new(r, platform, None), None, |i| match i {
        Island::ProbeMp4Packager => Reply {
            packager: Packager::Mp4box,
            ..Reply::default()
        },
        Island::EncodeIamf { src, dst, .. } => {
            depth.set(bits(Path::new(&src.utf8)));
            std::fs::write(&dst.utf8, b"iamf").unwrap();
            Reply::default()
        }
        Island::PackageMp4 { src, dst } => {
            assert_eq!(std::fs::read(&src.utf8).unwrap(), b"iamf");
            std::fs::write(&dst.utf8, b"mp4").unwrap();
            Reply::default()
        }
        _ => Reply::default(),
    });
    assert_eq!(run.error(), None);
    assert_eq!(depth.get(), 32);
    let kinds: Vec<&str> = run
        .islands()
        .iter()
        .map(|i| match i {
            Island::ProbeMp4Packager => "probe",
            Island::EncodeIamf { .. } => "iamf",
            Island::PackageMp4 { .. } => "package",
            Island::WriteMetadata { .. } => "metadata",
            _ => "other",
        })
        .collect();
    assert_eq!(kinds, ["probe", "iamf", "package", "metadata"]);
    let Island::EncodeIamf {
        lufs,
        peak_dbtp,
        layers,
        ..
    } = run.islands()[1]
    else {
        panic!();
    };
    let metrics = run.render.outcome().unwrap().metrics.unwrap();
    assert_eq!((*lufs, *peak_dbtp), (metrics.lufs, metrics.peak_dbtp));
    assert!(layers.is_empty());
    assert_eq!(dir.names(), ["out.mp4"]);
}

#[test]
fn island_progress_maps_into_the_render_range() {
    let dir = Dir::new("map");
    let input = dir.file("in.wav");
    scene_wav("objects-point", &input, 4800);
    let mut r = request(&input, &dir.file("out.flac"));
    r.output_layout = "5.1".into();
    let mut render = Render::new(r, Platform::default(), None);
    loop {
        match render.next(false, &TestHost) {
            Event::Island(Island::EncodeFlac { .. }) => break,
            Event::Done => panic!("no FLAC island"),
            _ => {}
        }
    }
    let raw = |stage, stage_fraction, message| Progress {
        stage,
        operation: operation::UNKNOWN,
        fraction: 0.,
        stage_fraction,
        current: 7,
        total: 9,
        message,
    };
    let half = render.map_progress(&raw(stage::RENDERING, 0.5, ""));
    assert_eq!(
        (
            half.stage,
            half.operation,
            half.current,
            half.total,
            half.message
        ),
        (
            stage::POST_PROCESSING,
            operation::ENCODE_FLAC,
            7,
            9,
            "encoding FLAC"
        )
    );
    assert!((half.fraction - 0.94).abs() < 1e-12 && half.stage_fraction == 0.5);
    let done = render.map_progress(&raw(stage::FINISHED, 0., "x"));
    assert_eq!(
        (done.fraction, done.stage_fraction, done.message),
        (0.98, 1., "x")
    );
    let nan = render.map_progress(&raw(stage::RENDERING, f64::NAN, ""));
    assert_eq!((nan.fraction, nan.stage_fraction), (0.9, 0.));
}

#[test]
fn preimported_scenes_skip_import_policy_report_and_preparation() {
    let dir = Dir::new("preimported");
    let input = dir.file("in.wav");
    scene_wav("objects-point", &input, 9600);
    let mut r = request(&input, &dir.file("out.wav"));
    r.semantic_policy_json =
        Some(br#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"gain_db":-6}}}"#.to_vec());
    r.capture_semantic_report = true;
    let mut first = drive(r.clone(), Platform::default(), None);
    assert_eq!(first.error(), None);
    let scene = first.render.scene().unwrap().clone();
    let prepared = first.render.take_prepared().unwrap();
    let expected = samples(&dir.file("out.wav"));
    std::fs::remove_file(dir.file("out.wav")).unwrap();
    let preimported = Preimported {
        scene,
        prepared: Some(prepared),
        apple_prepared: false,
    };
    let mut second = drive_with(
        Render::new(r, Platform::default(), Some(preimported)),
        None,
        ok,
    );
    assert_eq!(second.error(), None);
    let messages = second.messages();
    assert!(messages.contains(&"using cached scene"));
    for absent in [
        "importing scene",
        "applying semantic policy",
        "preparing backend",
    ] {
        assert!(!messages.contains(&absent), "{absent}");
    }
    assert_eq!(second.render.outcome().unwrap().semantic_report, None);
    assert!(
        second
            .logs()
            .iter()
            .all(|(_, module, _)| *module != "semantic-policy")
    );
    assert!(second.render.take_prepared().is_none());
    assert_eq!(samples(&dir.file("out.wav")), expected);
}

#[test]
fn utc_dates_follow_gmtime() {
    for (seconds, date) in [
        (0, "1970-01-01T00:00:00Z"),
        (-1, "1969-12-31T23:59:59Z"),
        (951_782_400, "2000-02-29T00:00:00Z"),
        (951_868_799, "2000-02-29T23:59:59Z"),
        (1_709_251_199, "2024-02-29T23:59:59Z"),
        (4_107_542_399, "2100-02-28T23:59:59Z"),
        (4_107_542_400, "2100-03-01T00:00:00Z"),
        (-2_208_988_800, "1900-01-01T00:00:00Z"),
        (253_402_300_799, "9999-12-31T23:59:59Z"),
    ] {
        assert_eq!(utc_date(seconds), date);
    }
}

/// `render-v1/fixed.txt`: fmt 11.2 `{:.1f}` `{:.2f}` `{:+.2f}` `{:.4f}` of doubles and `{:.4f}`
/// `{:.2f}` of floats, generated with the C++ engine's fmt at main `13643c2`.
#[test]
fn fixed_matches_fmt() {
    let text = std::fs::read_to_string(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/render-v1/fixed.txt"),
    )
    .unwrap();
    let mut checked = 0;
    for line in text.lines() {
        let fields: Vec<&str> = line.split(' ').collect();
        match fields[0] {
            "d" => {
                let v = f64::from_bits(u64::from_str_radix(fields[1], 16).unwrap());
                assert_eq!(fixed(v, 1, false), fields[2], "{line}");
                assert_eq!(fixed(v, 2, false), fields[3], "{line}");
                assert_eq!(fixed(v, 2, true), fields[4], "{line}");
                assert_eq!(fixed(v, 4, false), fields[5], "{line}");
            }
            "f" => {
                let v = f64::from(f32::from_bits(u32::from_str_radix(fields[1], 16).unwrap()));
                assert_eq!(fixed(v, 4, false), fields[2], "{line}");
                assert_eq!(fixed(v, 2, false), fields[3], "{line}");
            }
            _ => panic!("{line}"),
        }
        checked += 1;
    }
    assert_eq!(checked, 8363);
}
