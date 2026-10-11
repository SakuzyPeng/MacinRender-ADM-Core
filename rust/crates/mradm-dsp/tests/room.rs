//! Synthetic physical and lifecycle tests. Personal reference data stays outside
//! the repository and is checked by scripts/room/validate-reference.py.
use mradm_dsp::{
    geometry,
    room::{
        BLOCK,
        control::{RoomCommand, RoomDescription, State},
        geometry::Rotation,
        prepared::PreparedRoom,
        session::{Instance, PreparedSet, Session},
    },
};
use std::sync::Arc;

#[path = "common/room_fixture.rs"]
mod common;
use common::fixture;

#[test]
fn shared_world_decoder_preserves_both_tails_under_changing_pose() {
    use mradm_dsp::room::{hrir::HrirBank, resources::HrirData, signal::WorldTail};
    let base = fixture();
    let data = &base.room.hrir.data;
    let mut hrir = HrirData {
        identity: "directional-test-hrir".into(),
        source_sofa_sha256: None,
        impulses: data.impulses.clone(),
        taps: data.taps,
        directions: data.directions.clone(),
        faces: data.faces.clone(),
        delays: data.delays.clone(),
        guard: data.guard,
        output_taps: data.output_taps,
    };
    for (direction, response) in hrir.impulses.chunks_exact_mut(hrir.taps * 2).enumerate() {
        for value in response {
            *value *= 0.5 + direction as f32 * 0.25;
        }
    }
    let (length, _, _) = PreparedRoom::required_fft(&base.room.profile, &hrir).unwrap();
    let bank = Arc::new(HrirBank::new(Arc::new(hrir), length).unwrap());
    let a = Arc::new(PreparedRoom::new(base.room.profile.clone(), bank.clone()).unwrap());
    let mut profile = (*a.profile).clone();
    for (index, value) in profile.late.iter_mut().enumerate() {
        *value *= 0.2 + (index % 7) as f32 * 0.1;
    }
    let b = Arc::new(PreparedRoom::new(Arc::new(profile), bank).unwrap());
    let mut reference_a = WorldTail::new(a.clone()).unwrap();
    let mut reference_b = WorldTail::new(b.clone()).unwrap();
    let mut field_a = WorldTail::new_field(a.clone()).unwrap();
    let mut field_b = WorldTail::new_field(b).unwrap();
    let mut decoder = WorldTail::new_decoder(a.clone()).unwrap();
    let mut world_a = vec![0.; a.profile.late_directions.len() * BLOCK];
    let mut world_b = world_a.clone();
    let mut x = [0.; 2 * BLOCK];
    let mut y = x;
    let mut actual = x;
    let mut maximum = 0f32;
    let mut tail_energy = 0f64;
    for block in 0..24 {
        let mut input_a = [0.; BLOCK];
        let mut input_b = input_a;
        if block < 3 {
            input_a[block * 17] = 0.75;
            input_b[block * 23] = -0.25;
        }
        let pose = [block as f32 * 13. - 150., 23., -17.];
        reference_a.update(pose).unwrap();
        reference_b.update(pose).unwrap();
        reference_a.process(&input_a, &mut x).unwrap();
        reference_b.process(&input_b, &mut y).unwrap();
        field_a.process_field(&input_a, &mut world_a).unwrap();
        field_b.process_field(&input_b, &mut world_b).unwrap();
        for (value, other) in world_a.iter_mut().zip(&world_b) {
            *value += *other;
        }
        decoder.update(pose).unwrap();
        decoder.begin_world(&world_a).unwrap();
        decoder.render_range(0, BLOCK, &mut actual).unwrap();
        for i in 0..actual.len() {
            maximum = maximum.max((actual[i] - (x[i] + y[i])).abs());
            if block >= 3 {
                tail_energy += f64::from(actual[i]) * f64::from(actual[i]);
            }
        }
    }
    assert!(maximum < 1e-7, "shared late decoder error {maximum}");
    assert!(
        tail_energy > 1e-5,
        "head updates must retain both room tails"
    );
}

