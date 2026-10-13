use mradm_dsp::{
    geometry,
    room::{
        BLOCK,
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
fn reflection_pose_budget_preserves_direct_sound_and_bypasses_on_jumps_and_deadlines() {
    use mradm_dsp::room::{geometry as room_geometry, prepared::ResponseWork, signal::Emitter};
    let prepared = common::fixture_with_quality(Quality::Realtime);
    let room = &prepared.room;
    let position =
        room_geometry::position(&room.profile, [0., 1.4, 0.], 1.5, [1., 0., 0.]).unwrap();
    let sources = [room_geometry::WeightedPosition {
        position,
        weight: 1.,
    }];
    let mut candidate = Emitter::new(room.clone(), position).unwrap();
    let mut direct_reference = Emitter::new(room.clone(), position).unwrap();
    let mut early_reference = Emitter::new(room.clone(), position).unwrap();
    let mut work = ResponseWork::new(room).unwrap();
    let mut direct_work = ResponseWork::new(room).unwrap();
    let mut early_work = ResponseWork::new(room).unwrap();
    let limit = room.quality.reflection_pose_degrees() as f32;
    assert!(limit > 0.);
    let poses = [
        0.,
        0.4 * limit,
        0.8 * limit,
        1.4 * limit,
        1.6 * limit,
        1.8 * limit,
        -40.,
        -40. + 0.4 * limit,
        -40. + 0.8 * limit,
    ];
    let held = [
        0.,
        0.,
        0.,
        1.4 * limit,
        1.4 * limit,
        1.8 * limit,
        -40.,
        -40.,
        -40.,
    ];
    for (step, (&pose, &held)) in poses.iter().zip(&held).enumerate() {
        candidate
            .update_sources_with_pose_budget(
                position,
                &sources,
                [pose, 7., -3.],
                false,
                true,
                false,
                step != 5,
                &mut work,
            )
            .unwrap();
        direct_reference
            .update(position, [pose, 7., -3.], &mut direct_work)
            .unwrap();
        early_reference
            .update(position, [held, 7., -3.], &mut early_work)
            .unwrap();
        let input: Vec<_> = (0..BLOCK)
            .map(|i| ((i * 31 + step * 17) % 127) as f32 / 4096.)
            .collect();
        let render = |emitter: &mut Emitter| {
            let mut direct = [0.; 2 * BLOCK];
            let mut early = [0.; 2 * BLOCK];
            let mut send = [0.; BLOCK];
            emitter
                .process(&input, &[1.; BLOCK], &mut direct, &mut early, &mut send)
                .unwrap();
            (direct, early, send)
        };
        let actual = render(&mut candidate);
        let precise = render(&mut direct_reference);
        let expected_early = render(&mut early_reference);
        assert!(bits(&actual.0, &precise.0), "direct changed at {step}");
        assert!(
            bits(&actual.1, &expected_early.1),
            "reflection target differs at {step}"
        );
        assert!(
            bits(&actual.2, &precise.2),
            "head pose changed the late send at {step}"
        );
    }
}

#[test]
fn small_head_motion_is_independent_of_packetization_and_finite_preroll() {
    let prepared = common::fixture_with_quality(Quality::Realtime);
    let descriptions: Vec<_> = (1..=8)
        .map(|id| RoomDescription {
            id,
            position: around(id as f32 * 40.),
            ..Default::default()
        })
        .collect();
    let goal = (prepared.preroll_frames().div_ceil(2048) + 2) * 2048;
    let end = goal + 4096;
    let pcm: Vec<_> = (0..end)
        .map(|i| ((i * 31 % 127) as f32 - 63.) / 512.)
        .collect();
    let pose_at = |sample: usize| {
        let step = if sample < 17 {
            0
        } else {
            1 + (sample - 17) / 173
        } as f32;
        [
            ((179.85 + step * 0.08 + 180.) % 360.) - 180.,
            20. + step * 0.02,
            -9. + step * 0.03,
        ]
    };
    let render = |begin: usize, chunks: &[usize]| {
        let mut session = Session::new(prepared.clone(), 1.5, true, true).unwrap();
        session.configure(&descriptions).unwrap();
        session.reset(begin as u64).unwrap();
        let mut at = begin;
        let mut sequence = 0;
        let mut result = Vec::new();
        let mut output = vec![0.; 8192];
        while at < end {
            if at == begin || (at >= 17 && (at - 17).is_multiple_of(173)) {
                session.set_pose(pose_at(at)).unwrap();
            }
            let next_pose = if at < 17 {
                17
            } else {
                17 + ((at - 17) / 173 + 1) * 173
            };
            let count = chunks[sequence % chunks.len()]
                .min(next_pose - at)
                .min(end - at);
            session
                .process(
                    count,
                    (0..descriptions.len()).map(|_| Some(&pcm[at..at + count])),
                    &[],
                    &[],
                    &mut output[..2 * count],
                )
                .unwrap();
            result.extend_from_slice(&output[..2 * count]);
            at += count;
            sequence += 1;
        }
        result
    };
    let full = render(0, &[4096]);
    assert!(bits(&full, &render(0, &[1, 71, 243, 19, 1024])));
    let begin = (goal - prepared.preroll_frames()) / 2048 * 2048;
    assert!(begin > 0);
    assert!(bits(
        &full[2 * goal..],
        &render(begin, &[71, 256, 1024])[2 * (goal - begin)..]
    ));
}

#[test]
fn small_pose_steps_keep_the_reference_geometry_catch_up() {
    use mradm_dsp::room::{geometry as room_geometry, prepared::ResponseWork, signal::Emitter};
    let prepared = common::fixture_with_quality(Quality::Realtime);
    let room = &prepared.room;
    let position = |step: usize| {
        room_geometry::position(
            &room.profile,
            [step as f32 * 0.015, 1.4, 0.],
            1.5,
            [1., 0., 0.],
        )
        .unwrap()
    };
    let mut reference = Emitter::new(room.clone(), position(0)).unwrap();
    let mut candidate = Emitter::new(room.clone(), position(0)).unwrap();
    let mut old_work = ResponseWork::new(room).unwrap();
    let mut work = ResponseWork::new(room).unwrap();
    for step in 0..24 {
        let p = position(step);
        let sources = [room_geometry::WeightedPosition {
            position: p,
            weight: 1.,
        }];
        let pose = [
            (step / 2) as f32 * room.quality.reflection_pose_degrees() as f32 * 0.2,
            7.,
            -3.,
        ];
        reference
            .update_sources(p, &sources, pose, false, true, true, &mut old_work)
            .unwrap();
        candidate
            .update_sources_with_pose_budget(p, &sources, pose, false, true, true, true, &mut work)
            .unwrap();
        let input: Vec<_> = (0..BLOCK)
            .map(|i| ((i + step * BLOCK) % 179) as f32 / 4096.)
            .collect();
        let render = |emitter: &mut Emitter| {
            let mut direct = [0.; 2 * BLOCK];
            let mut early = [0.; 2 * BLOCK];
            let mut send = [0.; BLOCK];
            emitter
                .process(&input, &[1.; BLOCK], &mut direct, &mut early, &mut send)
                .unwrap();
            (direct, early, send)
        };
        let a = render(&mut reference);
        let b = render(&mut candidate);
        assert!(
            bits(&a.0, &b.0) && bits(&a.1, &b.1) && bits(&a.2, &b.2),
            "step {step}"
        );
    }
}

#[test]
fn branch_updates_clear_previous_buckets_and_match_full_responses() {
    use mradm_dsp::room::{geometry as room_geometry, prepared::ResponseWork};
    for quality in [Quality::Exact, Quality::Realtime] {
        let prepared = common::fixture_with_quality(quality);
        let room = &prepared.room;
        let mut selected = ResponseWork::new(room).unwrap();
        let mut full = ResponseWork::new(room).unwrap();
        for step in 0..48 {
            let rotation = room_geometry::Rotation::new([step as f32 * 7., -13., 2.]).unwrap();
            let position = |radius: f32, angle: f32| {
                let d = geometry::portable_direction(angle, 9.);
                room_geometry::position(
                    &room.profile,
                    [
                        (-d[1] as f32) * radius,
                        (d[0] as f32) * radius,
                        (d[2] as f32) * radius,
                    ],
                    1.5,
                    [1., 0., 0.],
                )
                .unwrap()
            };
            // Alternate sparse delay buckets, full silence, and multi-point
            // extent-like inputs, then revisit both branch caches independently.
            let sources = [
                room_geometry::WeightedPosition {
                    position: position([0.25, 3.0, 0.8, 1.9][step % 4], step as f32 * 31.),
                    weight: if step % 7 == 0 { 0. } else { 0.7 },
                },
                room_geometry::WeightedPosition {
                    position: position(1.2, step as f32 * 31. + 20.),
                    weight: 0.3,
                },
            ];
            let sources = &sources[..if step % 3 == 0 { 2 } else { 1 }];
            full.build_many(room, sources, &rotation).unwrap();
            let (direct, early) =
                [(true, true), (true, false), (false, true), (false, false)][step % 4];
            selected
                .build_branches(room, sources, &rotation, direct, early)
                .unwrap();
            for (branch, enabled) in [direct, early].into_iter().enumerate() {
                let range = branch * 2 * room.short_taps..(branch + 1) * 2 * room.short_taps;
                if enabled {
                    assert!(
                        bits(&selected.impulse[range.clone()], &full.impulse[range]),
                        "{quality:?}: branch {branch}, step {step}"
                    );
                } else {
                    assert!(selected.impulse[range].iter().all(|v| v.to_bits() == 0));
                }
            }
            assert_eq!(
                selected.direct_support,
                if direct { full.direct_support } else { 1 }
            );
            assert_eq!(
                selected.early_support,
                if early { full.early_support } else { 1 }
            );
        }
    }
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
                // Small head steps exercise the reflection budget through the off-block switch and EOS.
                session.set_pose([at as f32 * 0.0002, 7., -3.]).unwrap();
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

#[test]
fn short_room_switch_preserves_ramps_on_each_reflection_phase() {
    let input: Vec<_> = (0..70 * BLOCK)
        .map(|i| ((i * 37 % 193) as f32 - 96.) / 384.)
        .collect();
    for quality in [Quality::Exact, Quality::Realtime] {
        let base = common::fixture_with_quality(quality);
        let mut profile = (*base.room.profile).clone();
        profile.identity.push_str("-short");
        profile.dimensions = [1.; 3];
        profile.listener = [0.5; 3];
        profile.minimum_distance = 0.05;
        profile.clearance = 0.025;
        profile.early_fade = [0.001, 0.004];
        profile.absorption = [[0.5; 8]; 6];
        profile.eq = vec![1.];
        profile.late_energy = 0.; // Isolate the direct and early-reflection transition.
        let prepare = |profile| {
            Arc::new(
                PreparedSet::new(Arc::new(
                    PreparedRoom::with_quality(profile, base.room.hrir.clone(), quality).unwrap(),
                ))
                .unwrap(),
            )
        };
        let prepared = prepare(Arc::new(profile.clone()));
        assert!(prepared.room.short_taps < Quality::Realtime.reflection_frames());
        // A distinct identity exercises the complete switch with identical acoustics.
        profile.identity.push_str("-same-acoustics");
        let next = prepare(Arc::new(profile));
        // These source identities cover the four staggered reflection phases.
        for id in [6, 1, 2, 7] {
            let render = |switch: Option<usize>, chunks: &[usize]| {
                let mut session = Session::new(prepared.clone(), 1., true, true).unwrap();
                session
                    .configure(&[RoomDescription {
                        id,
                        position: [0., 0.3, 0.],
                        ..Default::default()
                    }])
                    .unwrap();
                let mut candidate =
                    switch.map(|_| Box::new(Instance::new(next.clone(), 1).unwrap()));
                let command = RoomCommand {
                    changed: 4,
                    duration: 9000,
                    state: State {
                        position: [-0.3, 0.2, 0.1],
                        ..Default::default()
                    },
                    ..Default::default()
                };
                let mut output = vec![0.; input.len() * 2];
                let mut phases = [false; 4];
                let mut at = 0;
                let mut packet = 0;
                while at < input.len() {
                    if switch == Some(at) {
                        session.queue_room(candidate.take().unwrap(), 1).unwrap();
                    }
                    let boundary = switch.filter(|sample| *sample > at).unwrap_or(input.len());
                    let count = chunks[packet % chunks.len()].min(boundary - at);
                    let commands = [command];
                    let info = session
                        .process(
                            count,
                            [Some(&input[at..at + count])].into_iter(),
                            &[],
                            if at == 0 { &commands } else { &[] },
                            &mut output[at * 2..(at + count) * 2],
                        )
                        .unwrap();
                    phases[info.phase as usize] = true;
                    drop(session.take_retired());
                    at += count;
                    packet += 1;
                }
                if switch.is_some() {
                    assert!(phases.into_iter().all(|phase| phase));
                    assert_eq!(session.info().applied_request, 1);
                    assert_eq!(session.info().phase, 0);
                }
                output
            };
            let reference = render(None, &[BLOCK]);
            assert!(reference.iter().any(|sample| sample.abs() > 1e-5));
            for (switch, chunks) in [
                (5 * BLOCK, &[BLOCK][..]),
                (5 * BLOCK + 173, &[1, 173, 733, 19, 257][..]),
            ] {
                let actual = render(Some(switch), chunks);
                let maximum = reference
                    .iter()
                    .zip(actual)
                    .map(|(a, b)| (*a - b).abs())
                    .fold(0., f32::max);
                assert!(
                    maximum < 2e-6,
                    "{quality:?} source {id}, switch at {switch}: PCM changed by {maximum}"
                );
            }
        }
    }
}
