//! `mr_adm_room_scene_benchmark ROOM_JSON OTHER_ROOM_JSON [SECONDS=10]
//! [all|stream|stream-cycles|stream-device|kernel|kernel-worker|latency] [SOURCES=32]
//! [--device NAME|ID] [--osc-port PORT] [--capture NAME|--capture-loopback] [--recordings NEW_DIR]
//! [--volume GAIN] [--quality exact|realtime] [--hrir PATH] [--source-ramps]`, or
//! `mr_adm_room_scene_benchmark --list-devices` (formerly `tests/manual/room_scene_benchmark.cpp`,
//! which drove the C++ live renderer, `SceneStreamEngine`, `SceneOutputSession` and
//! `OscHeadTrackingReceiver`). Release builds only; `scripts/room/device-probe.py` drives it.
//!
//! - `kernel` / `kernel-worker`: the room-binaural Scene renderer alone for 1, 16 and 32 sources at
//!   256 / 512-frame calls (the worker size registers the thread as the room worker), static, with
//!   a continuous pose and with a room switch; per-call times, deadline misses and memory.
//! - `latency`: the frames from a pose to the first changed output sample of the renderer.
//! - `stream` / `stream-cycles` / `stream-device`: a Scene stream played by a Scene output on the
//!   null backend or the selected device (measured on its callback), with poses at 100 Hz from
//!   the API or an OSC receiver, one room switch or one every 5 s, optional capture or loopback
//!   recordings, and the device report. Exit code 0 when it ended without failing, underrunning
//!   or losing frames, a switch applied and poses were applied.
//!
//! Every JSON line is one `case`; an error prints `probe_error` on stderr and exits 1.
use mradm_dsp::room::loading::polar;
use mradm_dsp::room::quality::Quality;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, Host, RoomRequest, Screens, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::renderer;
use mradm_engine::scene::stream::{Config, Descriptor, FrameInput, Submit, frame_flag};
use mradm_ffi::api::osc::receiver as osc_receiver;
use mradm_ffi::api::render::FfiHost;
use mradm_ffi::api::room::{RoomPreparedHandle, RoomProfileHandle, switch_config};
use mradm_ffi::api::scene_stream::{EngineSceneOutput, create_stream};
use mradm_json::Value;
use mradm_osc::receiver::State as OscState;
use mradm_render::live::{self, Entry, FrameView, State, Update, field};
use mradm_render::room_runtime::{Config as RoomConfig, state};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[path = "probe/islands.rs"]
mod islands;
mod probe;
use islands::{Capture, OutputTrace, distribution, obj, print};

/// `SceneOutputKind`.
const KIND_STEREO: u32 = 1;
const KIND_NULL: u32 = 2;

fn message(e: &mradm_render::routing::Error) -> String {
    String::from_utf8_lossy(&e.message).into_owned()
}

/// One room of the probe: the package, the HRIR path, the scale and the quality.
#[derive(Clone)]
struct Room {
    profile: Arc<RoomProfileHandle>,
    hrir: String,
    metres: f64,
    quality: Quality,
}

impl Room {
    fn load(path: &str) -> Result<Self, String> {
        let profile = RoomProfileHandle::load(path.as_bytes()).map_err(|e| e.to_string())?;
        Ok(Self {
            profile: Arc::from(profile),
            hrir: String::new(),
            metres: 1.5,
            quality: Quality::Exact,
        })
    }

    /// The package's processing rate (its manifest's `sample_rate`).
    fn rate(&self) -> Result<u32, String> {
        let manifest =
            mradm_json::parse(self.profile.manifest().to_bytes()).map_err(|e| e.to_string())?;
        mradm_json::at(&manifest, "sample_rate")?
            .as_unsigned_u32()
            .ok_or_else(|| "room manifest sample_rate".to_owned())
    }

    fn switch(&self) -> Result<RoomConfig, String> {
        switch_config(
            Some(&*self.profile),
            self.hrir.as_bytes(),
            self.metres,
            self.quality,
        )
        .map_err(|e| message(&e))
    }

    /// `inspect_room`: the preparation's description.
    fn inspect(&self) -> Result<Value, String> {
        let prepared = RoomPreparedHandle::prepare(
            &self.profile,
            self.hrir.as_bytes(),
            b"",
            None,
            self.quality as u32,
            self.metres,
        )
        .map_err(|e| e.to_string())?;
        mradm_json::parse(prepared.description().to_bytes()).map_err(|e| e.to_string())
    }