#[test]
fn near_source_keeps_the_complete_material_filter_support() {
    let base = fixture();
    let mut profile = (*base.room.profile).clone();
    profile.band_taps = 513;
    profile.band_offset = 256;
    profile.bands = vec![0.; 8 * 513];
    profile.bands[0] = 1.;
    profile.absorption = [[1.; 8]; 6];
    profile.late_energy = 0.;
    profile.eq = vec![1.];
    let (fft, _, _) = PreparedRoom::required_fft(&profile, &base.room.hrir.data).unwrap();
    let bank =
        Arc::new(mradm_dsp::room::hrir::HrirBank::new(base.room.hrir.data.clone(), fft).unwrap());
    let room = Arc::new(PreparedRoom::new(Arc::new(profile), bank).unwrap());
    let position =
        mradm_dsp::room::geometry::position(&room.profile, [0., 0.25, 0.], 1., [1., 0., 0.])
            .unwrap();
    let impulse = mradm_dsp::room::signal::impulse(room, position, [0.; 3]).unwrap();
    let dc = impulse
        .chunks_exact(2)
        .map(|v| f64::from(v[0]))
        .sum::<f64>();
    assert!(
        (dc - 8.4).abs() < 1e-4,
        "material pre-support must not wrap or disappear: {dc}"
    );
}

#[test]
fn propagation_beyond_the_query_fft_does_not_wrap() {
    let base = fixture();
    let mut profile = (*base.room.profile).clone();
    profile.dimensions = [50., 50., 3.2];
    profile.listener = [25., 25., 1.6];
    profile.absorption = [[1.; 8]; 6];
    profile.late_energy = 0.;
    profile.eq = vec![1.];
    let (fft, _, _) = PreparedRoom::required_fft(&profile, &base.room.hrir.data).unwrap();
    let bank =
        Arc::new(mradm_dsp::room::hrir::HrirBank::new(base.room.hrir.data.clone(), fft).unwrap());
    let room = Arc::new(PreparedRoom::new(Arc::new(profile), bank).unwrap());
    let position =
        mradm_dsp::room::geometry::position(&room.profile, [0., 15., 0.], 1., [1., 0., 0.])
            .unwrap();
    let expected = 15. * 48000. / room.profile.speed
        + 40.
        + room.profile.alignment_guard as f64
        + room.hrir.data.guard as f64
        + 2.;
    assert!(expected > room.fft_length as f64);
    let impulse = mradm_dsp::room::signal::impulse(room, position, [0.; 3]).unwrap();
    let peak = impulse
        .chunks_exact(2)
        .enumerate()
        .max_by(|(_, a), (_, b)| a[0].abs().total_cmp(&b[0].abs()))
        .unwrap()
        .0;
    assert!((peak as f64 - expected).abs() < 1.);
    let gain: f64 = impulse.chunks_exact(2).map(|v| f64::from(v[0])).sum();
    assert!((gain - 2.1 / 15.).abs() < 1e-6, "long-delay gain {gain}");
}

#[test]
fn a_new_generation_with_the_same_id_leaves_the_old_tail_in_place() {
    let prepared = fixture();
    let render = |replace: bool| {
        let mut session = Session::new(prepared.clone(), 1.5, true, false).unwrap();
        let mut description = RoomDescription {
            id: 9,
            position: [0., 1., 0.],
            ..RoomDescription::default()
        };
        session.configure(&[description]).unwrap();
        let mut input = [0.; BLOCK];
        input[0] = 1.;
        let mut output = [0.; BLOCK * 2];
        let mut result = Vec::new();
        for block in 0..32 {
            if block == 1 && replace {
                description.generation = 1;
                description.position = [1., 0., 0.];
                session.configure(&[description]).unwrap();
            }
            session
                .process(
                    BLOCK,
                    std::iter::once(Some(input.as_slice())),
                    &[],
                    &[],
                    &mut output,
                )
                .unwrap();
            result.extend_from_slice(&output);
            input.fill(0.);
        }
        result
    };
    assert_eq!(render(false), render(true));
}

