//! The realtime monitor engine (PCM consistency 5.5 A) driven by a mock stream and host: playback,
//! loops, seeks with the deferred device flush, live edits and their revisions, backend switches
//! with and without a warmup, failures and the end of stream. The C++ `mr_adm_realtime_tests`
//! drive the same engine through the island table.
use mradm_engine::monitor::{
    Cancel, CreateError, Monitor, MonitorHost, ObjectOverride, Overrides, Receipt, Source, State,
    Validate,
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
        if self.preroll {
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
    fn device_resume(&self) {}
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
