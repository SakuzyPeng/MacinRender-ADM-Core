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

#[test]
fn output_session_pulls_do_not_allocate() {
    use mradm_dsp::hptf::Coefficients;
    use mradm_engine::scene::output::Session;
    let renderer = renderer::Config {
        selection: Selection::Saf,
        layout_id: b"0+2+0",
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
        output_sample_rate: 48000,
        output_ring_frames: 1 << 15,
        startup_watermark_frames: 1,
        ..Config::default()
    };
    let s = Arc::new(Stream::create(&config, &renderer, Arc::new(TestHost)).unwrap());
    let out = Session::new(s.clone(), true, false).unwrap();
    out.reset_epoch(1, 0).unwrap();
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
    let samples = vec![0.5f32; 8192];
    let planes = [PlaneView {
        element_id: 1,
        sample_count: 8192,
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
        duration: 8192,
        flags: frame_flag::STATE_COMPLETE,
        planes: &planes[..],
        initial: &initial,
        updates: &[],
    };
    assert_eq!(s.submit_frame(&frame, 5000).unwrap(), Submit::Accepted);
    s.signal_end(1, 8192).unwrap();
    s.wait_idle(10000).unwrap();
    // A fade in progress and one queued behind it, both crossed inside the pulls.
    let boost = Coefficients {
        sample_rate: 48000,
        preamp_gain: 3.0,
        ..Coefficients::default()
    };
    out.set_hptf(Some(boost), 1).unwrap();
    out.set_volume(0.75).unwrap();
    out.play();
    let mut buffer = vec![0f32; 2 * 300];
    let mut eos = false;
    for i in 0..100 {
        if i == 3 {
            out.set_hptf(None, 2).unwrap();
        }
        let (p, n) = counted(|| {
            let p = out.pull(&mut buffer, 300);
            out.release();
            p
        });
        assert_eq!(n, 0, "an output pull allocated");
        if p.eos {
            eos = true;
            break;
        }
    }
    assert!(eos, "no end of stream");
    assert_eq!(out.hptf_snapshot().revision, 2);
}

/// The C++ `scene_callback_alloc_test` configurations (R3 D2): a binaural and a resampled stereo
/// feed with HpTF (one fade in progress, one queued behind it) and volume, and an unprotected
/// 4+7+0 feed with volume; every pull, including those that underrun after the first frame is
/// drained and those after the resumed feed, neither allocates nor frees.
#[test]
fn session_pulls_through_hptf_volume_and_underruns_do_not_allocate() {
    use mradm_dsp::hptf::{PreampMode, design, parse_parametric_eq};
    use mradm_engine::scene::output::{DeviceProgress, Session};
    let hptf = |text: &[u8], rate: u32| {
        let p = parse_parametric_eq(text).unwrap();
        design(&p.bands, p.preamp_db, rate, PreampMode::WarnOnly).unwrap()
    };
    let cases: [(Selection, &'static [u8], u32, u32, bool); 3] = [
        (Selection::SafBinaural, b"binaural", 48000, 48000, true),
        (Selection::Saf, b"0+2+0", 44100, 48000, true),
        (Selection::Saf, b"4+7+0", 48000, 48000, false),
    ];
    for (selection, layout, input_rate, output_rate, protect) in cases {
        let renderer = renderer::Config {
            selection,
            layout_id: layout,
            apple_geometry: false,
            speaker_spread: SpeakerSpread::Automatic,
            binaural_spread: BinauralSpreadMode::Automatic,
            split_power: false,
            smoothing_frames: 0,
            sample_rate: input_rate,
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
            output_sample_rate: output_rate,
            startup_watermark_frames: 1,
            ..Config::default()
        };
        let s = Arc::new(Stream::create(&config, &renderer, Arc::new(TestHost)).unwrap());
        let out = Session::new(s.clone(), protect, false).unwrap();
        let channels = out.channels() as usize;
        let hptf_a = hptf(
            b"Preamp: -3 dB\nFilter 1: ON PK Fc 1700 Hz Gain 6 dB Q 0.75\n",
            output_rate,
        );
        let hptf_b = hptf(
            b"Preamp: -6 dB\nFilter 1: ON LSC Fc 200 Hz Gain 9 dB Q 0.7\n",
            output_rate,
        );
        out.reset_epoch(1, 0).unwrap();
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
        let mut seed = 0x2468_ACE1u32;
        let samples: Vec<f32> = (0..4096)
            .map(|_| {
                seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                2. * ((seed >> 8) as f32 / 8_388_608. - 1.)
            })
            .collect();
        let submit = |start: i64| {
            let planes = [PlaneView {
                element_id: 1,
                sample_count: 4096,
                stride: 1,
                has_signal: true,
                samples: Some(&samples),
            }];
            let initial = [live::Entry {
                element_id: 1,
                state: live::State {
                    valid_fields: field::KNOWN,
                    position: [0.25, 0.75, 0.],
                    ..live::State::default()
                },
            }];
            let frame = FrameInput {
                epoch: 1,
                generation: 1,
                start,
                duration: 4096,
                flags: frame_flag::STATE_COMPLETE,
                planes: &planes[..],
                initial: if start == 0 { &initial[..] } else { &[] },
                updates: &[],
            };
            assert_eq!(s.submit_frame(&frame, 5000).unwrap(), Submit::Accepted);
            s.wait_idle(20000).unwrap();
        };
        let mut buffer = vec![0f32; 480 * channels];
        let mut pull = || {
            let (p, n) = counted(|| {
                let p = out.pull(&mut buffer, 480);
                if p.active {
                    out.release();
                }
                p
            });
            assert_eq!(n, 0, "{selection:?}: a pull allocated");
            p
        };
        out.play();
        submit(0);
        for _ in 0..6 {
            pull();
        }
        if protect {
            out.set_hptf(Some(hptf_a), 1).unwrap();
            pull();
            out.set_hptf(Some(hptf_b), 2).unwrap(); // queued behind the active fade
            out.set_volume(0.5).unwrap();
            pull();
        } else {
            out.set_volume(0.5).unwrap();
        }
        // Drain into repeated underruns, then resume.
        for _ in 0..32 {
            pull();
        }
        submit(4096);
        s.signal_end(1, 8192).unwrap();
        let deadline = Instant::now() + Duration::from_secs(20);
        while !out.status(DeviceProgress::default()).ended {
            if pull().frames == 0 {
                assert!(Instant::now() < deadline, "no end of stream");
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        assert!(
            out.status(DeviceProgress::default()).underruns > 0,
            "{selection:?}: the pulls underran"
        );
    }
}