#[test]
fn real_distance_propagation_and_room_boundary() {
    use mradm_dsp::room::{geometry as room_geometry, signal};
    let base = fixture();
    let mut profile = (*base.room.profile).clone();
    profile.absorption = [[1.; 8]; 6];
    profile.late_energy = 0.;
    profile.eq = vec![1.];
    let room = Arc::new(PreparedRoom::new(Arc::new(profile), base.room.hrir.clone()).unwrap());
    for radius in [0.75f32, 1.5, 3.] {
        let position =
            room_geometry::position(&room.profile, [0., radius, 0.], 1., [1., 0., 0.]).unwrap();
        assert!(!position.constrained);
        let impulse = signal::impulse(room.clone(), position, [0.; 3]).unwrap();
        let left: Vec<_> = impulse.chunks_exact(2).map(|v| v[0]).collect();
        let peak = left
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.abs().total_cmp(&b.1.abs()))
            .unwrap()
            .0;
        let expected = f64::from(radius) / room.profile.speed * 48000. + 178.;
        assert!(
            (peak as f64 - expected).abs() < 0.6,
            "radius={radius},arrival={peak},expected={expected}"
        );
        let dc = left.iter().map(|v| f64::from(*v)).sum::<f64>();
        assert!((dc * f64::from(radius) / 2.1 - 1.).abs() < 1e-5);
    }
    let wall = room_geometry::position(&room.profile, [0., 100., 0.], 1., [1., 0., 0.]).unwrap();
    assert!(wall.constrained);
    assert!((wall.point[0] - 6.85).abs() < 1e-12);
    let zero = room_geometry::position(&room.profile, [0.; 3], 1., [0., 1., 0.]).unwrap();
    assert_eq!(zero.direction, [0., 1., 0.]);
    assert_eq!(zero.distance, 0.25);
}

#[test]
fn removal_keeps_tail_and_lfe_remains_independent() {
    let prepared = fixture();
    let render = |remove: bool| {
        let mut session = Session::new(prepared.clone(), 1.5, true, false).unwrap();
        session
            .configure(&[RoomDescription {
                id: 9,
                position: [0., 1., 0.],
                ..RoomDescription::default()
            }])
            .unwrap();
        let mut input = [0.; BLOCK];
        input[0] = 1.;
        let mut result = Vec::new();
        let mut output = [0.; 2 * BLOCK];
        for block in 0..32 {
            if block == 1 && remove {
                session.configure(&[]).unwrap();
            }
            let planes = if remove && block > 0 {
                vec![]
            } else {
                vec![Some(input.as_slice())]
            };
            session
                .process(BLOCK, planes.into_iter(), &[], &[], &mut output)
                .unwrap();
            result.extend_from_slice(&output);
            input.fill(0.);
        }
        result
    };
    assert_eq!(render(false), render(true));
    let lfe = |two: bool, pose: bool| {
        let mut session = Session::new(prepared.clone(), 1.5, true, false).unwrap();
        let count: usize = if two { 2 } else { 1 };
        let sources: Vec<_> = (0..count)
            .map(|id| RoomDescription {
                id: id as u64,
                role: 2,
                position: [0., 1., 0.],
                ..RoomDescription::default()
            })
            .collect();
        session.configure(&sources).unwrap();
        let mut input = [0.; BLOCK];
        input[0] = if two { 0.25 } else { 0.5 };
        let mut output = [0.; 2 * BLOCK];
        let mut result = Vec::new();
        for block in 0..32 {
            if pose {
                session.set_pose([block as f32 * 3., 20., 10.]).unwrap();
            }
            session
                .process(
                    BLOCK,
                    (0..count).map(|_| Some(input.as_slice())),
                    &[],
                    &[],
                    &mut output,
                )
                .unwrap();
            result.extend_from_slice(&output);
            input.fill(0.);
        }
        result
    };
    let one = lfe(false, false);
    assert_eq!(one, lfe(true, true));
    assert!(one.chunks_exact(2).all(|v| v[0] == v[1]));
}

#[test]
fn direction_seams_and_poles_are_continuous() {
    let prepared = fixture();
    let mut work = mradm_dsp::room::prepared::ResponseWork::new(&prepared.room).unwrap();
    for (a, b) in [
        ([179.999, 0.], [-179.999, 0.]),
        ([0., 89.999], [180., 89.999]),
        ([0., -89.999], [180., -89.999]),
    ] {
        let mut left = vec![0.; prepared.room.hrir.data.output_taps * 2];
        let mut right = left.clone();
        let mut face = usize::MAX;
        work.hrir(
            &prepared.room,
            geometry::portable_direction(a[0], a[1]),
            &mut face,
            &mut left,
        )
        .unwrap();
        work.hrir(
            &prepared.room,
            geometry::portable_direction(b[0], b[1]),
            &mut face,
            &mut right,
        )
        .unwrap();
        let error = left
            .iter()
            .zip(&right)
            .map(|(x, y)| f64::from(x - y).powi(2))
            .sum::<f64>();
        let energy = left.iter().map(|v| f64::from(*v).powi(2)).sum::<f64>();
        assert!((error / energy).sqrt() < 5e-4);
    }
}