    fn renderer(&self, sink: bool) -> Result<renderer::Config<'_>, String> {
        Ok(renderer::Config {
            selection: Selection::RoomBinaural,
            layout_id: b"binaural",
            apple_geometry: false,
            speaker_spread: SpeakerSpread::Automatic,
            binaural_spread: BinauralSpreadMode::None,
            split_power: false,
            smoothing_frames: 0,
            sample_rate: self.rate()?,
            sofa: None,
            screens: Screens {
                has_reference: false,
                reference: RoomScreenGeometry::default(),
                has_reproduction: false,
                reproduction: RoomScreenGeometry::default(),
            },
            room: RoomRequest {
                package: Some(self.profile.package()),
                hrir: self.hrir.as_bytes(),
                metres: self.metres,
                quality: self.quality as u32,
            },
            sink,
        })
    }
}

/// `sources` objects on a ring at 1.4 (elevations -15, 0, 15), with deterministic PCM and, with
/// `moving`, one 512-frame position ramp per source and block at its own offset.
struct Fixture {
    descriptors: Vec<Descriptor>,
    states: Vec<Entry>,
    pcm: Vec<Vec<f32>>,
    updates: Vec<Update>,
    frames: u32,
    broadband: bool,
    moving: bool,
    first: bool,
}

fn ring(source: usize, sources: usize, turn: f32) -> Result<[f32; 3], String> {
    polar(
        source as f32 * 360. / sources as f32 + turn,
        (source % 3) as f32 * 15. - 15.,
        1.4,
    )
    .map_err(|_| "position".to_owned())
}

impl Fixture {
    fn new(sources: usize, frames: u32, broadband: bool, moving: bool) -> Result<Self, String> {
        let mut descriptors = Vec::new();
        let mut states = Vec::new();
        for i in 0..sources {
            let position = ring(i, sources, 0.)?;
            descriptors.push(Descriptor {
                element_id: i as u64 + 1,
                role: 0,
                speaker_label: Vec::new(),
                has_position: true,
                position,
                flags: 0,
                identity: None,
            });
            states.push(Entry {
                element_id: i as u64 + 1,
                state: State {
                    valid_fields: field::KNOWN,
                    position,
                    ..State::default()
                },
            });
        }
        Ok(Self {
            descriptors,
            states,
            pcm: vec![vec![0.; frames as usize]; sources],
            updates: Vec::new(),
            frames,
            broadband,
            moving,
            first: true,
        })
    }

    fn update(&mut self, block: u64) -> Result<(), String> {
        self.first = block == 0;
        self.updates.clear();
        let sources = self.pcm.len();
        let frames = self.frames as usize;
        for source in 0..sources {
            if self.moving {
                let position = ring(source, sources, ((block + 1) % 720) as f32 * 0.5)
                    .map_err(|_| "moving position".to_owned())?;
                self.updates.push(Update {
                    element_id: source as u64 + 1,
                    offset: ((source * 131 + 17) % frames) as u32,
                    ramp: self.frames,
                    jump: false,
                    changed: field::POSITION,
                    cleared: 0,
                    state: State {
                        valid_fields: field::POSITION,
                        position,
                        ..State::default()
                    },
                });
            }
            let pcm = &mut self.pcm[source];
            for (i, sample) in pcm.iter_mut().enumerate() {
                let index = block * frames as u64 + i as u64;
                *sample = if self.broadband {
                    // Fixed PCG hash noise avoids ambiguous periodic correlation peaks in device
                    // loopback measurements. No mutable RNG state.
                    let salt = (source as u32).wrapping_mul(3_812_015_801);
                    let state = (index as u32)
                        .wrapping_mul(747_796_405)
                        .wrapping_add(2_891_336_453)
                        ^ salt;
                    let word = ((state >> ((state >> 28) + 4)) ^ state).wrapping_mul(277_803_737);
                    let bits = (word >> 22) ^ word;
                    ((bits >> 8) as f32 - 8_388_608.) / 4_294_967_296.
                } else {
                    let word = (index * 3943 + source as u64 * 7817) % 65521;
                    (word as f32 - 32760.) / 16_777_216.
                };
            }
        }
        self.updates.sort_by_key(|u| u.offset);
        Ok(())
    }

