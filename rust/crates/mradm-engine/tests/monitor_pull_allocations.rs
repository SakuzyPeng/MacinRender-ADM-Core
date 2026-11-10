//! The monitor's device callback (PCM consistency 5.5 A; the C++ engine had no allocation guard)
//! neither allocates nor frees: on the first inactive and output-stage pulls, while playing,
//! across a seek's bridge, with repeated HpTF targets, on an underrun, while paused and after EOF.
use mradm_dsp::hptf::{Coefficients, Snapshot};
use mradm_engine::monitor::{
    Cancel, Monitor, MonitorHost, OutputStage, Overrides, Source, Validate,
};
use mradm_render::routing::Error;
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

thread_local! { static COUNT: Cell<Option<usize>> = const { Cell::new(None) }; }
struct Counting;
fn count() {
    let _ = COUNT.try_with(|c| {
        if let Some(n) = c.get() {
            c.set(Some(n + 1));
        }
    });
}
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        count();
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        count();
        unsafe { System.dealloc(p, layout) }
    }
    unsafe fn realloc(&self, p: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        count();
        unsafe { System.realloc(p, layout, size) }
    }
}
#[global_allocator]
static ALLOCATOR: Counting = Counting;

fn counted<T>(f: impl FnOnce() -> T) -> (T, usize) {
    COUNT.with(|c| c.set(Some(0)));
    let value = f();
    let n = COUNT.with(|c| c.replace(None)).unwrap_or(0);
    (value, n)
}

const RATE: u32 = 48000;

struct Noise {
    hold: Arc<AtomicBool>,
    left: u64,
    seed: u32,
    stage: Option<Arc<dyn OutputStage>>,
}

struct Accept;
impl Validate for Accept {
    fn validate(&self, _overrides: &Overrides) -> Result<(), Error> {
        Ok(())
    }
}

impl Source for Noise {
    fn out_channels(&self) -> usize {
        2
    }
    fn sample_rate(&self) -> u32 {
        RATE
    }
    fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        while self.hold.load(Ordering::SeqCst) {
            std::thread::sleep(Duration::from_millis(1));
        }
        let got = (frames as u64).min(self.left) as usize;
        for v in &mut output[..got * 2] {
            self.seed = self.seed.wrapping_mul(1664525).wrapping_add(1013904223);
            *v = (self.seed >> 8) as f32 / 16777216. - 0.5;
        }
        self.left -= got as u64;
        Ok(got)
    }
    fn seek(&mut self, _frame: u64) -> Result<(), Error> {
        Ok(())
    }
    fn seek_with_cancel(&mut self, frame: u64, _cancel: &Cancel) -> Result<(), Error> {
        self.seek(frame)
    }
    fn seek_requires_preroll(&self) -> bool {
        false
    }
    fn validator(&self) -> Arc<dyn Validate> {
        Arc::new(Accept)
    }
    fn set_overrides(&mut self, _overrides: &Overrides) {}
    fn set_orientation(&mut self, _pose: [f32; 3]) {}
    fn output_stage(&self) -> Option<Arc<dyn OutputStage>> {
        self.stage.clone()
    }
    fn intermediate_channels(&self) -> usize {
        2
    }
    fn produce_intermediate(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        self.process(output, frames)
    }
}

struct Passthrough;
impl OutputStage for Passthrough {
    fn render_output(
        &self,
        input: &[f32],
        output: &mut [f32],
        frames: usize,
        _pose: [f32; 3],
    ) -> usize {
        output[..frames * 2].copy_from_slice(&input[..frames * 2]);
        frames
    }
    fn loop_render_reset(&self, _frame: u64) {}
    fn render_failed(&self) -> bool {
        false
    }
}

struct Host;
impl MonitorHost for Host {
    fn log(&self, _level: u32, _module: &str, _message: &[u8]) {}
    fn device_flush(&self) {}
    fn device_pause(&self) {}
    fn device_resume(&self) {}
}

fn wait(mut predicate: impl FnMut() -> bool) {
    let end = Instant::now() + Duration::from_secs(5);
    while !predicate() {
        assert!(Instant::now() < end);
        std::thread::sleep(Duration::from_millis(1));
    }
}

