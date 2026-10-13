use mradm_dsp::{
    geometry,
    room::{
        control::{RoomCommand, RoomDescription, State},
        prepared::PreparedRoom,
        quality::Quality,
        screen::{RoomScreenGeometry, RoomScreens},
        session::{Instance, PreparedSet, Session},
    },
};
use std::sync::Arc;
#[path = "common/room_fixture.rs"]
mod common;

fn other(base: &Arc<PreparedSet>) -> Arc<PreparedSet> {
    let mut profile = (*base.room.profile).clone();
    profile.identity.push_str("-quality-switch");
    profile.absorption = [[0.9; 8]; 6];
    Arc::new(
        PreparedSet::new(Arc::new(
            PreparedRoom::with_quality(
                Arc::new(profile),
                base.room.hrir.clone(),
                base.room.quality,
            )
            .unwrap(),
        ))
        .unwrap(),
    )
}
fn around(angle: f32) -> [f32; 3] {
    let v = geometry::portable_direction(angle, 9.);
    [
        (-v[1] * 1.4) as f32,
        (v[0] * 1.4) as f32,
        (v[2] * 1.4) as f32,
    ]
}
fn bits(a: &[f32], b: &[f32]) -> bool {
    a.len() == b.len() && a.iter().zip(b).all(|(a, b)| a.to_bits() == b.to_bits())
}

#[test]
fn quality_is_fixed_and_incompatible_prepared_banks_are_rejected() {
    let exact = common::fixture();
    let fast = common::fixture_with_quality(Quality::Realtime);
    assert!(Quality::try_from(2).is_err());
    assert!("unknown".parse::<Quality>().is_err());
    assert!(
        PreparedRoom::with_quality(
            exact.room.profile.clone(),
            exact.room.hrir.clone(),
            Quality::Realtime
        )
        .is_err()
    );
    assert!(PreparedRoom::new(fast.room.profile.clone(), fast.room.hrir.clone()).is_err());
    let mut session = Session::new(exact, 1.5, true, false).unwrap();
    assert!(
        session
            .queue_room(Box::new(Instance::new(fast, 0).unwrap()), 1)
            .is_err()
    );
}

#[test]
fn realtime_ramps_switches_and_tail_are_independent_of_packetization() {
    let prepared = common::fixture_with_quality(Quality::Realtime);
    let next = other(&prepared);
    let descriptions: Vec<_> = (0..8)
        .map(|i| RoomDescription {
            id: i + 1,
            position: around(i as f32 * 45.),
            ..Default::default()
        })
        .collect();
    let initial: Vec<_> = descriptions
        .iter()
        .enumerate()
        .map(|(i, d)| RoomCommand {
            element: i as u32,
            state: State {
                position: d.position,
                head_locked: u32::from(i == 1),
                ..Default::default()
            },
            ..Default::default()
        })
        .collect();
    let total = 18_517;
    let mut events = Vec::new();
    for block in 0..(total / 512) {
        for (source, command) in initial.iter().enumerate().take(8) {
            events.push((
                block * 512 + (source * 131 + 17) % 512,
                RoomCommand {
                    element: source as u32,
                    changed: 4,
                    duration: 512,
                    state: State {
                        position: around(source as f32 * 45. + block as f32 * 3.),
                        ..command.state
                    },
                    ..Default::default()
                },
            ));
        }
    }
    events.sort_by_key(|v| v.0);
    let render = |chunks: &[usize]| {
        let mut session = Session::new(prepared.clone(), 1.5, true, true).unwrap();
        session.configure(&descriptions).unwrap();
        let mut candidate = Some(Box::new(Instance::new(next.clone(), 8).unwrap()));
        let mut output = Vec::new();
        let mut at = 0;
        let mut sequence = 0;
        while at < total {
            if at == 2049 {
                session.queue_room(candidate.take().unwrap(), 1).unwrap();
            }
            if at % 512 == 0 {
                session.set_pose([at as f32 * 0.002, 7., -3.]).unwrap();
            }
            let boundary = [2049, total, (at / 512 + 1) * 512]
                .into_iter()
                .filter(|v| *v > at)
                .min()
                .unwrap();
            let count = chunks[sequence % chunks.len()].min(boundary - at);
            sequence += 1;
            let input: Vec<_> = (at..at + count)
                .map(|i| ((i * 37 % 193) as f32 - 96.) / 384.)
                .collect();
            let commands: Vec<_> = events
                .iter()
                .filter(|(n, _)| (at..at + count).contains(n))
                .map(|(n, c)| RoomCommand {
                    offset: (n - at) as u32,
                    ..*c
                })
                .collect();
            let mut block = vec![0.; count * 2];
            session
                .process(
                    count,
                    (0..8).map(|_| Some(input.as_slice())),
                    if at == 0 { &initial } else { &[] },
                    &commands,
                    &mut block,
                )
                .unwrap();
            output.extend(block);
            at += count;
            drop(session.take_retired());
        }
        session.end_input();
        let mut tail = session.info().tail_frames as usize;
        while tail != 0 {
            let count = tail.min(chunks[sequence % chunks.len()]);
            sequence += 1;
            let mut block = vec![0.; count * 2];
            session
                .process(count, [None; 8].into_iter(), &[], &[], &mut block)
                .unwrap();
            output.extend(block);
            tail -= count;
            drop(session.take_retired());
        }
        assert_eq!(session.info().applied_request, 1);
        output
    };
    let a = render(&[512]);
    let b = render(&[1, 173, 733, 4096, 19, 257]);
    assert!(bits(&a, &b));
    assert!(a.iter().any(|v| v.abs() > 1e-5));
}

