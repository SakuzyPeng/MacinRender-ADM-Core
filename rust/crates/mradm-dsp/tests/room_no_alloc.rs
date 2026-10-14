use mradm_dsp::room::{
    BLOCK,
    control::{RoomCommand, RoomDescription, State},
    prepared::PreparedRoom,
    quality::Quality,
    screen::{RoomScreenGeometry, RoomScreens},
    session::{Instance, PreparedSet, Session},
};
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
#[path = "common/room_fixture.rs"]
mod common;

struct CheckedAllocator;
static CHECK: AtomicBool = AtomicBool::new(false);
static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);
unsafe impl GlobalAlloc for CheckedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if CHECK.load(Ordering::Relaxed) {
            ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe {
            System.dealloc(pointer, layout);
        }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        if CHECK.load(Ordering::Relaxed) {
            ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        }
        unsafe { System.realloc(pointer, layout, size) }
    }
}
#[global_allocator]
static ALLOCATOR: CheckedAllocator = CheckedAllocator;

#[test]
fn regular_room_dsp_does_not_allocate_on_any_worker() {
    for quality in [Quality::Realtime, Quality::Exact] {
        verify_quality(quality);
    }
}
fn verify_quality(quality: Quality) {
    ALLOCATIONS.store(0, Ordering::SeqCst);
    let base = if quality == Quality::Exact {
        common::fixture()
    } else {
        common::fixture_with_quality(quality)
    };
    let mut profile = (*base.room.profile).clone();
    profile.late_directions = (0..32)
        .map(|i| base.room.profile.late_directions[i % 6])
        .collect();
    profile.late = (0..32)
        .flat_map(|i| {
            base.room.profile.late[i % 6 * profile.late_taps..(i % 6 + 1) * profile.late_taps]
                .iter()
                .copied()
        })
        .collect();
    let prepared = Arc::new(
        PreparedSet::new(Arc::new(
            PreparedRoom::with_quality(Arc::new(profile.clone()), base.room.hrir.clone(), quality)
                .unwrap(),
        ))
        .unwrap(),
    );
    profile.identity.push_str("-changed");
    profile.absorption = [[0.9; 8]; 6];
    let other = Arc::new(
        PreparedSet::new(Arc::new(
            PreparedRoom::with_quality(Arc::new(profile), base.room.hrir.clone(), quality).unwrap(),
        ))
        .unwrap(),
    );
    let mut candidate = Some(Box::new(Instance::new(other, 8).unwrap()));
    let mut session = Session::new(prepared, 1.5, true, false).unwrap();
    let descriptions: Vec<_> = (0..8)
        .map(|id| RoomDescription {
            id,
            position: [0., 1., 0.],
            screens: RoomScreens {
                enabled: 1,
                reproduction: RoomScreenGeometry {
                    width: 90.,
                    ..RoomScreenGeometry::default()
                },
                ..RoomScreens::default()
            },
            ..RoomDescription::default()
        })
        .collect();
    session.configure(&descriptions).unwrap();
    let initial: Vec<_> = (0..8)
        .map(|element| RoomCommand {
            element,
            state: State {
                position: [-0.5, 0.8, 0.2],
                screen_reference: 1,
                ..State::default()
            },
            ..RoomCommand::default()
        })
        .collect();
    // Every source ramps with its own event offset, so voices and head-locked
    // paths segment on per-source update points (ADR 0019).
    let events: Vec<Vec<RoomCommand>> = (0..512)
        .map(|block| {
            let mut commands: Vec<_> = (0..8u32)
                .map(|element| RoomCommand {
                    element,
                    offset: (element * 37 + 11) % BLOCK as u32,
                    duration: BLOCK as u32,
                    changed: 4,
                    state: State {
                        position: if (block + element as usize).is_multiple_of(2) {
                            [-0.5, 0.8, 0.2]
                        } else {
                            [0.4, 0.9, -0.1]
                        },
                        ..State::default()
                    },
                    ..RoomCommand::default()
                })
                .collect();
            commands.sort_by_key(|c| c.offset);
            commands
        })
        .collect();
    let input = [0.01; BLOCK];
    let mut output = [0.; BLOCK * 2];
    let mut counts = [0; 512];
    CHECK.store(true, Ordering::SeqCst);
    for (block, count) in counts.iter_mut().enumerate() {
        // Exercise idle/wake transitions as well as continuous queued work.
        if block.is_multiple_of(16) {
            std::thread::sleep(std::time::Duration::from_millis(2));
        }
        if block == 64 {
            session.queue_room(candidate.take().unwrap(), 1).unwrap();
        }
        session.set_pose([block as f32 * 0.125, 0., 0.]).unwrap();
        session
            .process(
                BLOCK,
                (0..8).map(|_| Some(input.as_slice())),
                if block == 0 { &initial } else { &[] },
                &events[block],
                &mut output,
            )
            .unwrap();
        *count = ALLOCATIONS.load(Ordering::Relaxed);
    }
    CHECK.store(false, Ordering::SeqCst);
    for i in 0..512 {
        if counts[i] != if i == 0 { 0 } else { counts[i - 1] } {
            eprintln!("allocation count after block {i}: {}", counts[i]);
        }
    }
    assert_eq!(
        ALLOCATIONS.load(Ordering::SeqCst),
        0,
        "{quality:?}: processing, including first block, must use prepared workspaces"
    );
}
