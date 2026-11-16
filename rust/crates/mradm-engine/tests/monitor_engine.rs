//! The realtime monitor engine (PCM consistency 5.5 A) driven by mock streams and a host: playback,
//! loops (also through an output stage), pauses, seeks with their 10 ms bridge and the deferred
//! device flush, levels and LUFS, HpTF, live edits and their revisions, backend switches with their
//! exact crossfade, interrupted and short crossfades, warmups and cancellable pre-rolls, failures
//! and the end of stream. The device is a test sink that pulls what the ring holds, as the C++
//! `mr_adm_realtime_tests` did before R3 D1 moved them here.
use mradm_dsp::hptf::{Coefficients, PreampMode, Snapshot};
use mradm_engine::monitor::{
    Cancel, CreateError, Monitor, MonitorHost, ObjectOverride, OutputStage, Overrides, Receipt,
    Source, State, Validate,
};
use mradm_render::routing::Error;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const RATE: u32 = 48000;
const CHANNELS: usize = 2;

#[derive(Default)]
struct Probe {
    revision_set: AtomicU64,
    revision_at_seek: AtomicU64,
    processing: AtomicBool,
    hold_process: AtomicBool,
    fail_process: AtomicBool,
    seek_started: AtomicBool,
    release_seek: AtomicBool,
    fail_seek: AtomicBool,
    seek_cancelled: AtomicUsize,
}

/// Frame `p` carries `offset + p / 1e6` on every channel; `length` ends the stream.
struct Ramp {
    probe: Arc<Probe>,
    offset: f32,
    length: Option<u64>,
    position: u64,
    preroll: bool,
    /// A seek to this frame never pre-rolls (the C++ `SlowSeekStream`'s frame 7).
    instant: Option<u64>,
    channels: usize,
}

impl Ramp {
    fn new(probe: &Arc<Probe>) -> Self {
        Self {
            probe: probe.clone(),
            offset: 0.,
            length: None,
            position: 0,
            preroll: false,
            instant: None,
            channels: CHANNELS,
        }
    }
}

fn value(offset: f32, frame: u64) -> f32 {
    offset + frame as f32 / 1e6
}

struct Rejecting;

impl Validate for Rejecting {
    fn validate(&self, overrides: &Overrides) -> Result<(), Error> {
        if overrides.revision == 99 {
            return Err(Error {
                code: 3,
                message: b"test invalid edit".to_vec(),
                context: b"probe".to_vec(),
            });
        }
        Ok(())
    }
}

impl Source for Ramp {
    fn out_channels(&self) -> usize {
        self.channels
    }
    fn sample_rate(&self) -> u32 {
        RATE
    }
    fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        self.probe.processing.store(true, Ordering::SeqCst);
        while self.probe.hold_process.load(Ordering::SeqCst) {
            std::thread::sleep(Duration::from_millis(1));
        }
        if self.probe.fail_process.load(Ordering::SeqCst) {
            return Err(Error {
                code: 4,
                message: b"test process failure".to_vec(),
                context: Vec::new(),
            });
        }
        let left = self
            .length
            .map_or(frames as u64, |n| n.saturating_sub(self.position));
        let got = (frames as u64).min(left) as usize;
        for f in 0..got {
            let v = value(self.offset, self.position + f as u64);
            output[f * self.channels..(f + 1) * self.channels].fill(v);
        }
        self.position += got as u64;
        Ok(got)
    }
    fn seek(&mut self, frame: u64) -> Result<(), Error> {
        let revision = self.probe.revision_set.load(Ordering::SeqCst);
        self.probe
            .revision_at_seek
            .store(revision, Ordering::SeqCst);
        self.position = frame;
        Ok(())
    }
    fn seek_with_cancel(&mut self, frame: u64, cancel: &Cancel) -> Result<(), Error> {
        if self.preroll && self.instant != Some(frame) {
            self.probe.seek_started.store(true, Ordering::SeqCst);
            while !self.probe.release_seek.load(Ordering::SeqCst) {
                if cancel.requested() {
                    self.probe.seek_cancelled.fetch_add(1, Ordering::SeqCst);
                    return Err(Error {
                        code: 5,
                        message: b"test pre-roll cancelled".to_vec(),
                        context: Vec::new(),
                    });
                }
                std::thread::sleep(Duration::from_millis(1));
            }
            if self.probe.fail_seek.load(Ordering::SeqCst) {
                return Err(Error {
                    code: 4,
                    message: b"test incoming seek failure".to_vec(),
                    context: Vec::new(),
                });
            }
        }
        self.seek(frame)
    }
    fn seek_requires_preroll(&self) -> bool {
        self.preroll
    }
    fn validator(&self) -> Arc<dyn Validate> {
        Arc::new(Rejecting)
    }
    fn set_overrides(&mut self, overrides: &Overrides) {
        self.probe
            .revision_set
            .store(overrides.revision, Ordering::SeqCst);
    }
    fn set_orientation(&mut self, _pose: [f32; 3]) {}
}

#[derive(Default)]
struct Host {
    logs: Mutex<Vec<(u32, String, String)>>,
    flushes: AtomicUsize,
    resumes: AtomicUsize,
}

impl MonitorHost for Host {
    fn log(&self, level: u32, module: &str, message: &[u8]) {
        self.logs.lock().unwrap().push((
            level,
            module.into(),
            String::from_utf8_lossy(message).into_owned(),
        ));
    }
    fn device_flush(&self) {
        self.flushes.fetch_add(1, Ordering::SeqCst);
    }
    fn device_pause(&self) {}
    fn device_resume(&self) {
        self.resumes.fetch_add(1, Ordering::SeqCst);
    }
}