    fn initial(&self) -> &[Entry] {
        if self.first { &self.states } else { &[] }
    }

    fn elements(&self) -> Vec<live::Element<'_>> {
        self.descriptors
            .iter()
            .map(|d| live::Element {
                id: d.element_id,
                role: Some(live::Role::Object),
                speaker_label: b"",
                object_id: b"",
                has_position: true,
                position: d.position,
            })
            .collect()
    }

    fn planes(&self, frames: u32) -> probe::Planes<'_> {
        probe::Planes {
            pcm: &self.pcm,
            first_id: 1,
            frames,
        }
    }

    fn frame<'a>(&'a self, planes: &'a probe::Planes<'a>) -> FrameView<'a, probe::Planes<'a>> {
        FrameView {
            generation: 1,
            duration: self.frames,
            planes,
            initial: self.initial(),
            updates: &self.updates,
            trace: b"",
        }
    }
}

fn create(room: &Room) -> Result<renderer::Created, String> {
    renderer::create(&room.renderer(false)?, &FfiHost).map_err(|e| message(&e))
}

fn kernel(first: &Room, next: &Room, seconds: u32, quantum: u32) -> Result<(), String> {
    FfiHost.room_worker(quantum == 512);
    let rate = first.rate()?;
    for sources in [1usize, 16, 32] {
        for mode in 0..3u32 {
            let mut created = create(first)?;
            let r = &mut created.renderer;
            let mut fixture = Fixture::new(sources, quantum, false, false)?;
            r.configure(1, &fixture.elements())
                .map_err(|e| message(&e))?;
            let mut output = vec![0f32; quantum as usize * 2];
            for block in 0..64 {
                fixture.update(block)?;
                let planes = fixture.planes(quantum);
                r.render(&fixture.frame(&planes), &mut output)
                    .map_err(|e| message(&e))?;
            }
            let blocks = (u64::from(seconds) * u64::from(rate) / u64::from(quantum)) as usize;
            let mut times = vec![0f64; blocks];
            let mut peak_rss = probe::rss();
            let control = r.control().ok_or("room control")?;
            let cpu_start = islands::cpu_seconds();
            let begin = Instant::now();
            for (block, time) in times.iter_mut().enumerate() {
                fixture.update(block as u64 + 64)?;
                if mode > 0 {
                    r.pose([(block % 720) as f32 * 0.25 - 90., 10., 5.]);
                }
                if mode == 2 && block == 96 {
                    control.request(&next.switch()?).map_err(|e| message(&e))?;
                }
                let planes = fixture.planes(quantum);
                let frame = fixture.frame(&planes);
                let tick = Instant::now();
                r.render(&frame, &mut output).map_err(|e| message(&e))?;
                *time = tick.elapsed().as_secs_f64();
                if block % 64 == 0 {
                    peak_rss = peak_rss.max(probe::rss());
                }
            }
            let elapsed = begin.elapsed().as_secs_f64();
            let cpu = islands::cpu_seconds() - cpu_start;
            let audio = blocks as f64 * f64::from(quantum) / f64::from(rate);
            let budget = f64::from(quantum) / f64::from(rate);
            let misses = times.iter().filter(|t| **t > budget).count();
            times.sort_by(f64::total_cmp);
            let status = control.status();
            print(&obj([
                ("case", "kernel".into()),
                ("sources", sources.into()),
                ("call_frames", quantum.into()),
                (
                    "pose_update_hz",
                    if mode > 0 {
                        f64::from(rate) / f64::from(quantum)
                    } else {
                        0.
                    }
                    .into(),
                ),
                ("continuous_pose", (mode > 0).into()),
                ("room_switch", (mode == 2).into()),
                ("audio_seconds", audio.into()),
                ("wall_seconds", elapsed.into()),
                ("cpu_seconds", cpu.into()),
                ("cpu_audio_budget_percent", (100. * cpu / audio).into()),
                ("p99_ms", (times[blocks * 99 / 100] * 1000.).into()),
                ("maximum_ms", (times[blocks - 1] * 1000.).into()),
                ("deadline_misses", misses.into()),
                ("rss_peak_bytes", peak_rss.into()),
                ("applied_revision", status.applied_revision.into()),
                ("final_state", status.state.into()),
            ]));
        }
    }
    FfiHost.room_worker(false);
    Ok(())
}

