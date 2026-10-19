#![cfg(feature = "room-gpu-experiment")]
use mradm_dsp::room::{
    control::{RoomCommand, RoomDescription, State},
    gpu_experiment::ReferenceFactory,
    prepared::PreparedRoom,
    quality::Quality,
    session::{Instance, PreparedSet, Session},
};
use std::{
    alloc::{GlobalAlloc, Layout, System},
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
};
#[path = "common/room_fixture.rs"]
#[allow(dead_code)]
mod common;
static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
struct Allocator;
static CHECK: AtomicBool = AtomicBool::new(false);
static ALLOCS: AtomicUsize = AtomicUsize::new(0);
unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        if CHECK.load(Ordering::Relaxed) {
            ALLOCS.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        unsafe { System.dealloc(p, l) }
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        if CHECK.load(Ordering::Relaxed) {
            ALLOCS.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.realloc(p, l, n) }
    }
}
#[global_allocator]
static ALLOCATOR: Allocator = Allocator;
fn adapted(base: &Arc<PreparedSet>) -> Arc<PreparedSet> {
    Arc::new(
        PreparedSet::new(base.room.clone())
            .unwrap()
            .with_gpu_experiment(Arc::new(ReferenceFactory))
            .unwrap(),
    )
}
#[test]
fn recorded_full_session_preserves_fades_branches_reset_switch_and_tail_without_allocations() {
    let _lock = TEST_LOCK.lock().unwrap();
    for rate in [48000, 96000] {
        let baseline = common::fixture_at_rate(Quality::Realtime, rate);
        assert!(baseline.room.late[0].gpu_tail().is_some());
        let candidate = adapted(&baseline);
        let mut changed = (*baseline.room.profile).clone();
        changed.absorption = [[0.8; 8]; 6];
        changed.identity.push_str("-gpu-test");
        let next = Arc::new(
            PreparedSet::new(Arc::new(
                PreparedRoom::with_quality(
                    Arc::new(changed),
                    baseline.room.hrir.clone(),
                    Quality::Realtime,
                )
                .unwrap(),
            ))
            .unwrap(),
        );
        let next_gpu = adapted(&next);
        let mut a = Session::new(baseline.clone(), 1.5, true, true).unwrap();
        let mut b = Session::new(candidate, 1.5, true, true).unwrap();
        let descriptions: Vec<_> = (0..3)
            .map(|i| RoomDescription {
                id: i + 1,
                role: if i == 2 { 2 } else { 0 },
                position: [0.1 * i as f32, 0.4, 0.],
                ..Default::default()
            })
            .collect();
        a.configure(&descriptions).unwrap();
        b.configure(&descriptions).unwrap();
        let mut switch_a = Some(Box::new(Instance::new(next, 3).unwrap()));
        let mut switch_b = Some(Box::new(Instance::new(next_gpu, 3).unwrap()));
        let mut input = [0f32; 512];
        let mut out_a = [0f32; 1024];
        let mut out_b = [0f32; 1024];
        let mut error = 0f64;
        let mut energy = 0f64;
        let mut peak = 0f64;
        let mut max_error = 0f64;
        let mut commands = Vec::with_capacity(3);
        let mut absolute = 0usize;
        for block in 0..48 {
            // Block 5 follows exactly 2048 input frames: a GPU late result is
            // pending and belongs to the old epoch. Reset must discard it.
            if block == 5 || block == 8 {
                a.reset(4096).unwrap();
                b.reset(4096).unwrap();
            }
            if block == 24 {
                a.queue_room(switch_a.take().unwrap(), 1).unwrap();
                b.queue_room(switch_b.take().unwrap(), 1).unwrap();
            }
            let frames = [512, 137, 375, 512][block % 4];
            for (i, v) in input[..frames].iter_mut().enumerate() {
                *v = if block < 2 {
                    0.
                } else {
                    ((absolute + i) % 179) as f32 * 0.00001 - 0.00089
                };
            }
            commands.clear();
            for source in 0..2 {
                commands.push(RoomCommand {
                    element: source,
                    offset: ((source as usize * 131 + 17) % frames) as u32,
                    duration: 512,
                    changed: 4 | 8 | 16 | 256,
                    state: State {
                        position: [
                            (block as f32 * 0.1).sin() * 0.6,
                            0.2 + (block % 7) as f32 * 0.15,
                            0.,
                        ],
                        extent: if block % 9 == 0 { [0.1; 3] } else { [0.; 3] },
                        diffuse: 0.3,
                        head_locked: u32::from(source == 1 && (12..20).contains(&block)),
                        ..Default::default()
                    },
                    ..Default::default()
                });
            }
            commands.sort_by_key(|c| c.offset);
            a.set_pose([block as f32 * 0.3, 1., 0.]).unwrap();
            b.set_pose([block as f32 * 0.3, 1., 0.]).unwrap();
            a.process(
                frames,
                (0..3).map(|_| Some(&input[..frames])),
                &[],
                &commands,
                &mut out_a[..frames * 2],
            )
            .unwrap();
            ALLOCS.store(0, Ordering::SeqCst);
            CHECK.store(true, Ordering::SeqCst);
            let result = b.process(
                frames,
                (0..3).map(|_| Some(&input[..frames])),
                &[],
                &commands,
                &mut out_b[..frames * 2],
            );
            CHECK.store(false, Ordering::SeqCst);
            result.unwrap();
            assert_eq!(ALLOCS.load(Ordering::SeqCst), 0, "GPU recorder allocated");
            for (&x, &y) in out_a[..frames * 2].iter().zip(&out_b[..frames * 2]) {
                assert!(x.is_finite() && y.is_finite());
                let d = f64::from(x) - f64::from(y);
                error += d * d;
                energy += f64::from(x).powi(2);
                peak = peak.max(f64::from(x).abs());
                max_error = max_error.max(d.abs());
            }
            absolute += frames;
            let _ = a.take_retired();
            let _ = b.take_retired();
        }
        a.end_input();
        b.end_input();
        assert_eq!(a.info().tail_frames, b.info().tail_frames);
        for _ in 0..(a.info().tail_frames as usize).div_ceil(512) + 2 {
            a.process(512, (0..3).map(|_| None), &[], &[], &mut out_a)
                .unwrap();
            b.process(512, (0..3).map(|_| None), &[], &[], &mut out_b)
                .unwrap();
            for (&x, &y) in out_a.iter().zip(&out_b) {
                let d = f64::from(x) - f64::from(y);
                error += d * d;
                energy += f64::from(x).powi(2);
                peak = peak.max(f64::from(x).abs());
                max_error = max_error.max(d.abs());
            }
        }
        assert!(
            (error / energy).sqrt() <= 2e-5,
            "{rate}: L2 {} peak {}",
            (error / energy).sqrt(),
            max_error / peak
        );
        assert!(max_error / peak <= 2e-4);
        assert_eq!(a.info().latency_frames, b.info().latency_frames);
        assert_eq!(a.info().input_frames, b.info().input_frames);
    }
}