fn monitor(source: Ramp, realtime: bool) -> (Monitor, Arc<Host>) {
    let host = Arc::new(Host::default());
    let Ok(m) = Monitor::create(Box::new(source), realtime, host.clone()) else {
        panic!("create");
    };
    (m, host)
}

fn eventually(mut predicate: impl FnMut() -> bool) -> bool {
    let end = Instant::now() + Duration::from_secs(5);
    while !predicate() {
        if Instant::now() > end {
            return false;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    true
}

/// Pulls `frames` produced frames (non-realtime pulls return what the ring held).
fn drain(m: &Monitor, frames: usize) -> Vec<f32> {
    let mut pcm = Vec::with_capacity(frames * CHANNELS);
    let mut block = vec![0f32; 256 * CHANNELS];
    let end = Instant::now() + Duration::from_secs(5);
    while pcm.len() < frames * CHANNELS {
        assert!(Instant::now() < end, "drain timed out at {}", pcm.len());
        let want = (frames - pcm.len() / CHANNELS).min(256);
        let got = m.pull(&mut block, want).frames;
        pcm.extend_from_slice(&block[..got * CHANNELS]);
        if got == 0 {
            std::thread::sleep(Duration::from_millis(1));
        }
    }
    pcm
}

fn frame(pcm: &[f32], i: usize) -> f32 {
    assert_eq!(pcm[i * CHANNELS], pcm[i * CHANNELS + 1]);
    pcm[i * CHANNELS]
}

#[test]
fn invalid_formats_are_rejected() {
    let probe = Arc::new(Probe::default());
    let mut source = Ramp::new(&probe);
    source.channels = 0;
    let host = Arc::new(Host::default());
    match Monitor::create(Box::new(source), true, host) {
        Err(CreateError::Error(e)) => {
            assert_eq!(e.code, 6);
            assert_eq!(
                e.message,
                b"render stream reports an invalid format (0 channels or 0 sample rate)"
            );
        }
        _ => panic!("expected the format error"),
    }
}

#[test]
fn playback_is_continuous_and_counts_the_playhead() {
    let probe = Arc::new(Probe::default());
    let (m, _host) = monitor(Ramp::new(&probe), false);
    assert_eq!(m.status().state, State::Stopped);
    m.play();
    let pcm = drain(&m, 5000);
    for i in 0..5000 {
        assert_eq!(frame(&pcm, i), value(0., i as u64), "frame {i}");
    }
    let status = m.status();
    assert_eq!(status.state, State::Playing);
    assert_eq!(status.playhead_frames, 5000);
    assert!(!status.failed && !status.ended);
}

#[test]
fn loops_wrap_to_their_start() {
    let probe = Arc::new(Probe::default());
    let (m, _host) = monitor(Ramp::new(&probe), false);
    m.set_loop(100, 1100);
    m.play();
    let pcm = drain(&m, 3300);
    for i in 0..3300 {
        let expected = if i < 1100 { i } else { 100 + (i - 1100) % 1000 };
        assert_eq!(frame(&pcm, i), value(0., expected as u64), "frame {i}");
    }
}

#[test]
fn paused_seeks_defer_the_device_flush_until_play() {
    let probe = Arc::new(Probe::default());
    let (m, host) = monitor(Ramp::new(&probe), false);
    m.pause();
    m.seek(500);
    assert!(eventually(|| m.status().playhead_frames == 500));
    assert_eq!(host.flushes.load(Ordering::SeqCst), 0);
    m.play();
    assert_eq!(host.flushes.load(Ordering::SeqCst), 1);
    m.seek(9000);
    assert!(eventually(|| m.status().playhead_frames == 9000));
    assert!(eventually(|| host.flushes.load(Ordering::SeqCst) == 2));
    // After the 10 ms bridge the output follows the stream from the target.
    let pcm = drain(&m, 2000);
    for i in 1000..2000 {
        assert_eq!(frame(&pcm, i), value(0., 9000 + i as u64), "frame {i}");
    }
}

#[test]
fn live_edits_are_checked_and_acknowledged_by_revision() {
    let probe = Arc::new(Probe::default());
    let (m, _host) = monitor(Ramp::new(&probe), false);
    let bad = Overrides {
        objects: vec![ObjectOverride {
            object_id: b"AO_1001".to_vec(),
            gain_db: f32::NAN,
            ..ObjectOverride::default()
        }],
        revision: 5,
    };
    let e = m.set_overrides(bad).unwrap_err();
    assert_eq!(e.code, 1);
    assert_eq!(
        String::from_utf8(e.message).unwrap(),
        "实时增益必须为有限值,且未静音时转换为线性增益不得溢出"
    );
    assert_eq!(e.context, b"AO_1001");
    // A muted edit may carry a gain that would overflow.
    let muted = Overrides {
        objects: vec![ObjectOverride {
            gain_db: 1e9,
            mute: true,
            ..ObjectOverride::default()
        }],
        revision: 6,
    };
    m.set_overrides(muted).unwrap();
    assert!(eventually(|| m.status().override_revision == 6));
    let rejected = Overrides {
        revision: 99,
        ..Overrides::default()
    };
    let e = m.set_overrides(rejected).unwrap_err();
    assert_eq!(
        (e.code, e.message.as_slice()),
        (3, &b"test invalid edit"[..])
    );
    std::thread::sleep(Duration::from_millis(20));
    assert_eq!(m.status().override_revision, 6);
    assert_eq!(probe.revision_set.load(Ordering::SeqCst), 6);
    m.set_overrides(Overrides {
        revision: 7,
        ..Overrides::default()
    })
    .unwrap();
    assert!(eventually(|| m.status().override_revision == 7));
    assert_eq!(probe.revision_set.load(Ordering::SeqCst), 7);
}

#[test]
fn switches_reapply_the_current_edits_and_crossfade() {
    let probe = Arc::new(Probe::default());
    let (m, _host) = monitor(Ramp::new(&probe), false);
    m.play();
    drain(&m, 3000);
    m.set_overrides(Overrides {
        revision: 42,
        ..Overrides::default()
    })
    .unwrap();
    assert!(eventually(|| m.status().override_revision == 42));
    let incoming = Arc::new(Probe::default());
    let mut next = Ramp::new(&incoming);
    next.offset = 0.5;
    let receipt = Arc::new(Receipt::default());
    m.switch(Box::new(next), Some(receipt.clone()));
    assert!(eventually(|| receipt.state() == Receipt::APPLIED));
    assert_eq!(incoming.revision_set.load(Ordering::SeqCst), 42);
    // The edits reach the incoming stream before it seeks to the switch position.
    assert_eq!(incoming.revision_at_seek.load(Ordering::SeqCst), 42);
    let pcm = drain(&m, 40000);
    let last = frame(&pcm, 39999);
    assert!(last > 0.5, "the incoming stream reaches the output: {last}");
}

#[test]
fn failed_warmups_reject_and_keep_the_active_stream() {
    let probe = Arc::new(Probe::default());
    let (m, host) = monitor(Ramp::new(&probe), false);
    m.play();
    drain(&m, 2000);
    let incoming = Arc::new(Probe::default());
    let mut next = Ramp::new(&incoming);
    next.preroll = true;
    next.offset = 0.5;
    let receipt = Arc::new(Receipt::default());
    m.switch(Box::new(next), Some(receipt.clone()));
    assert!(eventually(|| incoming.seek_started.load(Ordering::SeqCst)));
    // The old stream keeps producing while the incoming one warms up.
    let pcm = drain(&m, 20000);
    assert!(frame(&pcm, 19999) < 0.5);
    incoming.fail_seek.store(true, Ordering::SeqCst);
    incoming.release_seek.store(true, Ordering::SeqCst);
    assert!(eventually(|| receipt.state() == Receipt::REJECTED));
    let pcm = drain(&m, 2000);
    assert!(frame(&pcm, 1999) < 0.5);
    assert!(!m.status().failed);
    let logs = host.logs.lock().unwrap();
    assert!(
        logs.iter().any(|(l, module, text)| *l == 3
            && module == "monitor"
            && text == "test incoming seek failure"),
        "{logs:?}"
    );
}

#[test]
fn a_seek_cancels_a_warmup() {
    let probe = Arc::new(Probe::default());
    let (m, _host) = monitor(Ramp::new(&probe), false);
    let incoming = Arc::new(Probe::default());
    let mut next = Ramp::new(&incoming);
    next.preroll = true;
    m.switch(Box::new(next), Some(Arc::new(Receipt::default())));
    assert!(eventually(|| incoming.seek_started.load(Ordering::SeqCst)));
    m.seek(7);
    assert!(eventually(|| incoming
        .seek_cancelled
        .load(Ordering::SeqCst)
        > 0));
    assert!(eventually(|| m.status().playhead_frames == 7));
    assert!(!m.status().failed);
}

#[test]
fn failures_are_logged_before_they_are_visible() {
    let probe = Arc::new(Probe::default());
    probe.fail_process.store(true, Ordering::SeqCst);
    let (m, host) = monitor(Ramp::new(&probe), false);
    m.play();
    assert!(eventually(|| m.status().failed));
    let logs = host.logs.lock().unwrap();
    assert_eq!(
        logs.as_slice(),
        &[(3, "monitor".into(), "test process failure".into())]
    );
}

#[test]
fn underruns_count_only_while_the_producer_should_feed() {
    let probe = Arc::new(Probe::default());
    probe.hold_process.store(true, Ordering::SeqCst);
    let mut source = Ramp::new(&probe);
    source.length = Some(3000);
    let (m, _host) = monitor(source, true);
    m.play();
    assert!(eventually(|| probe.processing.load(Ordering::SeqCst)));
    let mut block = vec![1f32; 512 * CHANNELS];
    let pulled = m.pull(&mut block, 512);
    assert_eq!(pulled.frames, 0);
    assert!(block.iter().all(|&v| v == 0.));
    assert_eq!(m.status().underruns, 1);
    probe.hold_process.store(false, Ordering::SeqCst);
    assert!(eventually(
        || m.status().buffered_frames == 3000 || m.status().ended
    ));
    let mut end = false;
    let deadline = Instant::now() + Duration::from_secs(5);
    while !end {
        assert!(Instant::now() < deadline);
        end = m.pull(&mut block, 512).mark_end;
    }
    let underruns = m.status().underruns;
    for _ in 0..4 {
        assert!(m.pull(&mut block, 512).mark_end);
    }
    assert_eq!(m.status().underruns, underruns);
    assert!(m.status().ended);
    assert_eq!(m.status().playhead_frames, 3000);
}

// ── R3 D1: the remaining checks of the C++ `realtime_stream_fixture_test` ─────────────────────

/// What a [`Fill`] source writes on every frame.
#[derive(Clone, Copy)]
enum Kind {
    /// Frame `p`, channel `c`: `p * 0.001 + c` (the C++ `PatternStream`).
    Pattern,
    Const(f32),
    /// 1.0 scaled by the live edit of object `obj0` (the C++ `GainStream`).
    Gain,
    /// A 1 kHz sine at 0.5 (the C++ `SineStream`).
    Sine,
    /// The sine scaled like [`Kind::Gain`].
    GainSine,
}

fn pattern(frame: u64, channel: usize) -> f32 {
    frame as f32 * 0.001 + channel as f32
}

struct Fill {
    kind: Kind,
    position: u64,
    /// Frames left before the end of stream; `rewind` restores it on a seek.
    remaining: Option<u64>,
    rewind: Option<u64>,
    gain: f32,
}

impl Fill {
    fn new(kind: Kind) -> Self {
        Self {
            kind,
            position: 0,
            remaining: None,
            rewind: None,
            gain: 1.,
        }
    }
    fn ending(mut self, frames: u64) -> Self {
        self.remaining = Some(frames);
        self
    }
}

impl Source for Fill {
    fn out_channels(&self) -> usize {
        CHANNELS
    }
    fn sample_rate(&self) -> u32 {
        RATE
    }
    fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        let got = self.remaining.map_or(frames, |n| frames.min(n as usize));
        const W: f64 = std::f64::consts::TAU * 1000.0 / 48000.0;
        for f in 0..got {
            let p = self.position + f as u64;
            for c in 0..CHANNELS {
                output[f * CHANNELS + c] = match self.kind {
                    Kind::Pattern => pattern(p, c),
                    Kind::Const(v) => v,
                    Kind::Gain => self.gain,
                    Kind::Sine => (0.5 * (W * p as f64).sin()) as f32,
                    Kind::GainSine => self.gain * (0.5 * (W * p as f64).sin()) as f32,
                };
            }
        }
        self.position += got as u64;
        if let Some(n) = &mut self.remaining {
            *n -= got as u64;
        }
        Ok(got)
    }
    fn seek(&mut self, frame: u64) -> Result<(), Error> {
        self.position = frame;
        if self.rewind.is_some() {
            self.remaining = self.rewind;
        }
        Ok(())
    }
    fn seek_with_cancel(&mut self, frame: u64, _cancel: &Cancel) -> Result<(), Error> {
        self.seek(frame)
    }
    fn seek_requires_preroll(&self) -> bool {
        false
    }
    fn validator(&self) -> Arc<dyn Validate> {
        Arc::new(Rejecting)
    }
    fn set_overrides(&mut self, overrides: &Overrides) {
        self.gain = 1.;
        for o in overrides.objects.iter().filter(|o| o.object_id == b"obj0") {
            self.gain = if o.mute {
                0.
            } else {
                10f32.powf(o.gain_db / 20.)
            };
        }
    }
    fn set_orientation(&mut self, _pose: [f32; 3]) {}
}

/// A device that pulls exactly what the test asks for and keeps every pulled frame (the C++
/// `ManualSink`).
struct Sink<'a> {
    monitor: &'a Monitor,
    pcm: Vec<f32>,
}