fn control_latency(room: &Room) -> Result<(), String> {
    let mut reference = create(room)?.renderer;
    let mut changed = create(room)?.renderer;
    let mut fixture = Fixture::new(1, 256, false, false)?;
    reference
        .configure(1, &fixture.elements())
        .map_err(|e| message(&e))?;
    changed
        .configure(1, &fixture.elements())
        .map_err(|e| message(&e))?;
    let (mut a, mut b) = ([0f32; 512], [0f32; 512]);
    for block in 0..256 {
        fixture.update(block)?;
        let planes = fixture.planes(256);
        reference
            .render(&fixture.frame(&planes), &mut a)
            .map_err(|e| message(&e))?;
        changed
            .render(&fixture.frame(&planes), &mut b)
            .map_err(|e| message(&e))?;
    }
    changed.pose([45., 15., 12.]);
    let mut first = None;
    for block in 0..8u64 {
        fixture.update(block + 256)?;
        let planes = fixture.planes(256);
        reference
            .render(&fixture.frame(&planes), &mut a)
            .map_err(|e| message(&e))?;
        changed
            .render(&fixture.frame(&planes), &mut b)
            .map_err(|e| message(&e))?;
        for frame in 0..256usize {
            if first.is_none()
                && (a[frame * 2] != b[frame * 2] || a[frame * 2 + 1] != b[frame * 2 + 1])
            {
                first = Some(block * 256 + frame as u64);
            }
        }
    }
    let first = first.ok_or("head control produced no output change")?;
    print(&obj([
        ("case", "control_latency".into()),
        ("frames_to_first_change", first.into()),
        (
            "milliseconds",
            (first as f64 * 1000. / f64::from(room.rate()?)).into(),
        ),
        (
            "scope",
            "Room PCM only; excludes host queue, sensor and physical device".into(),
        ),
    ]));
    Ok(())
}

#[derive(Default)]
struct Options {
    device_name: String,
    capture_name: String,
    recordings: Option<PathBuf>,
    loopback: bool,
    moving: bool,
    osc_port: i32,
    volume: f32,
}

/// Grows `values` to `count` and back, so later pushes do not allocate.
fn reserve(values: &mut Vec<f64>, count: usize) {
    values.reserve_exact(count);
}

fn create_recordings_dir(dir: &Path) -> Result<(), String> {
    if dir.file_name().is_none() {
        return Err("Recordings directory requires a non-empty name".to_owned());
    }
    if let Some(parent) = dir.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("Cannot create recordings directory: {e}"))?;
    }
    // Create the leaf exclusively so two probes cannot write into the same directory.
    std::fs::create_dir(dir).map_err(|e| {
        if e.kind() == std::io::ErrorKind::AlreadyExists {
            "Recordings directory must not already exist".to_owned()
        } else {
            format!("Cannot create recordings directory: {e}")
        }
    })
}

