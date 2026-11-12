//! The monitor session (PCM consistency 5.5 B) with a mock device and mock streams behind the
//! Apple island path: the seconds conversion, edits that survive rebuilds, the system-spatial
//! orientation, the backend-switch checks and their order, the rebuild with its fallback, the
//! output-device fallback, pending choices, HpTF and an invalidated session.
use mradm_dsp::hptf::{Coefficients, Snapshot, StereoSnapshot};
use mradm_dsp::room::loading::Package;
use mradm_dsp::room::screen::{RoomScreenGeometry, RoomScreens};
use mradm_engine::backend::{self, Screens};
use mradm_engine::monitor::session::{
    AppleOpen, Choice, Device, HptfDesign, Session, SessionHost, Token,
};
use mradm_engine::monitor::{
    Cancel, Monitor, ObjectOverride, OutputStage, Overrides, Source, State, Validate,
};
use mradm_engine::render::{PathText, Request, RoomOptions};
use mradm_render::routing::Error;
use mradm_scene::RenderScene;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

mod common;
use common::backend::TestHost;

const RATE: u32 = 48000;
const APPLE: u32 = 4;

/// One stream the next island opening returns.
#[derive(Clone)]
struct Spec {
    channels: usize,
    rate: u32,
    stage: bool,
    reject: bool,
    fail: Option<&'static str>,
}

impl Default for Spec {
    fn default() -> Self {
        Self {
            channels: 2,
            rate: RATE,
            stage: false,
            reject: false,
            fail: None,
        }
    }
}

/// What the streams saw: one entry per opened stream.
#[derive(Default)]
struct Seen {
    revision: AtomicU64,
    yaw_bits: AtomicU64,
}

struct Stream {
    spec: Spec,
    seen: Arc<Seen>,
    stage: Option<Arc<dyn OutputStage>>,
}

struct Pass;
impl OutputStage for Pass {
    fn render_output(
        &self,
        input: &[f32],
        output: &mut [f32],
        frames: usize,
        _: [f32; 3],
    ) -> usize {
        output[..frames * 2].copy_from_slice(&input[..frames * 2]);
        frames
    }
    fn loop_render_reset(&self, _: u64) {}
    fn render_failed(&self) -> bool {
        false
    }
}

struct Check(bool);
impl Validate for Check {
    fn validate(&self, overrides: &Overrides) -> Result<(), Error> {
        if self.0 || overrides.revision == 99 {
            return Err(err(2, "mock rejects the edit"));
        }
        Ok(())
    }
}