fn render(
    prepared: Arc<PreparedSet>,
    chunks: &[usize],
    head_locked: bool,
    move_pose: bool,
) -> Vec<f32> {
    let mut session = Session::new(prepared, 1.5, true, true).unwrap();
    session
        .configure(&[RoomDescription {
            id: 7,
            position: [0., 1., 0.],
            ..RoomDescription::default()
        }])
        .unwrap();
    let initial = State {
        head_locked: u32::from(head_locked),
        ..State::default()
    };
    let initial = RoomCommand {
        element: 0,
        state: initial,
        ..RoomCommand::default()
    };
    let mut gain = initial.state;
    gain.gain = 0.37;
    let mut moved = gain;
    moved.position = [-0.5, 0.9, 0.3];
    let events = [
        (
            173usize,
            RoomCommand {
                changed: 2,
                duration: 317,
                state: gain,
                ..RoomCommand::default()
            },
        ),
        (
            781,
            RoomCommand {
                changed: 4,
                duration: 0,
                state: moved,
                ..RoomCommand::default()
            },
        ),
    ];
    let mut input = vec![0.; 1711];
    for (i, value) in input.iter_mut().enumerate() {
        *value = (((i * 37) % 193) as f32 - 96.) / 384.;
    }
    let poses = [(401usize, [25., -17., 9.]), (1193, [-31., 6., -13.])];
    let mut output = Vec::new();
    let mut at = 0;
    let mut part = 0;
    let mut pose_index = 0;
    while at < input.len() {
        if move_pose && pose_index < poses.len() && poses[pose_index].0 == at {
            session.set_pose(poses[pose_index].1).unwrap();
            pose_index += 1;
        }
        let mut count = chunks[part % chunks.len()].min(input.len() - at);
        if move_pose && pose_index < poses.len() {
            count = count.min(poses[pose_index].0 - at);
        }
        let commands: Vec<_> = events
            .iter()
            .filter(|(n, _)| *n >= at && *n < at + count)
            .map(|(n, c)| RoomCommand {
                offset: (n - at) as u32,
                ..*c
            })
            .collect();
        let mut block = vec![0.; count * 2];
        session
            .process(
                count,
                [Some(&input[at..at + count])].into_iter(),
                if at == 0 {
                    std::slice::from_ref(&initial)
                } else {
                    &[]
                },
                &commands,
                &mut block,
            )
            .unwrap();
        output.extend(block);
        at += count;
        part += 1;
    }
    session.end_input();
    let tail = session.info().tail_frames as usize;
    let mut remaining = tail;
    while remaining > 0 {
        let count = remaining.min(chunks[part % chunks.len()]);
        let mut block = vec![0.; count * 2];
        session
            .process(count, [None].into_iter(), &[], &[], &mut block)
            .unwrap();
        output.extend(block);
        remaining -= count;
        part += 1;
    }
    assert_eq!(output.len(), (input.len() + tail) * 2);
    output
}

#[test]
fn arbitrary_packets_and_sample_events_are_bit_identical() {
    let prepared = fixture();
    let fixed = render(prepared.clone(), &[1024], false, true);
    let fragmented = render(prepared, &[1, 7, 31, 3, 257, 63], false, true);
    let mismatch = fixed
        .iter()
        .zip(&fragmented)
        .position(|(a, b)| a.to_bits() != b.to_bits());
    assert!(
        mismatch.is_none(),
        "fragmented output first differs at {mismatch:?}"
    );
    assert!(fixed[..BLOCK * 2].iter().all(|v| *v == 0.));
    assert!(
        fixed[1711 * 2..].iter().any(|v| v.abs() > 1e-5),
        "EOS must retain the room tail"
    );
}

#[test]
fn head_locked_content_ignores_head_rotation() {
    let prepared = fixture();
    let stationary = render(prepared.clone(), &[256], true, false);
    let rotating = render(prepared, &[256], true, true);
    let mismatch = stationary
        .iter()
        .zip(&rotating)
        .position(|(a, b)| a.to_bits() != b.to_bits());
    assert!(
        mismatch.is_none(),
        "head-locked output changed at {mismatch:?}"
    );
}