#[test]
fn hard_geometry_events_bypass_the_grid_while_another_field_is_ramping() {
    let prepared = common::fixture_with_quality(Quality::Realtime);
    let description = RoomDescription {
        id: 7,
        position: [0., 1., 0.],
        screens: RoomScreens {
            enabled: 1,
            reproduction: RoomScreenGeometry {
                width: 90.,
                ..Default::default()
            },
            ..Default::default()
        },
        ..Default::default()
    };
    let render = |dummy_ramp: bool| {
        let mut session = Session::new(prepared.clone(), 1.5, true, true).unwrap();
        session.configure(&[description]).unwrap();
        let initial = State {
            position: [-0.5, 0.9, 0.1],
            ..Default::default()
        };
        let mut events = vec![
            RoomCommand {
                offset: 173,
                changed: 4,
                state: State {
                    position: [0.7, 0.9, 0.1],
                    ..initial
                },
                ..Default::default()
            },
            RoomCommand {
                offset: 701,
                changed: 128,
                state: State {
                    position: [0.7, 0.9, 0.1],
                    screen_reference: 1,
                    ..initial
                },
                ..Default::default()
            },
            RoomCommand {
                offset: 1091,
                cleared: 4,
                duration: 397,
                state: initial,
                ..Default::default()
            },
            RoomCommand {
                offset: 2113,
                changed: 256,
                state: State {
                    head_locked: 1,
                    ..initial
                },
                ..Default::default()
            },
        ];
        if dummy_ramp {
            events.push(RoomCommand {
                changed: 8,
                duration: 5000,
                state: initial,
                ..Default::default()
            });
        }
        events.sort_by_key(|c| c.offset);
        let input: Vec<_> = (0..8192)
            .map(|i| ((i * 53 % 211) as f32 - 105.) / 512.)
            .collect();
        let mut output = vec![0.; input.len() * 2];
        session
            .process(
                input.len(),
                [Some(input.as_slice())].into_iter(),
                &[RoomCommand {
                    state: initial,
                    ..Default::default()
                }],
                &events,
                &mut output,
            )
            .unwrap();
        output
    };
    assert!(bits(&render(false), &render(true)));
}

#[test]
fn realtime_preroll_reproduces_continuous_ramps_on_each_source_phase() {
    let prepared = common::fixture_with_quality(Quality::Realtime);
    for id in 1..=8 {
        let mut full = Session::new(prepared.clone(), 1.5, true, true).unwrap();
        let mut seek = Session::new(prepared.clone(), 1.5, true, true).unwrap();
        let d = RoomDescription {
            id,
            position: [0., 1., 0.],
            ..Default::default()
        };
        full.configure(&[d]).unwrap();
        seek.configure(&[d]).unwrap();
        let command = RoomCommand {
            changed: 4,
            duration: 16000,
            state: State {
                position: [-0.7, 0.8, 0.2],
                ..Default::default()
            },
            ..Default::default()
        };
        let goal = 12000usize;
        let begin = (goal - prepared.preroll_frames()) / 2048 * 2048;
        assert!(begin > 0);
        seek.reset(begin as u64).unwrap();
        seek.prime(&command, 0).unwrap();
        let input: Vec<_> = (0..15000)
            .map(|i| ((i * 31 % 127) as f32 - 63.) / 256.)
            .collect();
        let mut a = vec![0.; input.len() * 2];
        let mut b = vec![0.; (input.len() - begin) * 2];
        full.process(
            input.len(),
            [Some(input.as_slice())].into_iter(),
            &[],
            &[command],
            &mut a,
        )
        .unwrap();
        seek.process(
            input.len() - begin,
            [Some(&input[begin..])].into_iter(),
            &[],
            &[],
            &mut b,
        )
        .unwrap();
        assert!(
            bits(&a[goal * 2..], &b[(goal - begin) * 2..]),
            "source {id} preroll differs"
        );
    }
}