#[test]
fn pipelined_position_ramps_preserve_fragmentation_screens_origin_and_zero_allocations() {
    let _lock = TEST_LOCK.lock().unwrap();
    use mradm_dsp::room::screen::{RoomScreenGeometry, RoomScreens};
    for rate in [48000, 96000] {
        let cpu = common::fixture_at_rate(Quality::Realtime, rate);
        let gpu = adapted(&cpu);
        let descriptions: Vec<_> = (0..4)
            .map(|i| RoomDescription {
                id: i + 1,
                role: if i == 3 { 2 } else { 0 },
                position: [0.1, 0.8, 0.],
                screens: RoomScreens {
                    enabled: u32::from(i == 1),
                    reproduction: RoomScreenGeometry {
                        width: 42.,
                        ..Default::default()
                    },
                    ..Default::default()
                },
                ..Default::default()
            })
            .collect();
        let initial: Vec<_> = (0..4)
            .map(|i| RoomCommand {
                element: i,
                state: State {
                    position: [0.1, 0.8, 0.],
                    screen_reference: 1,
                    diffuse: 0.2,
                    extent: [0.08, 0.02, 0.],
                    ..State::default()
                },
                ..Default::default()
            })
            .collect();
        let mut events = Vec::new();
        for (j, at) in [0, 1, 127, 255, 256, 257, 511, 777, 1023, 1536, 2047, 3073]
            .into_iter()
            .enumerate()
        {
            for source in 0..3 {
                events.push((
                    at,
                    RoomCommand {
                        element: source,
                        changed: 4,
                        cleared: if j == 8 { 4 } else { 0 },
                        duration: [0, 1, 128, 512][j % 4],
                        state: State {
                            position: if j == 5 {
                                [0.; 3]
                            } else {
                                [
                                    0.7 * (j as f32 * 0.3).sin(),
                                    0.4 * (j as f32 * 0.2).cos(),
                                    0.1,
                                ]
                            },
                            ..State::default()
                        },
                        ..RoomCommand::default()
                    },
                ));
            }
        }
        let input: Vec<_> = (0..4096)
            .map(|i| {
                if i < 512 {
                    0.
                } else {
                    (i % 179) as f32 * 0.00001 - 0.00089
                }
            })
            .collect();
        let render = |prepared: Arc<PreparedSet>, packets: &[usize], checked: bool| {
            let mut session = Session::new(prepared, 1.5, true, true).unwrap();
            session.configure(&descriptions).unwrap();
            session.set_pose([17., 3., 0.]).unwrap();
            let mut output = vec![0.; input.len() * 2];
            let mut commands = Vec::with_capacity(events.len());
            let (mut at, mut packet) = (0, 0);
            while at < input.len() {
                let n = packets[packet % packets.len()].min(input.len() - at);
                commands.clear();
                for (time, command) in &events {
                    if *time >= at && *time < at + n {
                        let mut c = *command;
                        c.offset = (*time - at) as u32;
                        commands.push(c);
                    }
                }
                ALLOCS.store(0, Ordering::SeqCst);
                CHECK.store(checked, Ordering::SeqCst);
                let result = session.process(
                    n,
                    (0..4).map(|_| Some(&input[at..at + n])),
                    if at == 0 { &initial } else { &[] },
                    &commands,
                    &mut output[at * 2..(at + n) * 2],
                );
                CHECK.store(false, Ordering::SeqCst);
                result.unwrap();
                if checked {
                    assert_eq!(ALLOCS.load(Ordering::SeqCst), 0);
                }
                at += n;
                packet += 1;
            }
            session.end_input();
            let tail = session.info().tail_frames as usize;
            let mut block = [0.; 1024];
            for _ in 0..tail.div_ceil(512) + 1 {
                session
                    .process(512, (0..4).map(|_| None), &[], &[], &mut block)
                    .unwrap();
                output.extend_from_slice(&block);
            }
            output
        };
        let baseline = render(cpu, &[512], false);
        let canonical = render(gpu.clone(), &[512], true);
        let fragmented = render(gpu, &[1, 137, 375, 511, 257, 19], true);
        assert_eq!(canonical.len(), baseline.len());
        assert!(
            canonical
                .iter()
                .map(|v| v.to_bits())
                .eq(fragmented.iter().map(|v| v.to_bits()))
        );
        let energy: f64 = baseline.iter().map(|v| f64::from(*v).powi(2)).sum();
        let error: f64 = baseline
            .iter()
            .zip(&canonical)
            .map(|(a, b)| (f64::from(*a) - f64::from(*b)).powi(2))
            .sum();
        assert!((error / energy).sqrt() <= 2e-5);
        assert!(canonical.iter().all(|v| v.is_finite()));
        assert!(canonical[..512 * 2].iter().all(|v| *v == 0.));
    }
}