impl<'a> Sink<'a> {
    fn new(monitor: &'a Monitor) -> Self {
        Self {
            monitor,
            pcm: Vec::new(),
        }
    }
    fn pump(&mut self, frames: usize) {
        let ch = self.monitor.channels();
        let mut block = vec![0f32; frames * ch];
        self.monitor.pull(&mut block, frames);
        self.pcm.extend_from_slice(&block);
    }
    /// Pulls `total` frames, each pull no larger than what the ring holds.
    fn drain(&mut self, total: usize) {
        let mut done = 0;
        while done < total {
            assert!(
                eventually(|| self.monitor.status().buffered_frames > 0),
                "producer stalled after {done} frames"
            );
            let want = (total - done).min(self.monitor.status().buffered_frames as usize);
            self.pump(want);
            done += want;
        }
    }
    fn frames(&self) -> usize {
        self.pcm.len() / self.monitor.channels()
    }
    fn last(&self) -> f32 {
        *self.pcm.last().unwrap()
    }
}

fn near(a: f32, b: f32) -> bool {
    (a - b).abs() < 1e-4
}

fn fill(kind: Kind) -> (Monitor, Arc<Host>) {
    create(Fill::new(kind))
}

fn create(source: impl Source + 'static) -> (Monitor, Arc<Host>) {
    let host = Arc::new(Host::default());
    let Ok(m) = Monitor::create(Box::new(source), true, host.clone()) else {
        panic!("create");
    };
    (m, host)
}

