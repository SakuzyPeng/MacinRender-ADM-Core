//! The player's pull (PCM consistency 5.4 B, formerly guarded by the C++
//! `scene_callback_alloc_test` alone) neither allocates nor frees, while buffering, after an
//! underrun and at the end of stream.
mod common;
use common::backend::TestHost;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, RoomRequest, Screens, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::renderer;
use mradm_engine::scene::stream::{
    Config, Descriptor, FrameInput, PlaneView, Stream, Submit, frame_flag, pull_flag,
};
use mradm_render::live::{self, field};
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::Arc;
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

/// The allocations of `f` on this thread.
fn counted<T>(f: impl FnOnce() -> T) -> (T, usize) {
    COUNT.with(|c| c.set(Some(0)));
    let value = f();
    let n = COUNT.with(|c| c.replace(None)).unwrap_or(0);
    (value, n)
}

#[test]
fn pulls_do_not_allocate() {
    let renderer = renderer::Config {
        selection: Selection::SafBinaural,
        layout_id: b"binaural",
        apple_geometry: false,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread: BinauralSpreadMode::Automatic,
        split_power: false,
        smoothing_frames: 0,
        sample_rate: 48000,
        sofa: None,
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry::default(),
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        room: RoomRequest {
            package: None,
            hrir: b"",
            metres: 0.,
            quality: 0,
        },
        sink: true,
    };
    let config = Config {
        output_sample_rate: 44100,
        output_ring_frames: 4096,
        startup_watermark_frames: 256,
        ..Config::default()
    };
    let s = Stream::create(&config, &renderer, Arc::new(TestHost)).unwrap();
    let mut buffer = vec![0f32; 2 * 512];
    let (p, n) = counted(|| s.pull(&mut buffer, 512));
    assert_eq!((p.flags, n), (pull_flag::BUFFERING, 0));

    s.begin_epoch(1, 0).unwrap();
    s.configure_generation(
        1,
        1,
        &[Descriptor {
            element_id: 1,
            role: 0,
            speaker_label: Vec::new(),
            has_position: false,
            position: [0., 1., 0.],
            flags: 0,
            identity: None,
        }],
    )
    .unwrap();
    let samples = vec![0.25f32; 2048];
    let planes = [PlaneView {
        element_id: 1,
        sample_count: 2048,
        stride: 1,
        has_signal: true,
        samples: Some(&samples),
    }];
    let initial = [live::Entry {
        element_id: 1,
        state: live::State {
            valid_fields: field::KNOWN,
            ..live::State::default()
        },
    }];
    let frame = FrameInput {
        epoch: 1,
        generation: 1,
        start: 0,
        duration: 2048,
        flags: frame_flag::STATE_COMPLETE,
        planes: &planes[..],
        initial: &initial,
        updates: &[],
    };
    assert_eq!(s.submit_frame(&frame, 5000).unwrap(), Submit::Accepted);
    s.wait_idle(10000).unwrap();
    let mut flags = 0;
    let mut total = 0;
    let deadline = Instant::now() + Duration::from_secs(20);
    while total < 2 {
        let (p, n) = counted(|| s.pull(&mut buffer, 512));
        assert_eq!(n, 0, "a pull allocated");
        flags |= p.flags;
        if p.flags & pull_flag::UNDERRUN != 0 {
            total += 1;
        }
        assert!(Instant::now() < deadline);
    }
    s.signal_end(1, 2048).unwrap();
    loop {
        let (p, n) = counted(|| s.pull(&mut buffer, 512));
        assert_eq!(n, 0, "a pull allocated");
        flags |= p.flags;
        if p.flags & pull_flag::EOS != 0 {
            break;
        }
        assert!(Instant::now() < deadline);
        std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(flags & pull_flag::FAILED, 0);
}