fn err(code: i32, text: &str) -> Error {
    Error {
        code,
        message: text.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

impl Source for Stream {
    fn out_channels(&self) -> usize {
        self.spec.channels
    }
    fn sample_rate(&self) -> u32 {
        self.spec.rate
    }
    fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        output[..frames * self.spec.channels].fill(0.25);
        Ok(frames)
    }
    fn seek(&mut self, _: u64) -> Result<(), Error> {
        Ok(())
    }
    fn seek_with_cancel(&mut self, frame: u64, _: &Cancel) -> Result<(), Error> {
        self.seek(frame)
    }
    fn seek_requires_preroll(&self) -> bool {
        false
    }
    fn validator(&self) -> Arc<dyn Validate> {
        Arc::new(Check(self.spec.reject))
    }
    fn set_overrides(&mut self, overrides: &Overrides) {
        self.seen
            .revision
            .store(overrides.revision, Ordering::SeqCst);
    }
    fn set_orientation(&mut self, pose: [f32; 3]) {
        self.seen
            .yaw_bits
            .store(u64::from(pose[0].to_bits()) | 1 << 32, Ordering::SeqCst);
    }
    fn output_stage(&self) -> Option<Arc<dyn OutputStage>> {
        self.stage.clone()
    }
    fn intermediate_channels(&self) -> usize {
        if self.spec.stage { 2 } else { 0 }
    }
    fn produce_intermediate(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        self.process(output, frames)
    }
}

#[derive(Default)]
struct Calls {
    devices: Vec<Vec<u8>>,
    starts: usize,
    stops: usize,
    /// The token value of each opening.
    opened: Vec<u32>,
    /// The Apple listener orientation of each opening.
    orientations: Vec<[f32; 3]>,
    /// Whether an opening logs a line.
    log_apple: bool,
}

struct MockDevice {
    id: Vec<u8>,
    calls: Arc<Mutex<Calls>>,
}

impl Device for MockDevice {
    fn realtime(&self) -> bool {
        false
    }
    fn start(&self, _: usize, _: u32, _: &Monitor) -> Result<(), Error> {
        if self.id == b"bad" {
            return Err(err(3, "mock device cannot start"));
        }
        self.calls.lock().unwrap().starts += 1;
        Ok(())
    }
    fn stop(&self) {
        self.calls.lock().unwrap().stops += 1;
    }
    fn flush(&self) {}
    fn pause(&self) {}
    fn resume(&self) {}
}

struct Host {
    scene: RenderScene,
    specs: Mutex<VecDeque<Spec>>,
    seen: Mutex<Vec<Arc<Seen>>>,
    calls: Arc<Mutex<Calls>>,
}

impl backend::Host for Host {
    fn validate_screens(&self, screens: &RoomScreens) -> Result<(), (i32, String)> {
        TestHost.validate_screens(screens)
    }
    fn prepare_room(
        &self,
        package: &Package,
        hrir: &[u8],
        sofa: &[u8],
        quality: u32,
        metres: f64,
    ) -> Result<mradm_render::room_runtime::Resources, (i32, String)> {
        TestHost.prepare_room(package, hrir, sofa, quality, metres)
    }
    fn room_polar(&self) -> mradm_render::room::PolarConversion {
        TestHost.room_polar()
    }
    fn room_scope(&self) -> mradm_render::room_runtime::Scope {
        TestHost.room_scope()
    }
    fn room_switch(&self) -> mradm_render::room_runtime::Prepare {
        TestHost.room_switch()
    }
}

impl SessionHost for Host {
    fn scene(&self) -> Option<&RenderScene> {
        Some(&self.scene)
    }
    fn apple(&self) -> bool {
        true
    }
    fn device(&self, _: &Token, device_id: &[u8]) -> Arc<dyn Device> {
        self.calls.lock().unwrap().devices.push(device_id.to_vec());
        Arc::new(MockDevice {
            id: device_id.to_vec(),
            calls: self.calls.clone(),
        })
    }
    fn open_apple(
        &self,
        plan: &AppleOpen<'_>,
        log: &dyn Fn(u32, &str, &[u8]),
    ) -> Result<Box<dyn Source>, Error> {
        let mut calls = self.calls.lock().unwrap();
        calls
            .opened
            .push(*plan.token.downcast_ref::<u32>().unwrap());
        calls.orientations.push(plan.options.orientation);
        if calls.log_apple {
            log(2, "apple", b"apple stream");
        }
        drop(calls);
        let spec = self.specs.lock().unwrap().pop_front().unwrap_or_default();
        if let Some(text) = spec.fail {
            return Err(err(4, text));
        }
        let seen = Arc::new(Seen::default());
        self.seen.lock().unwrap().push(seen.clone());
        Ok(Box::new(Stream {
            stage: spec.stage.then(|| Arc::new(Pass) as Arc<dyn OutputStage>),
            spec,
            seen,
        }))
    }
}

fn request(input: &str) -> Request {
    let text = PathText {
        utf8: input.to_owned(),
        display: input.to_owned(),
    };
    Request {
        input: text,
        output: None,
        renderer: APPLE,
        output_layout: "binaural".into(),
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
            reference: RoomScreenGeometry::default(),
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        internal_allow_speaker_stereo: false,
        iamf_mp4: false,
        iamf_layers: Vec::new(),
    }
}

struct Fixture {
    host: Arc<Host>,
    input: String,
    _dir: std::path::PathBuf,
}

static NEXT: AtomicUsize = AtomicUsize::new(0);

fn fixture(specs: Vec<Spec>) -> Fixture {
    let dir = std::env::temp_dir().join(format!(
        "mradm-engine-session-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::SeqCst)
    ));
    std::fs::create_dir_all(&dir).unwrap();
    let input = dir.join("objects-point.wav");
    common::adm_wav_with(
        &std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../mradm-scene/tests/policy-v1/scenes"),
        "objects-point",
        &input,
        mradm_wav::SampleFormat::Pcm24,
        Some(RATE),
        48000,
    );
    let input = input.to_str().unwrap().to_owned();
    let scene = mradm_scene::import_adm(input.as_bytes()).unwrap();
    Fixture {
        host: Arc::new(Host {
            scene,
            specs: Mutex::new(specs.into()),
            seen: Mutex::default(),
            calls: Arc::default(),
        }),
        input,
        _dir: dir,
    }
}

impl Fixture {
    fn choice(&self, tag: u32, system_spatial: bool) -> Choice {
        Choice {
            request: request(&self.input),
            system_spatial,
            token: Arc::new(tag),
        }
    }
    fn session(&self, system_spatial: bool) -> Session {
        Session::create(
            self.choice(1, system_spatial),
            RATE,
            48000,
            b"",
            self.host.clone(),
        )
        .unwrap_or_else(|e| panic!("{}", String::from_utf8_lossy(&e.message)))
    }
    fn seen(&self, i: usize) -> Arc<Seen> {
        self.host.seen.lock().unwrap()[i].clone()
    }
    fn opened(&self) -> Vec<u32> {
        self.host.calls.lock().unwrap().opened.clone()
    }
}

fn eventually(mut p: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + Duration::from_secs(5);
    while !p() {
        if Instant::now() > end {
            return false;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    true
}

fn edit(revision: u64) -> Overrides {
    Overrides {
        objects: vec![ObjectOverride {
            object_id: b"AO_1001".to_vec(),
            gain_db: -6.,
            ..ObjectOverride::default()
        }],
        revision,
    }
}

fn logs(s: &Session) -> Vec<(u32, String, String)> {
    (0..s.log_count())
        .map(|i| {
            let (l, m, t) = s.log_entry(i).unwrap();
            (l, m, String::from_utf8(t).unwrap())
        })
        .collect()
}

fn message(e: &Error) -> String {
    String::from_utf8_lossy(&e.message).into_owned()
}

fn playhead(s: &Session) -> u64 {
    s.status().unwrap().playhead_frames
}

#[test]
fn seeks_convert_seconds_and_clamp() {
    let f = fixture(Vec::new());
    let s = f.session(false);
    for (seconds, frame) in [
        (0.5, 24000),
        (-1., 0),
        (f64::NAN, 0),
        (5., 48000),
        (f64::INFINITY, 48000),
        (0.25, 12000),
    ] {
        s.seek_seconds(seconds).unwrap();
        assert!(eventually(|| playhead(&s) == frame), "{seconds} -> {frame}");
    }
    s.log(1, "probe", b"line");
    assert_eq!(
        logs(&s).last().unwrap(),
        &(1, "probe".into(), "line".into())
    );
}

#[test]
fn edits_survive_a_device_switch_only_when_accepted() {
    let f = fixture(Vec::new());
    let s = f.session(false);
    s.set_overrides(edit(5)).unwrap();
    let e = s.set_overrides(edit(99)).unwrap_err();
    assert_eq!(message(&e), "mock rejects the edit");
    s.set_output_device(b"usb").unwrap();
    assert!(eventually(|| f.seen(1).revision.load(Ordering::SeqCst) == 5));
    let calls = f.host.calls.lock().unwrap();
    assert_eq!(calls.devices, vec![b"".to_vec(), b"usb".to_vec()]);
    assert_eq!((calls.starts, calls.stops), (2, 1));
}

#[test]
fn system_spatial_keeps_the_stream_orientation_neutral() {
    for (system_spatial, yaw) in [(false, 30f32), (true, 0.)] {
        let f = fixture(Vec::new());
        let s = f.session(system_spatial);
        s.set_orientation([30., 0., 0.]);
        let bits = u64::from(yaw.to_bits()) | 1 << 32;
        assert!(eventually(
            || f.seen(0).yaw_bits.load(Ordering::SeqCst) == bits
        ));
    }
}

#[test]
fn the_apple_stream_gets_the_request_orientation_and_logs_into_the_session() {
    for (system_spatial, expected) in [(false, [10f32, 5., -3.]), (true, [0.; 3])] {
        let f = fixture(Vec::new());
        f.host.calls.lock().unwrap().log_apple = true;
        let mut choice = f.choice(1, system_spatial);
        choice.request.listener_orientation = [10., 5., -3.];
        let s = Session::create(choice, RATE, 48000, b"", f.host.clone())
            .unwrap_or_else(|e| panic!("{}", String::from_utf8_lossy(&e.message)));
        assert_eq!(f.host.calls.lock().unwrap().orientations, vec![expected]);
        let logs: Vec<_> = (0..s.log_count()).filter_map(|i| s.log_entry(i)).collect();
        let last = logs.last().expect("the Apple stream's line");
        assert_eq!(
            (last.0, last.1.as_str(), &last.2[..]),
            (2, "apple", &b"apple stream"[..])
        );
    }
}

#[test]
fn backend_switches_check_edits_then_channels_then_rate() {
    let f = fixture(vec![
        Spec::default(),
        Spec {
            reject: true,
            channels: 3,
            ..Spec::default()
        },
        Spec {
            channels: 3,
            rate: 44100,
            ..Spec::default()
        },
        Spec {
            rate: 44100,
            ..Spec::default()
        },
        Spec {
            fail: Some("mock open failure"),
            ..Spec::default()
        },
    ]);
    let s = f.session(false);
    let texts = [
        "mock rejects the edit",
        "monitor backend switches cannot change the output channel count; use binaural output for headphones",
        "monitor backend switch requires the same sample rate",
        "mock open failure",
    ];
    for text in texts {
        assert_eq!(
            message(&s.switch_backend(f.choice(2, false)).unwrap_err()),
            text
        );
    }
    // The playing stream is untouched.
    assert_eq!(f.opened(), vec![1, 2, 2, 2, 2]);
    assert!(!s.status().unwrap().failed);
}

#[test]
fn applied_switches_become_the_current_choice() {
    let f = fixture(Vec::new());
    let s = f.session(false);
    s.play();
    s.switch_backend(f.choice(2, false)).unwrap();
    // The device rebuild opens the choice the engine last applied.
    assert!(eventually(|| {
        let _ = s.status();
        f.host.seen.lock().unwrap().len() == 2
    }));
    std::thread::sleep(Duration::from_millis(50));
    let _ = s.status();
    s.set_output_device(b"usb").unwrap();
    assert_eq!(f.opened(), vec![1, 2, 2]);
}

#[test]
fn output_stage_flips_rebuild_and_restore_the_session() {
    let stage = Spec {
        stage: true,
        ..Spec::default()
    };
    let f = fixture(vec![Spec::default(), stage.clone(), stage]);
    let s = f.session(false);
    s.set_overrides(edit(7)).unwrap();
    s.seek_seconds(0.5).unwrap();
    assert!(eventually(|| playhead(&s) == 24000));
    s.play();
    s.switch_backend(f.choice(2, false)).unwrap();
    // The flip's own opening is dropped; the rebuild opens the choice again.
    assert_eq!(f.opened(), vec![1, 2, 2]);
    assert!(eventually(|| f.seen(2).revision.load(Ordering::SeqCst) == 7));
    assert_eq!(s.status().unwrap().state, State::Playing);
    let calls = f.host.calls.lock().unwrap();
    assert_eq!((calls.starts, calls.stops), (2, 1));
}

#[test]
fn failed_rebuilds_fall_back_to_the_previous_backend() {
    let stage = Spec {
        stage: true,
        ..Spec::default()
    };
    let fail = Spec {
        fail: Some("mock rebuild failure"),
        ..Spec::default()
    };
    let f = fixture(vec![Spec::default(), stage, fail]);
    let s = f.session(false);
    let e = s.switch_backend(f.choice(2, false)).unwrap_err();
    assert_eq!(message(&e), "mock rebuild failure");
    assert_eq!(f.opened(), vec![1, 2, 2, 1]);
    assert!(s.status().is_some());
    let last = logs(&s).last().cloned().unwrap();
    assert_eq!(
        last,
        (
            2,
            "monitor".into(),
            "切换后端失败,回退到原后端:mock rebuild failure".into()
        )
    );
}

#[test]
fn a_failed_fallback_invalidates_the_session() {
    let stage = Spec {
        stage: true,
        ..Spec::default()
    };
    let fail = |text| Spec {
        fail: Some(text),
        ..Spec::default()
    };
    let f = fixture(vec![Spec::default(), stage, fail("first"), fail("second")]);
    let s = f.session(false);
    assert_eq!(
        message(&s.switch_backend(f.choice(2, false)).unwrap_err()),
        "first"
    );
    assert!(s.status().is_none() && s.levels().is_none() && s.hptf_snapshot().is_none());
    assert_eq!(
        message(&s.seek_seconds(1.).unwrap_err()),
        "监听会话无效:后端重建失败"
    );
    assert_eq!(
        message(&s.set_overrides(edit(1)).unwrap_err()),
        "monitor session is unavailable"
    );
    assert_eq!(
        message(&s.set_output_device(b"usb").unwrap_err()),
        "监听会话无效:后端重建失败"
    );
    s.play();
    s.pause();
    s.set_loop_seconds(0., 1.);
}

#[test]
fn device_failures_fall_back_to_the_default_device() {
    let f = fixture(Vec::new());
    let s = f.session(false);
    s.set_output_device(b"").unwrap();
    assert_eq!(f.host.calls.lock().unwrap().devices.len(), 1, "same device");
    s.set_output_device(b"bad").unwrap();
    assert_eq!(
        logs(&s).last().cloned().unwrap(),
        (
            2,
            "monitor".into(),
            "切换输出设备失败,回落到默认设备:mock device cannot start".into()
        )
    );
    assert_eq!(
        f.host.calls.lock().unwrap().devices,
        vec![b"".to_vec(), b"bad".to_vec(), b"".to_vec()]
    );
    // The fallback device is current: switching to it again is a no-op.
    s.set_output_device(b"").unwrap();
    assert_eq!(f.host.calls.lock().unwrap().devices.len(), 3);
}

struct Gain(f32, AtomicUsize, bool);
impl HptfDesign for Gain {
    fn design(&self, sample_rate: u32) -> Result<StereoSnapshot, Error> {
        if self.2 && self.1.fetch_add(1, Ordering::SeqCst) > 0 {
            return Err(err(1, "mock design failure"));
        }
        let coefficients = Coefficients {
            sample_rate,
            preamp_gain: 0.5,
            max_response_db: self.0,
            ..Coefficients::default()
        };
        Ok(Snapshot {
            coefficients,
            revision: 4,
        }
        .into())
    }
}

#[test]
fn hptf_warns_on_clipping_and_on_a_failed_redesign() {
    let f = fixture(Vec::new());
    let s = f.session(false);
    s.set_hptf(Arc::new(Gain(1.5, AtomicUsize::new(0), true)))
        .unwrap();
    assert_eq!(
        logs(&s).last().cloned().unwrap(),
        (
            2,
            "monitor".into(),
            "耳机补偿曲线可能削波(左 1.500000 dB,右 1.500000 dB)".into()
        )
    );
    s.set_output_device(b"usb").unwrap();
    assert_eq!(
        logs(&s).last().cloned().unwrap(),
        (
            2,
            "monitor".into(),
            "耳机补偿重建失败:mock design failure".into()
        )
    );
    let mono = fixture(vec![Spec {
        channels: 1,
        ..Spec::default()
    }]);
    let s = mono.session(false);
    let e = s
        .set_hptf(Arc::new(Gain(0., AtomicUsize::new(0), false)))
        .unwrap_err();
    assert_eq!(
        (e.code, message(&e)),
        (2, "耳机补偿(HpTF)只适用于双声道耳机监听".into())
    );
}