fn gain_edit(object: &str, gain_db: f32, revision: u64, mute: bool) -> Overrides {
    Overrides {
        objects: vec![ObjectOverride {
            object_id: object.as_bytes().to_vec(),
            gain_db,
            mute,
            ..ObjectOverride::default()
        }],
        revision,
    }
}

#[test]
fn levels_cover_each_whole_callback_and_a_zero_pull_clears_them() {
    for realtime in [false, true] {
        let host = Arc::new(Host::default());
        let Ok(m) = Monitor::create(
            Box::new(Fill::new(Kind::Pattern).ending(37)),
            realtime,
            host,
        ) else {
            panic!("create");
        };
        m.play();
        assert!(eventually(|| m.status().ended));
        let mut sink = Sink::new(&m);
        sink.pump(64);
        let levels = m.levels();
        for c in 0..CHANNELS {
            let samples = (0..64).map(|f| sink.pcm[f * CHANNELS + c]);
            let peak = samples.clone().fold(0f32, |p, v| p.max(v.abs()));
            let sum: f64 = samples.map(|v| f64::from(v) * f64::from(v)).sum();
            assert_eq!(levels.peak[c], peak, "realtime={realtime} channel {c}");
            assert_eq!(
                levels.rms[c],
                (sum / 64.).sqrt() as f32,
                "EOS padding counts"
            );
        }
        let position = m.status().playhead_frames;
        sink.pump(0);
        let empty = m.levels();
        assert!(empty.peak[..CHANNELS].iter().all(|&v| v == 0.));
        assert!(empty.rms[..CHANNELS].iter().all(|&v| v == 0.));
        assert_eq!(
            m.status().playhead_frames,
            position,
            "a zero pull consumes nothing"
        );
    }
}

