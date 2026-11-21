use super::*;
use crate::test_alloc::COUNT;
use std::{cell::Cell, mem::MaybeUninit};
use windows::Win32::{
    Foundation::{E_FAIL, WAIT_OBJECT_0},
    Media::Audio::*,
};

#[test]
fn public_layouts_match_the_object_routing_plans() {
    let capabilities = layouts();
    let expected = [
        ("4+7+0", "7.1.4", 12, 0),
        ("4+5+0", "5.1.4", 10, 0),
        ("0+5+0", "5.1", 6, 0),
        ("9.1.6", "9.1.6", 16, 4),
        ("9+10+3", "22.2", 24, 11),
    ];
    assert_eq!(capabilities.len(), expected.len());
    for (cap, (id, name, channels, dynamic)) in capabilities.iter().zip(expected) {
        assert_eq!(
            (
                cap.id.as_str(),
                cap.display_name.as_str(),
                u32::from(cap.channel_count)
            ),
            (id, name, channels)
        );
        let bed = Bed::new(id, channels, Geometry::Standard).unwrap();
        assert_eq!(bed.dynamic, dynamic);
        assert_eq!(bed.mask.0.count_ones(), channels.min(12));
        assert_eq!(
            bed.routes.len() as u32,
            if channels == 24 { 23 } else { channels }
        );
        assert_eq!(
            bed.routes.iter().filter(|r| r.position.is_some()).count() as u32,
            dynamic
        );
    }
    let bed = Bed::new("4+7+0", 12, Geometry::Standard).unwrap();
    assert_eq!(
        bed.routes.iter().map(|r| r.kind).collect::<Vec<_>>(),
        [
            AudioObjectType_FrontLeft,
            AudioObjectType_FrontRight,
            AudioObjectType_FrontCenter,
            AudioObjectType_LowFrequency,
            AudioObjectType_SideLeft,
            AudioObjectType_SideRight,
            AudioObjectType_BackLeft,
            AudioObjectType_BackRight,
            AudioObjectType_TopFrontLeft,
            AudioObjectType_TopFrontRight,
            AudioObjectType_TopBackLeft,
            AudioObjectType_TopBackRight
        ]
    );
    for (id, channels) in [("0+5+0", 6), ("4+5+0", 10)] {
        let bed = Bed::new(id, channels, Geometry::Standard).unwrap();
        assert_eq!(
            [bed.routes[4].kind, bed.routes[5].kind],
            [AudioObjectType_BackLeft, AudioObjectType_BackRight]
        );
    }
    let bed = Bed::new("9+10+3", 24, Geometry::Standard).unwrap();
    let dynamic_slots: Vec<_> = bed
        .routes
        .iter()
        .enumerate()
        .filter_map(|(i, r)| r.position.map(|_| i))
        .collect();
    assert_eq!(dynamic_slots, [0, 1, 8, 13, 14, 17, 18, 19, 20, 21, 22]);
    for (geometry, left_x) in [
        (Geometry::Standard, -0.939693),
        (Geometry::Apple, -0.866025),
    ] {
        let bed = Bed::new("9.1.6", 16, geometry).unwrap();
        let [lx, ly, lz] = bed.routes[8].position.unwrap();
        let [rx, ry, rz] = bed.routes[9].position.unwrap();
        assert!((lx - left_x).abs() < 1e-5 && lz < 0.0);
        assert_eq!([lx, ly, lz], [-rx, ry, rz]);
    }
}

#[test]
fn dynamic_22_2_objects_use_the_selected_height_geometry() {
    // B+045 is at azimuth +45° and elevation −30° (standard) or −15° (Apple).
    for (geometry, bottom_left) in [
        (Geometry::Standard, [-0.612_372_4, -0.5, -0.612_372_4]),
        (Geometry::Apple, [-0.683_012_7, -0.258_819_04, -0.683_012_7]),
    ] {
        let bed = Bed::new("9+10+3", 24, geometry).unwrap();
        for (speaker, slot, expected) in
            [("T+000", 14, [0.0, 1.0, 0.0]), ("B+045", 21, bottom_left)]
        {
            let position = bed.routes[slot].position.expect("dynamic speaker");
            assert!(
                position
                    .into_iter()
                    .zip(expected)
                    .all(|(a, b)| (a - b).abs() < 1e-5),
                "{geometry:?} {speaker}: expected {expected:?}, got {position:?}"
            );
        }
    }
}