#[test]
fn original_hrir_nodes_and_public_rotation_convention() {
    let prepared = fixture();
    let bank = &prepared.room.hrir;
    let length = bank.fft_length;
    let mut fft = mradm_dsp::fft::RealFft::new(length).unwrap();
    let mut spectrum = vec![mradm_dsp::Complex32::default(); 2 * fft.bins()];
    let mut planar = vec![0.; bank.stride() * 4];
    let mut time = vec![0.; length];
    for direction in &bank.data.directions {
        let mut face = usize::MAX;
        bank.query(*direction, &mut face, &mut planar).unwrap();
        for ear in 0..2 {
            for bin in 0..fft.bins() {
                spectrum[ear * fft.bins() + bin] = mradm_dsp::Complex32::new(
                    planar[ear * bank.stride() * 2 + bin],
                    planar[ear * bank.stride() * 2 + bank.stride() + bin],
                );
            }
            fft.inverse(
                &spectrum[ear * fft.bins()..(ear + 1) * fft.bins()],
                &mut time,
            )
            .unwrap();
            for (i, value) in time.iter().enumerate() {
                let expected = if i == 96 + 2 + ear * 2 {
                    if ear == 0 { 1. } else { 0.75 }
                } else {
                    0.
                };
                assert!((value - expected).abs() < 2e-7);
            }
        }
    }
    let pose = [37., -29., 63.];
    let old = mradm_dsp::scene_math::Rotation::new(pose)
        .unwrap()
        .apply(25., 33., false);
    let expected = geometry::portable_direction(old[0], old[1]);
    let actual = Rotation::new(pose)
        .unwrap()
        .apply(geometry::portable_direction(25., 33.));
    assert!(geometry::norm(geometry::sub(expected, actual)) < 2e-7);
}

fn alternate(base: &Arc<PreparedSet>, name: &str, energy: f64) -> Arc<PreparedSet> {
    let mut profile = (*base.room.profile).clone();
    profile.identity = name.into();
    profile.id = name.into();
    profile.late_energy = energy;
    let room = Arc::new(PreparedRoom::new(Arc::new(profile), base.room.hrir.clone()).unwrap());
    Arc::new(PreparedSet::new(room).unwrap())
}

#[test]
fn shorter_room_keeps_the_head_locked_tail_after_switching() {
    let base = fixture();
    let prepare = |name: &str, side: f64| {
        let mut profile = (*base.room.profile).clone();
        profile.identity = name.into();
        profile.dimensions = [side; 3];
        profile.listener = [side / 2.; 3];
        profile.early_fade = [0., 0.001];
        profile.late_predelay = 0.;
        profile.late_taps = 1;
        profile.late = vec![0.; profile.late_directions.len()];
        profile.eq = vec![1.];
        Arc::new(
            PreparedSet::new(Arc::new(
                PreparedRoom::new(Arc::new(profile), base.room.hrir.clone()).unwrap(),
            ))
            .unwrap(),
        )
    };
    let large = prepare("large", 50.);
    let small = prepare("small", 1.);
    let small_tail = small.tail_frames();
    let mut session = Session::new(large.clone(), 1.5, true, true).unwrap();
    session
        .configure(&[RoomDescription {
            id: 1,
            position: [0., 50., 0.],
            ..RoomDescription::default()
        }])
        .unwrap();
    let initial = RoomCommand {
        state: State {
            position: [0., 50., 0.],
            head_locked: 1,
            ..State::default()
        },
        ..RoomCommand::default()
    };
    let mut block = [0.; 2 * BLOCK];
    session
        .process(BLOCK, [None].into_iter(), &[initial], &[], &mut block)
        .unwrap();
    session
        .queue_room(Box::new(Instance::new(small, 1).unwrap()), 1)
        .unwrap();
    for _ in 0..200 {
        session
            .process(BLOCK, [None].into_iter(), &[], &[], &mut block)
            .unwrap();
        drop(session.take_retired());
        if session.info().applied_request == 1 {
            break;
        }
    }
    assert_eq!(session.info().applied_request, 1);
    let mut input = [0.; BLOCK];
    input[BLOCK - 1] = 1.;
    session
        .process(
            BLOCK,
            [Some(input.as_slice())].into_iter(),
            &[],
            &[],
            &mut block,
        )
        .unwrap();
    let before_end = session.info().tail_frames;
    session.end_input();
    let tail = session.info().tail_frames as usize;
    assert_eq!(tail as u64, before_end);
    let frames = large.tail_frames() + BLOCK;
    let mut output = vec![0.; frames * 2];
    session
        .process(frames, [None].into_iter(), &[], &[], &mut output)
        .unwrap();
    assert!(
        output[small_tail * 2..].iter().any(|v| v.abs() > 0.01),
        "fixture must arrive after the new world's tail quota"
    );
    assert!(
        output[..tail * 2].iter().any(|v| v.abs() > 0.01),
        "EOS must retain the delayed head-locked impulse"
    );
    assert!(
        output[tail * 2..].iter().all(|v| v.abs() < 1e-6),
        "declared tail must cover every audible branch"
    );
}