fn tuned(base: &Arc<PreparedSet>) -> Arc<PreparedSet> {
    Arc::new(
        base.with_gpu_experiment(Arc::new(ReferenceFactory))
            .unwrap()
            .with_gpu_tuning(true, 4)
            .unwrap(),
    )
}
#[test]
fn staggered_long_eq_preserves_forks_reset_restart_and_complete_switch_tail() {
    let _lock = TEST_LOCK.lock().unwrap();
    for rate in [48000, 96000] {
        let original = common::fixture_at_rate(Quality::Realtime, rate);
        let mut profile = (*original.room.profile).clone();
        profile.eq = (0..32768)
            .map(|i| {
                if i == 0 {
                    0.7
                } else {
                    libm::cos(i as f64 * 0.037) as f32 * 0.0002 / (1. + i as f32 * 0.0005)
                }
            })
            .collect();
        let prepare = |profile| {
            Arc::new(
                PreparedSet::new(Arc::new(
                    PreparedRoom::with_quality(
                        Arc::new(profile),
                        original.room.hrir.clone(),
                        Quality::Realtime,
                    )
                    .unwrap(),
                ))
                .unwrap(),
            )
        };
        let base = prepare(profile.clone());
        profile.identity.push_str("-different-eq");
        profile.eq[2173] += 0.03;
        profile.eq.extend_from_slice(&[0.01; 13]);
        profile.absorption = [[0.8; 8]; 6];
        let next = prepare(profile);
        let mut a = Session::new(base.clone(), 1.5, true, false).unwrap();
        let mut b = Session::new(tuned(&base), 1.5, true, false).unwrap();
        let descriptions: Vec<_> = (0..8)
            .map(|i| RoomDescription {
                id: i + 1,
                position: [i as f32 * 0.1, 0.5, 0.],
                ..Default::default()
            })
            .collect();
        a.configure(&descriptions).unwrap();
        b.configure(&descriptions).unwrap();
        let mut switch_a = Some(Box::new(Instance::new(next.clone(), 8).unwrap()));
        let mut switch_b = Some(Box::new(Instance::new(tuned(&next), 8).unwrap()));
        let mut input = [[0.; 512]; 8];
        let mut out_a = [0.; 1024];
        let mut out_b = [0.; 1024];
        let (mut energy, mut error, mut peak, mut maximum) = (0f64, 0f64, 0f64, 0f64);
        let mut phases = [false; 4];
        for block in 0..200 {
            if block == 16 {
                // Reset at a boundary shared by the phase-zero long partition.
                a.reset(4096).unwrap();
                b.reset(4096).unwrap();
            }
            if block == 40 {
                a.queue_room(switch_a.take().unwrap(), 1).unwrap();
                b.queue_room(switch_b.take().unwrap(), 1).unwrap();
            }
            let n = [512, 137, 375, 512][block % 4];
            for (source, row) in input.iter_mut().enumerate() {
                for (i, value) in row[..n].iter_mut().enumerate() {
                    *value = if block < source * 3 || (source == 3 && (20..150).contains(&block)) {
                        0.
                    } else {
                        ((block * 512 + i + source * 29) % 179) as f32 * 0.00001 - 0.00089
                    };
                }
            }
            a.set_pose([block as f32 * 0.3, 2., 0.]).unwrap();
            b.set_pose([block as f32 * 0.3, 2., 0.]).unwrap();
            a.process(
                n,
                input.iter().map(|row| Some(&row[..n])),
                &[],
                &[],
                &mut out_a[..n * 2],
            )
            .unwrap();
            ALLOCS.store(0, Ordering::SeqCst);
            CHECK.store(true, Ordering::SeqCst);
            let value = b.process(
                n,
                input.iter().map(|row| Some(&row[..n])),
                &[],
                &[],
                &mut out_b[..n * 2],
            );
            CHECK.store(false, Ordering::SeqCst);
            value.unwrap();
            assert_eq!(ALLOCS.load(Ordering::SeqCst), 0);
            phases[b.info().phase as usize] = true;
            assert_eq!(a.info().phase, b.info().phase);
            for (&x, &y) in out_a[..n * 2].iter().zip(&out_b[..n * 2]) {
                assert!(x.is_finite() && y.is_finite());
                energy += f64::from(x).powi(2);
                error += (f64::from(x) - f64::from(y)).powi(2);
                peak = peak.max(f64::from(x).abs());
                maximum = maximum.max((f64::from(x) - f64::from(y)).abs());
            }
            let _ = a.take_retired();
            let _ = b.take_retired();
        }
        assert!(
            phases.into_iter().all(|v| v),
            "did not cover all room phases"
        );
        a.end_input();
        b.end_input();
        assert_eq!(a.info().tail_frames, b.info().tail_frames);
        for _ in 0..(a.info().tail_frames as usize).div_ceil(512) + 2 {
            a.process(512, (0..8).map(|_| None), &[], &[], &mut out_a)
                .unwrap();
            b.process(512, (0..8).map(|_| None), &[], &[], &mut out_b)
                .unwrap();
            for (&x, &y) in out_a.iter().zip(&out_b) {
                assert!(x.is_finite() && y.is_finite());
                energy += f64::from(x).powi(2);
                error += (f64::from(x) - f64::from(y)).powi(2);
                peak = peak.max(f64::from(x).abs());
                maximum = maximum.max((f64::from(x) - f64::from(y)).abs());
            }
        }
        assert!(
            (error / energy).sqrt() <= 2e-5,
            "{rate}: {}",
            (error / energy).sqrt()
        );
        assert!(maximum / peak <= 2e-4);
        assert_eq!(a.info().latency_frames, b.info().latency_frames);
        assert_eq!(a.info().input_frames, b.info().input_frames);
    }
}