// Keep preparation, bounded observation and joined teardown together in this manual probe.
#[allow(clippy::too_many_lines)]
fn streaming(
    first: &Room,
    next: &Room,
    seconds: u32,
    cycles: bool,
    sources: usize,
    options: &Options,
) -> Result<bool, String> {
    let renderer = first.renderer(true)?;
    let rate = renderer.sample_rate;
    let config = Config {
        output_sample_rate: rate,
        ..Config::default()
    };
    let stream = create_stream(&config, &renderer).map_err(|e| message(&e))?;
    drop(renderer);
    let prepared_report = first.inspect()?;
    let real_device = !options.device_name.is_empty();
    let selected = if real_device {
        Some(islands::select(&options.device_name)?)
    } else {
        None
    };
    let capture = options.loopback || !options.capture_name.is_empty();
    if capture && (!real_device || options.recordings.is_none()) {
        return Err("Capture requires --device and a new --recordings directory".to_owned());
    }
    if let Some(dir) = &options.recordings {
        create_recordings_dir(dir)?;
    }
    let receiver = if options.osc_port >= 0 {
        let r = osc_receiver(options.osc_port as u16, b"room-probe");
        r.start().map_err(|e| e.message)?;
        Some(r)
    } else {
        None
    };
    let begin = Instant::now();
    let trace = Arc::new(Mutex::new(OutputTrace::new(begin, seconds, capture, rate)));
    let mut tap = if capture {
        let loopback = if options.loopback {
            selected.as_ref().map_or("", |s| s.id.as_str())
        } else {
            ""
        };
        Some(Capture::open(
            begin,
            seconds,
            &options.capture_name,
            loopback,
            rate,
        )?)
    } else {
        None
    };
    let devices = islands::measured_devices(trace.clone());
    let (kind, device_id) = match &selected {
        Some(s) => (KIND_STEREO, s.id.as_str()),
        None => (KIND_NULL, ""),
    };
    // SAFETY: the proxy table forwards to the process-lifetime island table.
    let output =
        unsafe { EngineSceneOutput::open(&stream, kind, b"", 0, device_id.as_bytes(), devices) }
            .map_err(|e| message(&e))?;
    output.set_volume(options.volume).map_err(|e| message(&e))?;
    output.begin_epoch(1, 0).map_err(|e| message(&e))?;
    let stream = stream.stream().clone();
    let mut fixture = Fixture::new(sources, 512, true, options.moving)?;
    stream
        .configure_generation(1, 1, &fixture.descriptors)
        .map_err(|e| message(&e))?;
    let control = stream.room_control().ok_or("room control")?;
    print(&obj([
        ("case", "probe_ready".into()),
        (
            "device",
            selected
                .as_ref()
                .map_or("null".to_owned(), |s| s.name.clone())
                .into(),
        ),
        (
            "osc_port",
            u64::from(receiver.as_ref().map_or(0, |r| r.snapshot().bound_port)).into(),
        ),
        ("seconds", seconds.into()),
        ("sample_rate", rate.into()),
        ("room_prepared", prepared_report.clone()),
        (
            "quality",
            if first.quality == Quality::Realtime {
                "realtime"
            } else {
                "exact"
            }
            .into(),
        ),
        ("source_ramps", options.moving.into()),
        ("volume", options.volume.into()),
    ]));
    let cpu_start = islands::cpu_seconds();
    let tracking_failed = AtomicBool::new(false);
    let material_ended = AtomicBool::new(false);
    let stop = AtomicBool::new(false);
    let mut control_age_ms = Vec::new();
    let mut control_call_ms = Vec::new();
    reserve(&mut control_age_ms, (seconds as usize + 10) * 110);
    reserve(&mut control_call_ms, (seconds as usize + 10) * 110);
    let first_switch = first.switch()?;
    let next_switch = next.switch()?;
    let frames = u64::from(seconds) * u64::from(rate);
    let (requested, poses_applied, tracking_error, observed) = std::thread::scope(|scope| {
        let tracking = scope.spawn(|| -> (u64, u64, String) {
            let (mut requested, mut applied) = (0u64, 0u64);
            let result = (|| -> Result<(), String> {
                let origin = Instant::now();
                let interval = if receiver.is_some() {
                    Duration::from_millis(1)
                } else {
                    Duration::from_millis(10)
                };
                let (mut tick, mut sequence) = (0u32, 0u64);
                while !stop.load(Ordering::Acquire) {
                    let time = begin.elapsed().as_secs_f64();
                    let mut apply = true;
                    let mut pose = [
                        ((time * 0.25).sin() * 90.) as f32,
                        ((time * 0.7).sin() * 20.) as f32,
                        ((time * 0.4).sin() * 10.) as f32,
                    ];
                    if let Some(r) = &receiver {
                        let snapshot = r.snapshot();
                        if snapshot.state == OscState::Failed {
                            return Err(r.last_error());
                        }
                        apply = snapshot.fresh && snapshot.sequence != sequence;
                        if apply {
                            sequence = snapshot.sequence;
                            pose = snapshot.euler_deg;
                            control_age_ms.push(snapshot.age_ms as f64);
                        }
                    }
                    if apply {
                        let start = Instant::now();
                        stream.set_listener_orientation(pose);
                        control_call_ms.push(start.elapsed().as_secs_f64() * 1000.);
                        applied += 1;
                    }
                    if !material_ended.load(Ordering::Acquire)
                        && (cycles || requested == 0)
                        && time >= 2. + requested as f64 * 5.
                    {
                        let room = if requested % 2 == 0 {
                            &next_switch
                        } else {
                            &first_switch
                        };
                        match control.request(room) {
                            Ok(_) => requested += 1,
                            Err(e) if !material_ended.load(Ordering::Acquire) => {
                                return Err(message(&e));
                            }
                            Err(_) => {}
                        }
                    }
                    let status = control.status();
                    if status.state == state::FAILED {
                        return Err(status.message);
                    }
                    tick += 1;
                    islands::wait_until(origin + interval * tick);
                }
                Ok(())
            })();
            let error = match result {
                Ok(()) => String::new(),
                Err(e) => {
                    tracking_failed.store(true, Ordering::Release);
                    e
                }
            };
            (requested, applied, error)
        });
        let observed = (|| -> Result<_, String> {
            let mut peak_rss = probe::rss();
            let mut last_underruns = 0u64;
            let mut underrun_events = Vec::new();
            let mut resident = Vec::with_capacity(seconds as usize + 10);
            let mut queued_ms = Vec::with_capacity((seconds as usize + 10) * 2);
            let mut next_observation = Instant::now();
            let (mut sample, mut block) = (0u64, 0u64);
            while sample < frames {
                if tracking_failed.load(Ordering::Acquire) {
                    break;
                }
                fixture.update(block)?;
                let duration = (frames - sample).min(512) as u32;
                let planes = fixture.planes(duration);
                // The last material slice can be shorter than a canonical producer frame.
                let keep = fixture
                    .updates
                    .iter()
                    .rposition(|u| u.offset < duration)
                    .map_or(0, |i| i + 1);
                let frame = FrameInput {
                    epoch: 1,
                    generation: 1,
                    start: sample as i64,
                    duration,
                    flags: frame_flag::STATE_COMPLETE,
                    planes: &planes,
                    initial: fixture.initial(),
                    updates: &fixture.updates[..keep],
                };
                if stream.submit_frame(&frame, 5000).map_err(|e| message(&e))? != Submit::Accepted {
                    return Err("producer timeout".to_owned());
                }
                sample += u64::from(duration);
                if block == 4 {
                    output.play();
                }
                let current = output.status();
                if current.failed != 0 || current.ended != 0 {
                    return Err("Unexpected device/Scene failure or premature EOS".to_owned());
                }
                if current.underruns != last_underruns {
                    last_underruns = current.underruns;
                    underrun_events.push(obj([
                        ("seconds", begin.elapsed().as_secs_f64().into()),
                        ("count", last_underruns.into()),
                        ("room_state", control.status().state.into()),
                    ]));
                }
                if Instant::now() >= next_observation {
                    let time = begin.elapsed().as_secs_f64();
                    let rss = probe::rss();
                    peak_rss = peak_rss.max(rss);
                    resident.push(obj([("seconds", time.into()), ("rss_bytes", rss.into())]));
                    queued_ms.push(1000. * current.queued_frames as f64 / f64::from(rate));
                    print(&obj([
                        ("case", "device_progress".into()),
                        ("seconds", time.into()),
                        ("rss_bytes", rss.into()),
                        ("presented_frames", current.presented_frames.into()),
                        ("underruns", current.underruns.into()),
                    ]));
                    next_observation += Duration::from_secs(1);
                }
                block += 1;
            }
            Ok((peak_rss, underrun_events, resident, queued_ms))
        })();
        // Continue tracking throughout the tail, but do not request a new room after EOS.
        material_ended.store(true, Ordering::Release);
        let ended = observed.and_then(|o| {
            stream
                .signal_end(1, frames as i64)
                .map_err(|e| message(&e))?;
            let limit = Instant::now() + Duration::from_secs(8);
            loop {
                let s = output.status();
                if s.ended != 0 || s.failed != 0 || Instant::now() >= limit {
                    break;
                }
                std::thread::sleep(Duration::from_millis(10));
            }
            Ok(o)
        });
        stop.store(true, Ordering::Release);
        let (requested, applied, error) = tracking.join().unwrap_or_default();
        (requested, applied, error, ended)
    });
    let (peak_rss, underrun_events, resident, queued_ms) = observed?;
    let status = output.status();
    let room = control.status();
    let osc = match &receiver {
        Some(r) => mradm_json::parse(r.snapshot_json()?.as_bytes()).map_err(|e| e.to_string())?,
        None => Value::Null,
    };
    // Stop both callbacks before reading their recordings.
    drop(output);
    if let Some(tap) = &mut tap {
        tap.stop();
    }
    let elapsed = begin.elapsed().as_secs_f64();
    let failed = status.failed != 0 || tracking_failed.load(Ordering::Acquire);
    let trace = trace.lock().unwrap_or_else(|e| e.into_inner());
    let mut report = obj([
        (
            "case",
            if real_device {
                "Scene_streaming_device"
            } else {
                "Scene_streaming_null_device"
            }
            .into(),
        ),
        (
            "device",
            selected.as_ref().map_or("null".to_owned(), |s| s.name.clone()).into(),
        ),
        ("sources", sources.into()),
        (
            "quality",
            if first.quality == Quality::Realtime {
                "realtime"
            } else {
                "exact"
            }
            .into(),
        ),
        ("source_ramps", options.moving.into()),
        ("requested_seconds", seconds.into()),
        ("sample_rate", rate.into()),
        ("room_prepared", prepared_report),
        (
            "control_transport",
            if receiver.is_some() {
                "OSC loopback UDP"
            } else {
                "direct API"
            }
            .into(),
        ),
        ("nominal_head_update_hz", 100u64.into()),
        ("poses_applied", poses_applied.into()),
        ("room_switch_requests", requested.into()),
        ("repeated_room_switches", cycles.into()),
        (
            "cpu_percent",
            (100. * (islands::cpu_seconds() - cpu_start) / elapsed).into(),
        ),
        ("wall_seconds", elapsed.into()),
        ("rss_peak_bytes", peak_rss.into()),
        ("memory_samples", Value::Array(resident)),
        ("presented_frames", status.presented_frames.into()),
        ("consumed_frames", status.consumed_frames.into()),
        ("expected_frames", (frames + room.tail_frames).into()),
        ("underruns", status.underruns.into()),
        ("underrun_events", Value::Array(underrun_events)),
        ("ended", (status.ended != 0).into()),
        ("failed", failed.into()),
        ("control_error", tracking_error.into()),
        ("applied_revision", room.applied_revision.into()),
        ("osc", osc),
        ("volume", options.volume.into()),
        ("callback_interval", trace.intervals.report()),
        ("callback_pull_duration", trace.pull_duration.report()),
        ("control_call_duration_ms", distribution(control_call_ms)),
        ("osc_receive_to_apply_ms", distribution(control_age_ms)),
        ("osc_age_quantization_ms", 1u64.into()),
        ("host_queued_audio_ms", distribution(queued_ms)),
        (
            "clock_scope",
            "presented_frames is callback consumption, not a physical DAC clock".into(),
        ),
        (
            "latency_scope",
            "OSC receiver snapshot age to API apply and host queue depth; excludes sensor, kernel response, driver and headphone"
                .into(),
        ),
    ]);
    if let (Some(tap), Some(dir), Value::Object(map)) = (&tap, &options.recordings, &mut report) {
        map.insert(
            "output_recording".to_owned(),
            trace.recording.save(dir, "output")?,
        );
        map.insert("input_recording".to_owned(), tap.save(dir)?);
        map.insert(
            "recording_scope".to_owned(),
            "output callback PCM to selected input callback; excludes sensor and pre-callback processing"
                .into(),
        );
        std::fs::write(
            dir.join("recordings.json"),
            format!("{}\n", mradm_json::dump(&report, Some(2))),
        )
        .map_err(|_| "Cannot save capture metadata".to_owned())?;
    }
    print(&report);
    Ok(status.ended != 0
        && !failed
        && status.underruns == 0
        && status.presented_frames == frames + room.tail_frames
        && room.applied_revision != 0
        && poses_applied != 0)
}