#[test]
fn a_new_request_invalidates_pending_room_before_preparation_finishes() {
    let a = fixture();
    let b = alternate(&a, "incoming", 0.2);
    let c = alternate(&a, "superseded", 0.1);
    let mut session = Session::new(a, 1.5, true, true).unwrap();
    session
        .queue_room(Box::new(Instance::new(b.clone(), 0).unwrap()), 1)
        .unwrap();
    let mut output = [0.; BLOCK * 2];
    for _ in 0..50 {
        session
            .process(BLOCK, [].into_iter(), &[], &[], &mut output)
            .unwrap();
        if session.info().phase == 2 {
            break;
        }
    }
    assert_eq!(session.info().phase, 2);
    session
        .queue_room(Box::new(Instance::new(c.clone(), 0).unwrap()), 2)
        .unwrap();
    assert_eq!(session.info().pending_request, 2);
    session.note_room_request(3).unwrap(); // preparation may fail without producing an instance
    assert_ne!(session.info().pending_request, 2);
    for _ in 0..100 {
        session
            .process(BLOCK, [].into_iter(), &[], &[], &mut output)
            .unwrap();
    }
    assert_eq!(session.info().applied_request, 1);
    drop(session.take_retired()); // release the outgoing room, but delay obsolete-candidate disposal
    for _ in 0..100 {
        session
            .process(BLOCK, [].into_iter(), &[], &[], &mut output)
            .unwrap();
    }
    assert_eq!(session.info().applied_request, 1);
    assert_eq!(session.info().phase, 0);
    assert_eq!(session.info().pending_request, 0);
    let obsolete = session.take_retired().unwrap();
    assert_eq!(
        obsolete.prepared.room.profile.identity,
        c.room.profile.identity
    );
    assert!(session.validate_room(&obsolete, 2).is_err());
    session
        .queue_room(Box::new(Instance::new(b, 0).unwrap()), 4)
        .unwrap();
    assert_eq!(session.info().applied_request, 4);
}

#[test]
fn identical_acoustics_switch_has_no_gain_step_and_completes() {
    let a = fixture();
    let b = alternate(&a, "same-acoustics", a.room.profile.late_energy);
    let mut reference = Session::new(a.clone(), 1.5, true, true).unwrap();
    let mut switched = Session::new(a, 1.5, true, true).unwrap();
    let descriptions = [RoomDescription {
        id: 1,
        position: [0., 1., 0.],
        ..RoomDescription::default()
    }];
    reference.configure(&descriptions).unwrap();
    switched.configure(&descriptions).unwrap();
    let mut maximum = 0f32;
    let mut phases = [false; 4];
    for block in 0..100 {
        if block == 3 {
            switched
                .queue_room(Box::new(Instance::new(b.clone(), 1).unwrap()), 7)
                .unwrap();
        }
        let input: Vec<_> = (0..BLOCK)
            .map(|i| (((i + block * BLOCK) * 37 % 193) as f32 - 96.) / 384.)
            .collect();
        let mut x = [0.; BLOCK * 2];
        let mut y = [0.; BLOCK * 2];
        reference
            .process(
                BLOCK,
                [Some(input.as_slice())].into_iter(),
                &[],
                &[],
                &mut x,
            )
            .unwrap();
        let info = switched
            .process(
                BLOCK,
                [Some(input.as_slice())].into_iter(),
                &[],
                &[],
                &mut y,
            )
            .unwrap();
        phases[info.phase as usize] = true;
        maximum = maximum.max(
            x.iter()
                .zip(&y)
                .map(|(a, b)| (a - b).abs())
                .fold(0., f32::max),
        );
        drop(switched.take_retired());
    }
    assert!(
        maximum < 2e-6,
        "equivalent room switch changed PCM by {maximum}"
    );
    assert!(phases.into_iter().all(|v| v));
    assert_eq!(switched.info().applied_request, 7);
    assert_eq!(switched.info().phase, 0);
}