#[test]
fn paused_output_is_exact_silence_with_and_without_hptf() {
    for hptf in [false, true] {
        let (m, _host) = fill(Kind::Sine);
        if hptf {
            m.hptf().publish(boost(1)).unwrap();
        }
        m.play();
        let mut sink = Sink::new(&m);
        sink.drain(4000);
        m.pause();
        let boundary = sink.pcm.len();
        sink.pump(2048);
        assert!(sink.pcm[boundary..].iter().all(|&v| v == 0.), "hptf={hptf}");
    }
}

#[test]
fn a_seek_bridges_from_the_last_emitted_frame_and_settles_on_the_target() {
    let (m, _host) = fill(Kind::Pattern);
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(300);
    m.seek(5000);
    assert!(eventually(|| m.status().playhead_frames == 5000));
    let before = sink.pcm.len();
    let (old_left, old_right) = (sink.pcm[before - 2], sink.pcm[before - 1]);
    const BRIDGE: usize = 480;
    sink.drain(BRIDGE + 20);
    let pcm = &sink.pcm;
    assert!(near(pcm[before], old_left) && near(pcm[before + 1], old_right));
    let mut max_step = 0f32;
    for i in before..pcm.len() {
        max_step = max_step.max((pcm[i] - pcm[i - 2]).abs());
    }
    assert!(max_step < 0.05, "the bridge removes the jump: {max_step}");
    for f in BRIDGE..BRIDGE + 20 {
        for c in 0..CHANNELS {
            assert!(
                near(pcm[before + f * 2 + c], pattern(5000 + f as u64, c)),
                "frame {f}"
            );
        }
    }
}

#[test]
fn a_paused_seek_resumes_from_silence_and_a_quick_play_waits_for_the_target() {
    let host = Arc::new(Host::default());
    let Ok(m) = Monitor::create(Box::new(Fill::new(Kind::Pattern)), false, host.clone()) else {
        panic!("create");
    };
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(300);
    m.pause();
    m.seek(7000);
    assert!(eventually(|| m.status().playhead_frames == 7000));
    let start = sink.pcm.len();
    sink.pump(64);
    assert!(sink.pcm[start..].iter().all(|&v| v == 0.));
    assert_eq!(
        m.status().playhead_frames,
        7000,
        "silent pulls do not advance the playhead"
    );
    let resumes = host.resumes.load(Ordering::SeqCst);
    let resumed_at = sink.pcm.len();
    m.play();
    assert_eq!(host.flushes.load(Ordering::SeqCst), 1);
    assert_eq!(host.resumes.load(Ordering::SeqCst), resumes + 1);
    sink.drain(500);
    assert_eq!(
        &sink.pcm[resumed_at..resumed_at + 2],
        &[0., 0.],
        "the fade-in starts silent"
    );
    let settled = resumed_at + 480 * 2;
    for c in 0..CHANNELS {
        assert!(near(sink.pcm[settled + c], pattern(7480, c)));
    }
    // A play right after a paused seek waits for the target and flushes and resumes once.
    m.pause();
    let (flushes, resumes) = (
        host.flushes.load(Ordering::SeqCst),
        host.resumes.load(Ordering::SeqCst),
    );
    m.seek(9000);
    m.play();
    assert!(eventually(|| {
        let s = m.status();
        s.playhead_frames == 9000 && s.state == State::Playing
    }));
    assert_eq!(host.flushes.load(Ordering::SeqCst), flushes + 1);
    assert_eq!(host.resumes.load(Ordering::SeqCst), resumes + 1);
}

#[test]
fn the_end_of_stream_is_clean_and_followed_by_silence() {
    let (m, _host) = create(Fill::new(Kind::Pattern).ending(2000));
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(2000);
    assert!(eventually(|| m.status().ended));
    assert_eq!(m.status().underruns, 0);
    sink.pump(100);
    assert_eq!(
        m.status().underruns,
        0,
        "pulls after the end are not underruns"
    );
    assert!(sink.pcm[2000 * CHANNELS..].iter().all(|&v| v == 0.));
}

#[test]
fn gain_edits_are_checked_as_a_whole_and_reach_the_output() {
    let (m, _host) = fill(Kind::Gain);
    m.set_overrides(gain_edit("obj0", -6.0206, 1, false))
        .unwrap();
    for gain in [800., f32::INFINITY, f32::NEG_INFINITY, f32::NAN] {
        let e = m
            .set_overrides(gain_edit("obj0", gain, 2, false))
            .unwrap_err();
        assert_eq!((e.code, e.context.as_slice()), (1, &b"obj0"[..]), "{gain}");
    }
    let mut mixed = gain_edit("obj0", -20., 2, false);
    mixed
        .objects
        .extend(gain_edit("obj1", 800., 2, false).objects);
    assert!(
        m.set_overrides(mixed).is_err(),
        "a later bad entry rejects the snapshot"
    );
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(20000);
    assert_eq!(m.status().override_revision, 1);
    assert!(
        sink.pcm.iter().all(|&v| near(v, 0.5)),
        "the edit before play applies at once"
    );
    m.set_overrides(gain_edit("obj0", -20., 2, false)).unwrap();
    sink.drain(30000);
    assert!(near(sink.last(), 0.1) && m.status().override_revision == 2);
    // The incoming backend of a switch keeps the edit.
    m.switch(Box::new(Fill::new(Kind::Gain)), None);
    sink.drain(40000);
    assert!(near(sink.last(), 0.1), "{}", sink.last());
    m.set_overrides(gain_edit("obj0", 800., 3, true)).unwrap();
    sink.drain(30000);
    assert_eq!((m.status().override_revision, sink.last()), (3, 0.));
    m.set_overrides(gain_edit("obj0", -1000., 4, false))
        .unwrap();
    sink.drain(30000);
    assert_eq!(
        (m.status().override_revision, sink.last()),
        (4, 0.),
        "an underflow is valid"
    );
}