#[test]
fn dual_lfe_and_deinterleaving_use_only_the_frames_written() {
    let bed = Bed::new("9+10+3", 24, Geometry::Standard).unwrap();
    let mut pcm = [0.0; 24 * 3];
    for (ch, sample) in pcm[..24].iter_mut().enumerate() {
        *sample = ch as f32;
    }
    pcm[9] = 0.0;
    pcm[24 + 9] = 2.0;
    // The dual LFE sees only one active input when the object buffer holds one frame.
    let mut dst = [MaybeUninit::new(0.0); 3];
    COUNT.with(|count| count.set(Some(0)));
    for (i, route) in bed.routes.iter().enumerate() {
        route.write(&pcm, 24, &mut dst[..1]);
        let expected = if i < 9 { i } else { i + 1 };
        assert_eq!(unsafe { dst[0].assume_init() }, expected as f32);
    }
    // Different frames carry LFE1 / LFE2, so block-wide activity attenuates both.
    bed.routes[3].write(&pcm, 24, &mut dst);
    let allocations = COUNT.with(|count| count.replace(None).unwrap());
    let gain = mradm_render::routing::LFE_SPLIT_POWER_GAIN;
    assert_eq!(
        dst.map(|v| unsafe { v.assume_init() }),
        [3.0 * gain, 2.0 * gain, 0.0]
    );
    pcm[3] = 0.5;
    pcm[9] = 0.5;
    bed.routes[3].write(&pcm, 24, &mut dst[..1]);
    assert_eq!(unsafe { dst[0].assume_init() }, gain);
    pcm[3] = 2.0e-12;
    pcm[9] = 1.0e-12;
    bed.routes[3].write(&pcm, 24, &mut dst[..1]);
    assert_eq!(unsafe { dst[0].assume_init() }, pcm[3] + pcm[9]);
    assert_eq!(allocations, 0);
}

#[test]
fn invalid_layout_and_width_fail_before_device_activation() {
    for (id, channels, code, context) in [
        ("binaural", 2, code::UNSUPPORTED, "layout=binaural"),
        (
            "4+7+0",
            2,
            code::INVALID_ARGUMENT,
            "layout=4+7+0 channels=2",
        ),
    ] {
        let table = open(id.into(), 0);
        let device = unsafe { crate::render::device_island::Device::adopt(&table) };
        unsafe extern "C" fn silence(_: *mut c_void, _: *mut f32, _: usize, _: usize) -> usize {
            0
        }
        let e = device
            .start(channels, 48000, std::ptr::null_mut(), silence)
            .unwrap_err();
        assert_eq!(e.code, code);
        assert_eq!(e.context, context.as_bytes());
    }
}

#[test]
fn invalidated_stream_is_dropped_and_retried_until_the_endpoint_returns() {
    let wake = Wake::new().unwrap();
    let recovering = AtomicBool::new(false);
    let drops = Cell::new(0);
    struct FakeStream<'a>(&'a Cell<usize>);
    impl Drop for FakeStream<'_> {
        fn drop(&mut self) {
            self.0.set(self.0.get() + 1);
        }
    }
    let mut opens = 0;
    let mut updates = 0;
    pump(
        FakeStream(&drops),
        &wake,
        &recovering,
        || {
            assert!(recovering.load(Ordering::Acquire));
            assert_eq!(drops.get(), 1);
            opens += 1;
            if opens == 1 {
                Err(Error::new(code::UNSUPPORTED, "endpoint switching", ""))
            } else {
                Ok(FakeStream(&drops))
            }
        },
        |_| {
            assert!(!recovering.load(Ordering::Acquire));
            updates += 1;
            if updates == 1 {
                Err(E_FAIL.into())
            } else {
                wake.stopping.store(true, Ordering::Release);
                Ok(())
            }
        },
    );
    assert_eq!((opens, updates, drops.get()), (2, 2, 2));
}