#[test]
fn pulls_do_not_allocate() {
    let hold = Arc::new(AtomicBool::new(false));
    let source = Noise {
        hold: hold.clone(),
        left: 120_000,
        seed: 1,
        stage: None,
    };
    let Ok(m) = Monitor::create(Box::new(source), true, Arc::new(Host)) else {
        panic!("create");
    };
    let mut block = vec![0f32; 480 * 2];
    let mut pull = |m: &Monitor| counted(|| m.pull(&mut block, 480));
    m.play();
    wait(|| m.status().buffered_frames >= 4096);
    for _ in 0..8 {
        let (got, n) = pull(&m);
        assert_eq!((got.frames, n), (480, 0), "playing");
    }
    // Rotate through all pending and status mailbox slots, including slots first reached by
    // a later publication. Status queries run on the control side, outside the counted pull.
    for revision in 3..9 {
        let coefficients = Coefficients {
            sample_rate: RATE,
            preamp_gain: if revision % 2 == 0 { 0.75 } else { 0.5 },
            ..Coefficients::default()
        };
        m.hptf()
            .publish(
                Snapshot {
                    coefficients,
                    revision,
                }
                .into(),
            )
            .unwrap();
        for _ in 0..8 {
            assert_eq!(pull(&m).1, 0, "hptf blend");
        }
        assert_eq!(m.hptf().applied_revision(), revision);
    }
    // A seek's 10 ms bridge.
    m.seek(30_000);
    wait(|| m.status().playhead_frames == 30_000 && m.status().buffered_frames >= 4096);
    for _ in 0..4 {
        assert_eq!(pull(&m).1, 0, "seek bridge");
    }
    // An underrun while the producer is held.
    hold.store(true, Ordering::SeqCst);
    let mut underran = false;
    for _ in 0..200 {
        let (got, n) = pull(&m);
        assert_eq!(n, 0, "underrun");
        if got.frames < 480 {
            underran = true;
            break;
        }
    }
    assert!(underran && m.status().underruns > 0);
    hold.store(false, Ordering::SeqCst);
    m.pause();
    for _ in 0..4 {
        let (got, n) = pull(&m);
        assert_eq!((got.frames, n), (0, 0), "paused");
    }
    m.play();
    let end = Instant::now() + Duration::from_secs(5);
    loop {
        assert!(Instant::now() < end);
        let (got, n) = pull(&m);
        assert_eq!(n, 0, "draining");
        if got.mark_end {
            break;
        }
        if got.frames == 0 {
            std::thread::sleep(Duration::from_millis(1));
        }
    }
    for _ in 0..4 {
        let (got, n) = pull(&m);
        assert!(got.mark_end);
        assert_eq!(n, 0, "ended");
    }
}

#[test]
fn initial_inactive_pulls_do_not_allocate() {
    let source = Noise {
        hold: Arc::new(AtomicBool::new(false)),
        left: 120_000,
        seed: 1,
        stage: None,
    };
    let Ok(m) = Monitor::create(Box::new(source), true, Arc::new(Host)) else {
        panic!("create");
    };
    let mut block = [1f32; 480 * 2];
    let (got, n) = counted(|| m.pull(&mut block, 480));
    assert_eq!((got.frames, n), (0, 0), "first stopped pull");
    assert!(block.iter().all(|&v| v == 0.));
    m.pause();
    let (got, n) = counted(|| m.pull(&mut block, 480));
    assert_eq!((got.frames, n), (0, 0), "paused before play");
}

#[test]
fn output_stage_pulls_do_not_allocate() {
    let source = Noise {
        hold: Arc::new(AtomicBool::new(false)),
        left: 120_000,
        seed: 1,
        stage: Some(Arc::new(Passthrough)),
    };
    let Ok(m) = Monitor::create(Box::new(source), true, Arc::new(Host)) else {
        panic!("create");
    };
    let mut block = [0f32; 480 * 2];
    m.play();
    wait(|| m.status().buffered_frames >= 4096);
    for _ in 0..8 {
        let (got, n) = counted(|| m.pull(&mut block, 480));
        assert_eq!((got.frames, n), (480, 0), "output-stage pull");
        assert!(block.iter().any(|&v| v != 0.));
    }
}