#[test]
fn a_switch_crossfades_over_exactly_2048_frames() {
    let (m, _host) = fill(Kind::Const(0.25));
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(3000);
    m.switch(Box::new(Fill::new(Kind::Const(0.75))), None);
    sink.drain(40000);
    assert!(near(sink.last(), 0.75));
    let first = (0..sink.frames())
        .take_while(|&f| sink.pcm[f * 2] == 0.25)
        .count();
    assert!(first > 0 && first + 2048 < sink.frames());
    for f in 0..=2048 {
        let expected = 0.25 + f as f32 / 4096.;
        let i = (first - 1 + f) * 2;
        assert_eq!(
            (sink.pcm[i], sink.pcm[i + 1]),
            (expected, expected),
            "ramp frame {f}"
        );
    }
}

#[test]
fn an_interrupted_crossfade_restarts_from_the_incoming_stream() {
    for seek in [false, true] {
        let (m, _host) = fill(Kind::Const(0.25));
        m.play();
        assert!(eventually(|| m.status().buffered_frames == 8192));
        let first = Arc::new(Receipt::default());
        m.switch(Box::new(Fill::new(Kind::Const(0.75))), Some(first.clone()));
        assert!(eventually(|| first.state() == Receipt::APPLIED));
        let mut sink = Sink::new(&m);
        sink.pump(1024);
        assert!(
            eventually(|| m.status().buffered_frames == 8192),
            "one blended chunk"
        );
        if seek {
            m.seek(7777);
            assert!(eventually(|| m.status().playhead_frames == 7777));
            sink.drain(500);
            assert_eq!(sink.pcm[2048], 0.25);
            assert_eq!(sink.last(), 0.75, "the seek bridges to the incoming stream");
        } else {
            let second = Arc::new(Receipt::default());
            m.switch(Box::new(Fill::new(Kind::Const(0.5))), Some(second.clone()));
            assert!(eventually(|| second.state() == Receipt::APPLIED));
            sink.drain(8192 + 2049);
            for f in 0..=2048 {
                let expected = 0.75 - f as f32 / 8192.;
                let i = (9216 + f) * 2;
                assert_eq!(
                    (sink.pcm[i], sink.pcm[i + 1]),
                    (expected, expected),
                    "frame {f}"
                );
            }
        }
    }
}

#[test]
fn a_short_incoming_stream_ends_inside_the_crossfade() {
    let (m, _host) = fill(Kind::Const(0.25));
    m.play();
    assert!(eventually(|| m.status().buffered_frames == 8192));
    let mut short = Fill::new(Kind::Const(0.75)).ending(37);
    short.rewind = Some(37);
    let receipt = Arc::new(Receipt::default());
    m.switch(Box::new(short), Some(receipt.clone()));
    assert!(eventually(|| receipt.state() == Receipt::APPLIED));
    let mut sink = Sink::new(&m);
    sink.pump(1024);
    assert!(
        eventually(|| m.status().buffered_frames == 7168 + 37),
        "only the common frames"
    );
    sink.pump(37);
    assert!(eventually(|| m.status().ended));
    assert_eq!(m.status().buffered_frames, 7168);
    sink.pump(7168);
    for f in 0..37 {
        assert_eq!(
            sink.pcm[(8192 + f) * 2],
            0.25 + f as f32 / 4096.,
            "frame {f}"
        );
    }
    let status = m.status();
    assert_eq!((status.playhead_frames, status.underruns), (8192 + 37, 0));
}

#[test]
fn stopping_during_the_last_loop_block_starts_no_new_preroll() {
    let probe = Arc::new(Probe::default());
    probe.hold_process.store(true, Ordering::SeqCst);
    let mut source = Ramp::new(&probe);
    source.preroll = true;
    source.instant = Some(7);
    let (m, _host) = monitor(source, true);
    m.set_loop(8, 1031);
    m.seek(7); // immediate; only the loop's restoring seek could pre-roll
    m.play();
    assert!(eventually(|| probe.processing.load(Ordering::SeqCst)));
    let stopping = {
        let probe = probe.clone();
        std::thread::spawn(move || {
            // The C++ teardown: begin the shutdown, stop the device (here: release the block), drop.
            m.begin_shutdown();
            probe.hold_process.store(false, Ordering::SeqCst);
            drop(m);
        })
    };
    assert!(eventually(
        || probe.seek_started.load(Ordering::SeqCst) || stopping.is_finished()
    ));
    let started = probe.seek_started.load(Ordering::SeqCst);
    probe.release_seek.store(true, Ordering::SeqCst); // a regression fails below, not by hanging
    stopping.join().unwrap();
    assert!(!started, "the stop starts no new loop pre-roll");
}

#[test]
fn a_long_preroll_releases_control_and_is_cancelled_by_a_seek_or_the_stop() {
    let probe = Arc::new(Probe::default());
    let mut source = Ramp::new(&probe);
    source.preroll = true;
    source.instant = Some(7);
    let (m, _host) = monitor(source, true);
    m.seek(999_999);
    assert!(eventually(|| probe.seek_started.load(Ordering::SeqCst)));
    let before = Instant::now();
    m.pause();
    m.seek(7);
    assert!(
        before.elapsed() < Duration::from_millis(200),
        "control is not blocked"
    );
    assert!(eventually(|| m.status().playhead_frames == 7));
    assert!(probe.seek_cancelled.load(Ordering::SeqCst) > 0 && !m.status().failed);
    let rejected = Overrides {
        revision: 99,
        ..Overrides::default()
    };
    assert!(m.set_overrides(rejected).is_err());
    assert_ne!(m.status().override_revision, 99);
    probe.seek_started.store(false, Ordering::SeqCst);
    m.seek(999_999);
    assert!(eventually(|| probe.seek_started.load(Ordering::SeqCst)));
    let stopping = Instant::now();
    drop(m);
    assert!(
        stopping.elapsed() < Duration::from_millis(200),
        "the stop cancels the pre-roll"
    );
}