fn unsigned(text: &str) -> Result<u32, String> {
    // `std::stoul`: leading digits after optional whitespace.
    let digits: String = text
        .trim_start()
        .chars()
        .take_while(char::is_ascii_digit)
        .collect();
    digits
        .parse()
        .map_err(|_| format!("invalid number: {text}"))
}

fn run() -> Result<bool, String> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() == 2 && args[1] == "--list-devices" {
        let outputs: Vec<Value> = islands::outputs()
            .into_iter()
            .map(|o| {
                obj([
                    ("id", o.id.into()),
                    ("name", o.name.into()),
                    ("default", o.default.into()),
                ])
            })
            .collect();
        let report = obj([
            ("outputs", Value::Array(outputs)),
            ("inputs", Capture::inputs()?),
        ]);
        println!("{}", mradm_json::dump(&report, Some(2)));
        return Ok(true);
    }
    if args.len() < 3 {
        return Err("usage: room_scene_benchmark ROOM_JSON OTHER_ROOM_JSON [SECONDS=10] \
            [all|stream|stream-cycles|stream-device|kernel|kernel-worker|latency] [SOURCES=32] [--device NAME|ID] \
            [--osc-port PORT] [--capture NAME|--capture-loopback] [--recordings NEW_DIR] [--volume GAIN] \
            [--quality exact|realtime] [--hrir PATH] [--source-ramps]"
            .to_owned());
    }
    let seconds = args.get(3).map_or(Ok(10), |a| unsigned(a))?;
    if !(5..=3600).contains(&seconds) {
        return Err("seconds must be 5..3600".to_owned());
    }
    let mut first = Room::load(&args[1])?;
    let mut next = Room::load(&args[2])?;
    let mode = args.get(4).map_or("all", String::as_str);
    let sources = args.get(5).map_or(Ok(32), |a| unsigned(a))? as usize;
    if ![1, 16, 32].contains(&sources) {
        return Err("stream sources must be 1, 16 or 32".to_owned());
    }
    let mut options = Options {
        osc_port: -1,
        volume: 0.1,
        ..Options::default()
    };
    let mut i = 6;
    while i < args.len() {
        let flag = args[i].as_str();
        if flag == "--capture-loopback" {
            options.loopback = true;
            i += 1;
            continue;
        }
        if flag == "--source-ramps" {
            options.moving = true;
            i += 1;
            continue;
        }
        i += 1;
        let value = args.get(i).ok_or("Missing probe option value")?;
        match flag {
            "--device" => options.device_name = value.clone(),
            "--capture" => options.capture_name = value.clone(),
            "--recordings" => options.recordings = Some(PathBuf::from(value)),
            "--volume" => {
                options.volume = value
                    .trim()
                    .parse()
                    .map_err(|_| format!("invalid volume: {value}"))?;
            }
            "--osc-port" => {
                options.osc_port = value
                    .trim()
                    .parse()
                    .map_err(|_| format!("invalid port: {value}"))?;
            }
            "--hrir" => {
                first.hrir = value.clone();
                next.hrir = value.clone();
            }
            "--quality" => {
                let quality = match value.as_str() {
                    "exact" => Quality::Exact,
                    "realtime" => Quality::Realtime,
                    _ => return Err("quality must be exact or realtime".to_owned()),
                };
                first.quality = quality;
                next.quality = quality;
            }
            _ => return Err("Unknown probe option".to_owned()),
        }
        i += 1;
    }
    if !options.volume.is_finite()
        || options.volume <= 0.
        || options.volume > 1.
        || options.osc_port < -1
        || options.osc_port > 65535
        || (options.loopback && !options.capture_name.is_empty())
        || (mode == "stream-device" && options.device_name.is_empty())
    {
        return Err("Invalid device probe options".to_owned());
    }
    if matches!(mode, "all" | "kernel" | "kernel-worker") {
        kernel(
            &first,
            &next,
            seconds,
            if mode == "kernel-worker" { 512 } else { 256 },
        )?;
    }
    if matches!(mode, "all" | "latency") {
        control_latency(&first)?;
    }
    if matches!(mode, "all" | "stream" | "stream-cycles" | "stream-device") {
        return streaming(
            &first,
            &next,
            seconds,
            matches!(mode, "stream-cycles" | "stream-device"),
            sources,
            &options,
        );
    }
    if !matches!(mode, "kernel" | "kernel-worker" | "latency") {
        return Err("unknown benchmark mode".to_owned());
    }
    Ok(true)
}

fn main() -> ExitCode {
    if cfg!(debug_assertions) {
        eprintln!("Use a Release build.");
        return ExitCode::from(2);
    }
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(1),
        Err(e) => {
            eprintln!(
                "{}",
                mradm_json::dump(
                    &obj([
                        ("case", "probe_error".into()),
                        ("failed", true.into()),
                        ("error", e.into()),
                    ]),
                    None
                )
            );
            ExitCode::from(1)
        }
    }
}