#[test]
#[ignore = "requires a Windows default output endpoint with spatial sound enabled"]
fn default_endpoint_submits_all_layouts_without_rust_allocations() {
    let _apartment = Apartment::enter().unwrap();
    let wake = Wake::new().unwrap();
    for layout in layouts() {
        let bed = Bed::new(&layout.id, layout.channel_count.into(), Geometry::Standard).unwrap();
        let mut stream = Stream::open(&bed, 48000, wake.handle()).unwrap();
        let mut frames = 0;
        for _ in 0..32 {
            assert_eq!(
                unsafe { WaitForSingleObject(wake.handle(), 1000) },
                WAIT_OBJECT_0
            );
            COUNT.with(|count| count.set(Some(0)));
            let result = stream.update(&bed, &mut |_, count| {
                frames += count;
                0
            });
            let allocations = COUNT.with(|count| count.replace(None).unwrap());
            result.unwrap();
            assert_eq!(allocations, 0);
        }
        assert!(frames > 0);
        println!("{}: 32 updates, {frames} frames", layout.display_name);
    }
}

#[test]
#[ignore = "requires a Windows default output endpoint with spatial sound enabled"]
fn failed_start_can_retry_and_stop_joins_the_last_pull() {
    const TIMEOUT: Duration = Duration::from_secs(2);
    #[derive(Debug, PartialEq, Eq)]
    enum Event {
        PullEntered,
        PullExited,
        StopStarted,
        StopReturned,
    }
    struct PullGate {
        gate: Mutex<()>,
        events: mpsc::Sender<Event>,
    }
    unsafe extern "C" fn blocked_pull(user: *mut c_void, _: *mut f32, _: usize, _: usize) -> usize {
        let pull = unsafe { &*user.cast::<PullGate>() };
        let events = pull.events.clone();
        let _ = events.send(Event::PullEntered);
        drop(pull.gate.lock().unwrap_or_else(|e| e.into_inner()));
        let _ = events.send(Event::PullExited);
        0
    }
    let (events, observed) = mpsc::channel();
    let pull = PullGate {
        gate: Mutex::new(()),
        events,
    };
    let user = std::ptr::from_ref(&pull).cast_mut().cast();
    let table = open("4+7+0".into(), 0);
    let device = unsafe { crate::render::device_island::Device::adopt(&table) };
    let bad = device.start(12, 0, user, blocked_pull).unwrap_err();
    assert_eq!(bad.code, code::UNSUPPORTED);
    assert!(
        bad.context
            .starts_with(b"system-spatial sample_rate=0Hz hr=0x")
    );
    assert_eq!(observed.try_recv(), Err(mpsc::TryRecvError::Empty));
    for _ in 0..2 {
        let hold = pull.gate.lock().unwrap();
        device.start(12, 48000, user, blocked_pull).unwrap();
        assert_eq!(observed.recv_timeout(TIMEOUT), Ok(Event::PullEntered));
        assert_eq!(
            device
                .start(12, 48000, user, blocked_pull)
                .unwrap_err()
                .code,
            code::INVALID_ARGUMENT
        );
        thread::scope(|scope| {
            scope.spawn(|| {
                pull.events.send(Event::StopStarted).unwrap();
                device.stop();
                pull.events.send(Event::StopReturned).unwrap();
            });
            assert_eq!(observed.recv_timeout(TIMEOUT), Ok(Event::StopStarted));
            let while_held = observed.recv_timeout(Duration::from_millis(50));
            drop(hold);
            assert_eq!(observed.recv_timeout(TIMEOUT), Ok(Event::PullExited));
            assert_eq!(while_held, Err(mpsc::RecvTimeoutError::Timeout));
            assert_eq!(observed.recv_timeout(TIMEOUT), Ok(Event::StopReturned));
        });
        assert_eq!(
            observed.recv_timeout(Duration::from_millis(30)),
            Err(mpsc::RecvTimeoutError::Timeout)
        );
        assert!(!device.progress().recovering);
    }
}