#[test]
fn a_warmed_backend_crossfades_in_once_its_preroll_ends() {
    let (m, _host) = fill(Kind::Const(0.25));
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(3000);
    m.set_overrides(Overrides {
        revision: 42,
        ..Overrides::default()
    })
    .unwrap();
    let incoming = Arc::new(Probe::default());
    let mut next = Ramp::new(&incoming);
    next.preroll = true;
    next.offset = 0.75;
    let receipt = Arc::new(Receipt::default());
    m.switch(Box::new(next), Some(receipt.clone()));
    assert!(eventually(|| incoming.seek_started.load(Ordering::SeqCst)));
    assert_eq!(
        incoming.revision_set.load(Ordering::SeqCst),
        42,
        "edits precede the pre-roll"
    );
    sink.drain(40000);
    assert!(
        near(sink.last(), 0.25),
        "the pre-roll does not replace the old audio early"
    );
    incoming.release_seek.store(true, Ordering::SeqCst);
    m.pause();
    std::thread::sleep(Duration::from_millis(30));
    m.play();
    sink.drain(40000);
    assert!(
        sink.last() > 0.75,
        "the warmed backend reaches the output: {}",
        sink.last()
    );
    assert_eq!(receipt.state(), Receipt::APPLIED);
}

/// The C++ `LoopProbeStream`: a one-channel intermediate carries the source frame index; the
/// output stage writes it where the playout cursor expects it and -1 otherwise.
#[derive(Default)]
struct LoopStage {
    consumer: AtomicU64,
    resets: AtomicUsize,
    last_reset: AtomicU64,
}

impl OutputStage for LoopStage {
    fn render_output(
        &self,
        intermediate: &[f32],
        output: &mut [f32],
        frames: usize,
        _pose: [f32; 3],
    ) -> usize {
        let position = self.consumer.load(Ordering::SeqCst);
        for f in 0..frames {
            let expected = (position + f as u64) as f32;
            let v = if intermediate[f] == expected {
                expected
            } else {
                -1.
            };
            output[f * CHANNELS..(f + 1) * CHANNELS].fill(v);
        }
        self.consumer
            .store(position + frames as u64, Ordering::SeqCst);
        frames
    }
    fn loop_render_reset(&self, frame: u64) {
        self.consumer.store(frame, Ordering::SeqCst);
        self.resets.fetch_add(1, Ordering::SeqCst);
        self.last_reset.store(frame, Ordering::SeqCst);
    }
    fn render_failed(&self) -> bool {
        false
    }
}

struct LoopProbe {
    stage: Arc<LoopStage>,
    producer: u64,
    repositions: Arc<(AtomicUsize, AtomicU64)>,
}

impl Source for LoopProbe {
    fn out_channels(&self) -> usize {
        CHANNELS
    }
    fn sample_rate(&self) -> u32 {
        RATE
    }
    fn process(&mut self, _output: &mut [f32], _frames: usize) -> Result<usize, Error> {
        Ok(0)
    }
    fn seek(&mut self, frame: u64) -> Result<(), Error> {
        self.producer = frame;
        self.stage.consumer.store(frame, Ordering::SeqCst);
        Ok(())
    }
    fn seek_with_cancel(&mut self, frame: u64, _cancel: &Cancel) -> Result<(), Error> {
        self.seek(frame)
    }
    fn seek_requires_preroll(&self) -> bool {
        false
    }
    fn validator(&self) -> Arc<dyn Validate> {
        Arc::new(Rejecting)
    }
    fn set_overrides(&mut self, _overrides: &Overrides) {}
    fn set_orientation(&mut self, _pose: [f32; 3]) {}
    fn output_stage(&self) -> Option<Arc<dyn OutputStage>> {
        Some(self.stage.clone())
    }
    fn intermediate_channels(&self) -> usize {
        1
    }
    fn produce_intermediate(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        for (f, v) in output[..frames].iter_mut().enumerate() {
            *v = (self.producer + f as u64) as f32;
        }
        self.producer += frames as u64;
        Ok(frames)
    }
    fn reposition_source(&mut self, frame: u64) -> Result<(), Error> {
        self.producer = frame;
        self.repositions.0.fetch_add(1, Ordering::SeqCst);
        self.repositions.1.store(frame, Ordering::SeqCst);
        Ok(())
    }
}

#[test]
fn an_output_stage_loop_wraps_the_source_and_the_playout_separately() {
    let stage = Arc::new(LoopStage::default());
    let repositions = Arc::new((AtomicUsize::new(0), AtomicU64::new(u64::MAX)));
    let source = LoopProbe {
        stage: stage.clone(),
        producer: 0,
        repositions: repositions.clone(),
    };
    let (m, _host) = create(source);
    assert!(m.output_stage());
    m.set_loop(0, 1000);
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(3500);
    for i in 0..3500 {
        assert_eq!(
            sink.pcm[i * CHANNELS],
            (i % 1000) as f32,
            "playout frame {i}"
        );
    }
    assert!(
        repositions.0.load(Ordering::SeqCst) >= 3,
        "the producer wraps the source"
    );
    assert!(
        stage.resets.load(Ordering::SeqCst) >= 3,
        "the playout wraps at its boundary"
    );
    assert_eq!(repositions.1.load(Ordering::SeqCst), 0);
    assert_eq!(stage.last_reset.load(Ordering::SeqCst), 0);
}