#[test]
fn room_switch_keeps_latest_request_and_rejects_underprepared_state() {
    let a = fixture();
    let b = alternate(&a, "B", 0.1);
    let c = alternate(&a, "C", 0.4);
    let mut session = Session::new(a.clone(), 1.5, true, true).unwrap();
    session
        .configure(&[RoomDescription {
            id: 1,
            position: [0., 1., 0.],
            ..RoomDescription::default()
        }])
        .unwrap();
    assert!(
        session
            .queue_room(Box::new(Instance::new(b.clone(), 0).unwrap()), 1)
            .is_err()
    );
    assert_eq!(session.info().phase, 0);
    let mut candidate = Box::new(Instance::new(b.clone(), 0).unwrap());
    candidate.reserve(1).unwrap();
    assert_eq!(candidate.capacity(), 1);
    session.queue_room(candidate, 2).unwrap();
    let input = [0.01; BLOCK];
    let mut output = [0.; BLOCK * 2];
    for _ in 0..50 {
        session
            .process(
                BLOCK,
                [Some(input.as_slice())].into_iter(),
                &[],
                &[],
                &mut output,
            )
            .unwrap();
        if session.info().phase == 2 {
            break;
        }
    }
    assert_eq!(session.info().phase, 2);
    session
        .queue_room(Box::new(Instance::new(a, 1).unwrap()), 3)
        .unwrap();
    let discarded = session
        .queue_room(Box::new(Instance::new(c, 1).unwrap()), 4)
        .unwrap();
    assert!(discarded[0].is_some());
    for _ in 0..200 {
        session
            .process(
                BLOCK,
                [Some(input.as_slice())].into_iter(),
                &[],
                &[],
                &mut output,
            )
            .unwrap();
        drop(session.take_retired());
        if session.info().applied_request == 4 && session.info().phase == 0 {
            break;
        }
    }
    assert_eq!(session.info().applied_request, 4);
    assert_eq!(session.info().phase, 0);
}

#[test]
fn finite_preroll_reproduces_a_continuously_moving_source() {
    let prepared = fixture();
    let mut full = Session::new(prepared.clone(), 1.5, true, true).unwrap();
    let mut seek = Session::new(prepared.clone(), 1.5, true, true).unwrap();
    let descriptions = [RoomDescription {
        id: 1,
        position: [0., 1., 0.],
        ..RoomDescription::default()
    }];
    full.configure(&descriptions).unwrap();
    seek.configure(&descriptions).unwrap();
    let mut target = State::default();
    target.position = [-0.7, 0.8, 0.2];
    let command = RoomCommand {
        changed: 4,
        duration: 16000,
        state: target,
        ..RoomCommand::default()
    };
    let goal = 12000usize;
    let begin = (goal - prepared.preroll_frames()) / 2048 * 2048;
    assert!(begin > 0);
    seek.reset(begin as u64).unwrap();
    seek.prime(&command, 0).unwrap();
    let input: Vec<_> = (0..15000)
        .map(|i| ((i * 31 % 127) as f32 - 63.) / 256.)
        .collect();
    let mut reference = vec![0.; input.len() * 2];
    full.process(
        input.len(),
        [Some(input.as_slice())].into_iter(),
        &[],
        &[command],
        &mut reference,
    )
    .unwrap();
    let mut actual = vec![0.; (input.len() - begin) * 2];
    seek.process(
        input.len() - begin,
        [Some(&input[begin..])].into_iter(),
        &[],
        &[],
        &mut actual,
    )
    .unwrap();
    let mismatch = reference[goal * 2..]
        .iter()
        .zip(&actual[(goal - begin) * 2..])
        .position(|(a, b)| a.to_bits() != b.to_bits());
    assert!(
        mismatch.is_none(),
        "finite preroll first differs at {mismatch:?}"
    );
}