#[test]
fn lufs_starts_silent_reads_a_tone_and_resets_on_a_paused_seek() {
    let (m, _host) = fill(Kind::GainSine);
    assert!(
        !m.levels().momentary_lufs.is_finite(),
        "silent before playback"
    );
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(160_000);
    let first = m.levels();
    assert!(first.momentary_lufs > -40. && first.momentary_lufs < 0.);
    assert!(first.shortterm_lufs > -40. && first.integrated_lufs > -40.);
    m.pause();
    m.seek(0);
    assert!(eventually(|| {
        let l = m.levels();
        m.status().playhead_frames == 0
            && !l.momentary_lufs.is_finite()
            && !l.shortterm_lufs.is_finite()
            && !l.integrated_lufs.is_finite()
    }));
    m.play();
    sink.drain(160_000);
    let replay = m.levels().integrated_lufs;
    assert!(
        (replay - first.integrated_lufs).abs() < 0.1,
        "{replay} vs {}",
        first.integrated_lufs
    );
    // The seek also restarts the integration: 20 dB quieter after it reads 20 LU lower (without
    // the restart the gate would keep the louder history and the reading would barely move).
    m.pause();
    m.set_overrides(gain_edit("obj0", -20., 1, false)).unwrap();
    assert!(eventually(|| m.status().override_revision == 1));
    m.seek(0);
    assert!(eventually(
        || m.status().playhead_frames == 0 && !m.levels().integrated_lufs.is_finite()
    ));
    m.play();
    sink.drain(160_000);
    let quiet = m.levels().integrated_lufs;
    assert!(
        (quiet - (first.integrated_lufs - 20.)).abs() < 0.5,
        "{quiet} vs {}",
        first.integrated_lufs
    );
}

/// A +6 dB peak at 1 kHz (Q 1.5), warn-only preamp.
fn boost_coefficients() -> Coefficients {
    let profile = mradm_dsp::hptf::parse_parametric_eq(
        b"Preamp: 0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 6 dB Q 1.5\n",
    )
    .unwrap();
    mradm_dsp::hptf::design(
        &profile.bands,
        profile.preamp_db,
        RATE,
        PreampMode::WarnOnly,
    )
    .unwrap()
}

fn boost(revision: u64) -> mradm_dsp::hptf::StereoSnapshot {
    Snapshot {
        coefficients: boost_coefficients(),
        revision,
    }
    .into()
}

fn peak(pcm: &[f32]) -> f64 {
    pcm.iter().fold(0f64, |p, &v| p.max(f64::from(v).abs()))
}

/// The tone's peak through the boost: 0.5 scaled by the cascade's analytic gain at 1 kHz.
fn boosted_peak() -> f64 {
    0.5 * 10f64.powf(boost_coefficients().magnitude_db(1000.).unwrap() / 20.)
}

#[test]
fn hptf_follows_its_analytic_response_and_survives_a_switch() {
    let (m, _host) = fill(Kind::Sine);
    m.hptf().publish(boost(7)).unwrap();
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(40000);
    assert_eq!(m.hptf().applied_revision(), 7);
    let tail = sink.pcm.len() - 8000;
    assert!((peak(&sink.pcm[tail..]) - boosted_peak()).abs() < 0.01);
    m.switch(Box::new(Fill::new(Kind::Sine)), None);
    sink.drain(40000);
    let tail = sink.pcm.len() - 8000;
    assert!(
        (peak(&sink.pcm[tail..]) - boosted_peak()).abs() < 0.01,
        "after the switch"
    );
}

#[test]
fn without_hptf_the_output_is_bit_identical() {
    let (m, _host) = fill(Kind::Pattern);
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(8000);
    for f in 0..8000 {
        for c in 0..CHANNELS {
            assert_eq!(
                sink.pcm[f * CHANNELS + c],
                pattern(f as u64, c),
                "frame {f}"
            );
        }
    }
}

#[test]
fn lufs_reads_before_hptf_and_the_peak_after() {
    let run = |hptf: bool| {
        let (m, _host) = fill(Kind::Sine);
        if hptf {
            m.hptf().publish(boost(1)).unwrap();
        }
        m.play();
        Sink::new(&m).drain(160_000);
        let levels = m.levels();
        (levels.momentary_lufs, levels.peak[0])
    };
    let (lufs_off, peak_off) = run(false);
    let (lufs_on, peak_on) = run(true);
    assert!(lufs_off.is_finite() && lufs_on.is_finite());
    assert!((lufs_on - lufs_off).abs() < 0.1, "{lufs_on} vs {lufs_off}");
    assert!(peak_on > peak_off + 0.2, "{peak_on} vs {peak_off}");
}

#[test]
fn hptf_adds_no_click_across_a_seek() {
    let (m, _host) = fill(Kind::Sine);
    m.hptf().publish(boost(1)).unwrap();
    m.play();
    let mut sink = Sink::new(&m);
    sink.drain(20000);
    m.seek(100_000);
    assert!(eventually(|| m.status().playhead_frames == 100_000));
    sink.drain(20000);
    let w = std::f64::consts::TAU * 1000.0 / 48000.0;
    let bound = peak(&sink.pcm) * 2. * (w / 2.).sin() * 1.1 + 0.01;
    let worst = (2..sink.pcm.len())
        .map(|i| f64::from((sink.pcm[i] - sink.pcm[i - 2]).abs()))
        .fold(0f64, f64::max);
    assert!(
        worst <= bound,
        "largest step {worst} above the tone's own slope {bound}"
    );
}
