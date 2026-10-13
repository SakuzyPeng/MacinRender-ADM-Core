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
fn screen_warp_follows_authored_ramps_and_switches_at_the_event_sample() {
    use mradm_dsp::room::screen::{RoomScreenGeometry, RoomScreens};
    let mut session = Session::new(fixture(), 1.5, true, true).unwrap();
    let x = (libm::sin(29f64.to_radians()) * 2.) as f32;
    let y = (libm::cos(29f64.to_radians()) * 2.) as f32;
    session
        .configure(&[RoomDescription {
            id: 1,
            position: [-x, y, 0.],
            screens: RoomScreens {
                enabled: 1,
                reproduction: RoomScreenGeometry {
                    width: 90.,
                    ..RoomScreenGeometry::default()
                },
                ..RoomScreens::default()
            },
            ..RoomDescription::default()
        }])
        .unwrap();
    let first = RoomCommand {
        state: State {
            position: [-x, y, 0.],
            screen_reference: 1,
            ..State::default()
        },
        ..RoomCommand::default()
    };
    let target = RoomCommand {
        changed: 4,
        duration: 1000,
        state: State {
            position: [x, y, 0.],
            screen_reference: 1,
            ..State::default()
        },
        ..RoomCommand::default()
    };
    let mut output = vec![0.; 1000];
    session
        .process(500, [None].into_iter(), &[first], &[target], &mut output)
        .unwrap();
    let mid = session.source_position(1).unwrap();
    assert!(mid.direction[1].abs() < 1e-6);
    assert!(
        (mid.distance - f64::from(y) * 1.5).abs() < 1e-6,
        "warp after interpolation, not interpolation between warped endpoints"
    );
    session
        .process(500, [None].into_iter(), &[], &[], &mut output)
        .unwrap();
    let end = session.source_position(1).unwrap();
    assert!((end.direction[0] + end.direction[1]).abs() < 1e-6);
    let off = RoomCommand {
        changed: 128,
        duration: 1000,
        state: State {
            screen_reference: 0,
            ..State::default()
        },
        ..RoomCommand::default()
    };
    session
        .process(1, [None].into_iter(), &[], &[off], &mut output)
        .unwrap();
    let end = session.source_position(1).unwrap();
    assert!((end.direction[1] + f64::from(x) / 2.).abs() < 1e-6);
}

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
        preprocessing_guard: data.preprocessing_guard,
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
        .as_chunks::<2>()
        .0
        .iter()
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
        .as_chunks::<2>()
        .0
        .iter()
        .enumerate()
        .max_by(|(_, a), (_, b)| a[0].abs().total_cmp(&b[0].abs()))
        .unwrap()
        .0;
    assert!((peak as f64 - expected).abs() < 1.);
    let gain: f64 = impulse
        .as_chunks::<2>()
        .0
        .iter()
        .map(|v| f64::from(v[0]))
        .sum();
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
        let left: Vec<_> = impulse.as_chunks::<2>().0.iter().map(|v| v[0]).collect();
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
    assert!(one.as_chunks::<2>().0.iter().all(|v| v[0] == v[1]));
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
fn source_spans_preserve_coincident_events_ramps_and_block_boundaries() {
    let prepared = fixture();
    let descriptions: Vec<_> = (0..4)
        .map(|i| RoomDescription {
            id: i + 1,
            role: if i == 3 { 2 } else { 0 },
            position: [i as f32 * 0.2 - 0.3, 1., 0.1],
            ..RoomDescription::default()
        })
        .collect();
    let initial: Vec<_> = descriptions
        .iter()
        .enumerate()
        .map(|(i, d)| RoomCommand {
            element: i as u32,
            state: State {
                position: d.position,
                diffuse: 0.2 * i as f32,
                head_locked: u32::from(i == 0),
                ..State::default()
            },
            ..RoomCommand::default()
        })
        .collect();
    let mut gain = initial[0].state;
    gain.gain = 0.17;
    let mut moved = initial[1].state;
    moved.position = [-0.8, 0.6, 0.2];
    let mut locked = initial[1].state;
    locked.head_locked = 1;
    let events = [
        (
            0,
            RoomCommand {
                element: 0,
                changed: 2,
                duration: 333,
                state: gain,
                ..RoomCommand::default()
            },
        ),
        (
            255,
            RoomCommand {
                element: 1,
                changed: 4,
                duration: 411,
                state: moved,
                ..RoomCommand::default()
            },
        ),
        (
            256,
            RoomCommand {
                element: 3,
                changed: 2,
                duration: 211,
                state: gain,
                ..RoomCommand::default()
            },
        ),
        (
            257,
            RoomCommand {
                element: 0,
                cleared: 2,
                duration: 171,
                ..RoomCommand::default()
            },
        ),
        (
            257,
            RoomCommand {
                element: 1,
                changed: 256,
                state: locked,
                ..RoomCommand::default()
            },
        ),
        (
            1023,
            RoomCommand {
                element: 1,
                cleared: 4,
                duration: 137,
                ..RoomCommand::default()
            },
        ),
    ];
    let input: Vec<Vec<_>> = (0..4)
        .map(|source| {
            (0..1539)
                .map(|i| ((i * 37 + source * 19) % 113) as f32 / 512. - 0.1)
                .collect()
        })
        .collect();
    for scene in [false, true] {
        let render = |chunks: &[usize]| {
            let mut session = Session::new(prepared.clone(), 1.5, scene, true).unwrap();
            session.configure(&descriptions).unwrap();
            let mut result = Vec::new();
            let mut at = 0;
            let mut packet = 0;
            while at < input[0].len() {
                if at == 537 {
                    session.set_pose([23., -7., 11.]).unwrap();
                }
                let mut count = chunks[packet % chunks.len()].min(input[0].len() - at);
                if at < 537 {
                    count = count.min(537 - at);
                }
                let commands: Vec<_> = events
                    .iter()
                    .filter(|(n, _)| *n >= at && *n < at + count)
                    .map(|(n, c)| RoomCommand {
                        offset: (n - at) as u32,
                        ..*c
                    })
                    .collect();
                let mut output = vec![0.; count * 2];
                session
                    .process(
                        count,
                        input.iter().enumerate().map(|(i, plane)| {
                            if i == 2 {
                                None
                            } else {
                                Some(&plane[at..at + count])
                            }
                        }),
                        if at == 0 { &initial } else { &[] },
                        &commands,
                        &mut output,
                    )
                    .unwrap();
                result.extend(output);
                at += count;
                packet += 1;
            }
            session.end_input();
            let mut remaining = session.info().tail_frames as usize;
            while remaining != 0 {
                let count = remaining.min(511);
                let mut output = vec![0.; count * 2];
                session
                    .process(count, [None; 4].into_iter(), &[], &[], &mut output)
                    .unwrap();
                result.extend(output);
                remaining -= count;
            }
            result
        };
        let reference = render(&[1]);
        for chunks in [&[4096][..], &[3, 255, 1, 257, 33][..]] {
            let actual = render(chunks);
            assert_eq!(actual.len(), reference.len());
            let first = actual
                .iter()
                .zip(&reference)
                .position(|(a, b)| a.to_bits() != b.to_bits());
            assert_eq!(
                first, None,
                "input span changed sample {first:?}, scene={scene}"
            );
        }
    }
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

/// Twelve world directions so the late pass takes its parallel branch.
fn many_directions(base: &Arc<PreparedSet>, name: &str, absorption: f64) -> Arc<PreparedSet> {
    let mut profile = (*base.room.profile).clone();
    profile.identity = name.into();
    profile.id = name.into();
    profile.absorption = [[absorption; 8]; 6];
    let t = (1. + 5f64.sqrt()) / 2.;
    let mut directions = Vec::new();
    for (a, b) in [(1., t), (-1., t), (1., -t), (-1., -t)] {
        directions.extend([[0., a, b], [a, b, 0.], [b, 0., a]]);
    }
    for direction in &mut directions {
        let length = geometry::norm(*direction);
        *direction = direction.map(|v| v / length);
    }
    let taps = profile.late_taps;
    profile.late = (0..directions.len())
        .flat_map(|d| {
            (0..taps).map(move |i| {
                if i == 17 + d * 7 || i == 2200 + d * 5 {
                    0.05
                } else {
                    0.
                }
            })
        })
        .collect();
    profile.late_directions = directions;
    let room = Arc::new(PreparedRoom::new(Arc::new(profile), base.room.hrir.clone()).unwrap());
    Arc::new(PreparedSet::new(room).unwrap())
}

#[test]
fn single_late_pass_matches_field_merge_and_mark_segmented_decoding() {
    use mradm_dsp::room::{run_on_workers, signal::WorldTail};
    use std::sync::Mutex;
    /// One canonical block of late-pass input.
    #[derive(Clone, Copy)]
    struct LateBlock {
        current: [f32; BLOCK],
        next: [f32; BLOCK],
        incoming: bool,
        frames: usize,
        marks: [bool; BLOCK],
        poses: [[f32; 3]; BLOCK],
    }

    struct LatePass {
        current: WorldTail,
        next: WorldTail,
        decoder: WorldTail,
        output: [f32; 2 * BLOCK],
        on_worker: bool,
    }

    impl LatePass {
        fn new(first: &Arc<PreparedSet>, second: &Arc<PreparedSet>) -> Self {
            Self {
                current: WorldTail::new_field(first.room.clone()).unwrap(),
                next: WorldTail::new_field(second.room.clone()).unwrap(),
                decoder: WorldTail::new_decoder(first.room.clone()).unwrap(),
                output: [0.; 2 * BLOCK],
                on_worker: false,
            }
        }

        fn render(&mut self, block: &LateBlock) -> mradm_dsp::Result<()> {
            self.on_worker = std::thread::current()
                .name()
                .is_some_and(|name| name.starts_with("mradm-room-"));
            self.current.begin_field(&block.current)?;
            if block.incoming {
                self.next.begin_field(&block.next)?;
            }
            self.decoder.render_world(
                &self.current,
                block.frames,
                block.incoming.then_some(&self.next),
                &block.marks,
                &block.poses,
                &mut self.output,
            )
        }
    }
    let base = fixture();
    let first = many_directions(&base, "late-a", 0.94);
    let second = many_directions(&base, "late-b", 0.9);
    let count = first.room.profile.late_directions.len();
    assert!(count >= 8, "the pooled pass must take its parallel branch");
    // Reference: the former field -> merge -> per-mark decoder chain, with
    // every stage kept serial so it does not depend on worker scheduling.
    let mut legacy_a = WorldTail::new_field(first.room.clone()).unwrap();
    let mut legacy_b = WorldTail::new_field(second.room.clone()).unwrap();
    let mut legacy = WorldTail::new_decoder(first.room.clone()).unwrap();
    for tail in [&mut legacy_a, &mut legacy_b, &mut legacy] {
        tail.set_parallel(false);
    }
    // The single pass on the test thread takes the serial branch; on a room
    // worker it takes the parallel branch, as in session dispatch.
    let mut serial = LatePass::new(&first, &second);
    let pooled = Arc::new(Mutex::new(LatePass::new(&first, &second)));
    let mut world_a = vec![0.; count * BLOCK];
    let mut world_b = world_a.clone();
    let mut merged = world_a.clone();
    let mut expected = [0.; 2 * BLOCK];
    let mut pose = [0.; 3];
    for index in 0..40 {
        let mut block = LateBlock {
            current: [0.; BLOCK],
            next: [0.; BLOCK],
            incoming: (10..30).contains(&index),
            frames: match index {
                20 => 0,
                21 => 77,
                _ => BLOCK,
            },
            marks: [false; BLOCK],
            poses: [[0.; 3]; BLOCK],
        };
        for i in 0..BLOCK {
            block.current[i] = ((((index * BLOCK + i) * 37) % 193) as f32 - 96.) / 384.;
            block.next[i] = ((((index * BLOCK + i) * 53) % 211) as f32 - 105.) / 512.;
        }
        // Marks every 64 samples; the pose changes only at some of them.
        for at in (0..BLOCK).step_by(64) {
            block.marks[at] = true;
            if (index + at / 64) % 3 == 0 {
                pose = [index as f32 * 7. + at as f32 * 0.1 - 120., 11., -4.];
            }
            block.poses[at] = pose;
        }
        legacy_a
            .process_field(&block.current, &mut world_a)
            .unwrap();
        if block.incoming {
            legacy_b.process_field(&block.next, &mut world_b).unwrap();
        }
        for (i, value) in merged.iter_mut().enumerate() {
            *value = if i % BLOCK >= block.frames {
                0.
            } else {
                world_a[i]
            };
            if block.incoming {
                *value += world_b[i];
            }
        }
        legacy.begin_world(&merged).unwrap();
        let mut begin = 0;
        while begin < BLOCK {
            let end = (begin + 1..BLOCK)
                .find(|i| block.marks[*i])
                .unwrap_or(BLOCK);
            legacy.update(block.poses[begin]).unwrap();
            legacy
                .render_range(begin, end - begin, &mut expected)
                .unwrap();
            begin = end;
        }
        serial.render(&block).unwrap();
        let job = pooled.clone();
        run_on_workers(move || job.lock().unwrap().render(&block)).unwrap();
        let pooled = pooled.lock().unwrap();
        assert!(!serial.on_worker && pooled.on_worker);
        for (name, actual) in [("serial", &serial.output), ("parallel", &pooled.output)] {
            let mismatch = expected
                .iter()
                .zip(actual)
                .position(|(x, y)| x.to_bits() != y.to_bits());
            assert!(
                mismatch.is_none(),
                "{name} block {index} differs at {mismatch:?}"
            );
        }
    }
    assert!(expected.iter().any(|v| v.abs() > 1e-6));
}

#[test]
fn late_pass_ignores_control_marks_without_a_pose_change() {
    let base = fixture();
    let first = many_directions(&base, "late-a", 0.94);
    let second = many_directions(&base, "late-b", 0.9);
    let descriptions: Vec<_> = (0..5)
        .map(|i| RoomDescription {
            id: i + 1,
            role: if i == 4 { 2 } else { 0 },
            position: [i as f32 * 0.3 - 0.6, 1., 0.1],
            ..RoomDescription::default()
        })
        .collect();
    let initial: Vec<_> = descriptions
        .iter()
        .enumerate()
        .map(|(i, d)| RoomCommand {
            element: i as u32,
            state: State {
                position: d.position,
                ..State::default()
            },
            ..RoomCommand::default()
        })
        .collect();
    // Geometry commands that restate the current position still create
    // session marks. Only pose changes may split or retarget the late pass.
    let render = |restated: bool| {
        let mut session = Session::new(first.clone(), 1.5, true, true).unwrap();
        session.configure(&descriptions).unwrap();
        let frames = 384;
        let mut input = vec![0.; frames];
        let mut block = vec![0.; frames * 2];
        let mut output = Vec::new();
        for call in 0..80 {
            for (i, value) in input.iter_mut().enumerate() {
                *value = ((((call * frames + i) * 37) % 193) as f32 - 96.) / 384.;
            }
            if call % 3 == 1 {
                session
                    .set_pose([call as f32 * 1.7 - 40., 5., -3.])
                    .unwrap();
            }
            if call == 10 {
                session
                    .queue_room(Box::new(Instance::new(second.clone(), 5).unwrap()), 1)
                    .unwrap();
            }
            let commands: Vec<_> = if restated {
                (1..4)
                    .map(|e| RoomCommand {
                        element: e,
                        offset: e * 97,
                        changed: 4,
                        ..initial[e as usize]
                    })
                    .collect()
            } else {
                Vec::new()
            };
            session
                .process(
                    frames,
                    (0..5).map(|_| Some(input.as_slice())),
                    if call == 0 { &initial } else { &[] },
                    &commands,
                    &mut block,
                )
                .unwrap();
            output.extend_from_slice(&block);
            drop(session.take_retired());
        }
        assert_eq!(session.info().applied_request, 1, "switch must complete");
        output
    };
    let plain = render(false);
    let restated = render(true);
    let mismatch = plain
        .iter()
        .zip(&restated)
        .position(|(a, b)| a.to_bits() != b.to_bits());
    assert!(
        mismatch.is_none(),
        "late output first differs at {mismatch:?}"
    );
    assert!(plain.iter().any(|v| v.abs() > 1e-5));
}

/// A position `radius` metres away at azimuth / elevation in degrees.
fn around(azimuth: f32, elevation: f32, radius: f64) -> [f32; 3] {
    let v = geometry::portable_direction(azimuth, elevation);
    [
        (-v[1] * radius) as f32,
        (v[0] * radius) as f32,
        (v[2] * radius) as f32,
    ]
}

#[test]
fn a_source_ramp_ignores_other_sources_update_points() {
    use mradm_dsp::room::control::RoomOverride;
    // ADR 0019: a source is resampled only at block starts, pose changes and
    // its own events. Restating other sources' controls is a no-op for them
    // and must not resample a ramp in the world or head-locked branch.
    let prepared = fixture();
    let next = alternate(&prepared, "per-source-next", 0.7);
    let descriptions: Vec<_> = (0..4)
        .map(|i| RoomDescription {
            id: i + 1,
            role: if i == 3 { 2 } else { 0 },
            position: around(i as f32 * 70. - 100., 10., 1.2),
            ..RoomDescription::default()
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
                ..State::default()
            },
            ..RoomCommand::default()
        })
        .collect();
    let neutral = [RoomOverride {
        gain_db: -1.5,
        mute: 0,
        extent: [1.; 3],
        diffuse: 1.,
        divergence: 1.,
        has_head: 0,
        head: 0,
        reserved: 0,
    }; 4];
    let render = |restated: bool| {
        let mut session = Session::new(prepared.clone(), 1.5, true, true).unwrap();
        session.configure(&descriptions).unwrap();
        session.set_overrides(&neutral).unwrap();
        let frames = 384;
        let mut input = vec![0.; frames];
        let mut block = vec![0.; frames * 2];
        let mut output = Vec::new();
        for call in 0..80 {
            for (i, value) in input.iter_mut().enumerate() {
                *value = ((((call * frames + i) * 37) % 193) as f32 - 96.) / 384.;
            }
            session
                .set_pose([call as f32 * 2.3 - 30., 4., -2.])
                .unwrap();
            if call == 10 {
                session
                    .queue_room(Box::new(Instance::new(next.clone(), 4).unwrap()), 1)
                    .unwrap();
            }
            if restated && call % 5 == 2 {
                // Between calls the FIFO is mid-block (384 is not a block multiple).
                session.set_overrides(&neutral).unwrap();
            }
            // Sources 0 (world) and 1 (head-locked) ramp continuously with
            // their own events at offsets that are not block starts.
            let mut commands: Vec<_> = [(0u32, 0u32), (1, 100)]
                .into_iter()
                .map(|(element, offset)| RoomCommand {
                    element,
                    offset,
                    duration: frames as u32,
                    changed: 4,
                    state: State {
                        position: around(
                            element as f32 * 90. + call as f32 * 9.,
                            5.,
                            1.1 + call as f64 * 0.01,
                        ),
                        head_locked: element,
                        ..State::default()
                    },
                    ..RoomCommand::default()
                })
                .collect();
            if restated {
                for (element, offset) in [(2u32, 37u32), (3, 211), (2, 300)] {
                    commands.push(RoomCommand {
                        element,
                        offset,
                        changed: 4,
                        ..initial[element as usize]
                    });
                }
            }
            commands.sort_by_key(|c| c.offset);
            session
                .process(
                    frames,
                    (0..4).map(|_| Some(input.as_slice())),
                    if call == 0 { &initial } else { &[] },
                    &commands,
                    &mut block,
                )
                .unwrap();
            output.extend_from_slice(&block);
            drop(session.take_retired());
        }
        assert_eq!(session.info().applied_request, 1, "switch must complete");
        output
    };
    let quiet = render(false);
    let restated = render(true);
    let mismatch = quiet
        .iter()
        .zip(&restated)
        .position(|(a, b)| a.to_bits() != b.to_bits());
    assert!(mismatch.is_none(), "output first differs at {mismatch:?}");
    assert!(quiet.iter().any(|v| v.abs() > 1e-5));
}

#[test]
fn staggered_ramps_on_every_source_render_and_fragment_bit_identically() {
    // Every source ramps with its own event offset: 32 distinct offsets per
    // 512 frames. A session-wide update grid exceeded the 16 targets per
    // 10 ms of each dynamic filter here; per-source grids keep it bounded.
    let prepared = fixture();
    let sources = 32;
    let descriptions: Vec<_> = (0..sources)
        .map(|i| RoomDescription {
            id: i as u64 + 1,
            position: around(i as f32 * 11.25, (i % 3) as f32 * 15. - 15., 1.4),
            ..RoomDescription::default()
        })
        .collect();
    let mut events = Vec::new();
    for call in 0..12 {
        for source in 0..sources {
            let sample = call * 512 + (source * 131 + 17) % 512;
            events.push((
                sample,
                RoomCommand {
                    element: source as u32,
                    duration: 512,
                    changed: 4,
                    state: State {
                        position: around(
                            source as f32 * 11.25 + (call + 1) as f32 * 3.,
                            (source % 3) as f32 * 15. - 15.,
                            1.4,
                        ),
                        ..State::default()
                    },
                    ..RoomCommand::default()
                },
            ));
        }
    }
    events.sort_by_key(|e| e.0);
    let total = 12 * 512 + 1024;
    let render = |chunks: &[usize]| {
        let mut session = Session::new(prepared.clone(), 1.5, true, true).unwrap();
        session.configure(&descriptions).unwrap();
        let mut input = vec![0.; 4096];
        let mut block = vec![0.; 8192];
        let mut commands = Vec::new();
        let mut output = Vec::new();
        let (mut position, mut sequence) = (0usize, 0);
        while position < total {
            if position.is_multiple_of(512) {
                session.set_pose([position as f32 * 0.01, 0., 0.]).unwrap();
            }
            let count = chunks[sequence % chunks.len()]
                .min((position / 512 + 1) * 512 - position)
                .min(total - position);
            sequence += 1;
            for (i, value) in input[..count].iter_mut().enumerate() {
                *value = ((((position + i) * 53) % 211) as f32 - 105.) / 512.;
            }
            commands.clear();
            for (sample, command) in &events {
                if (position..position + count).contains(sample) {
                    commands.push(RoomCommand {
                        offset: (sample - position) as u32,
                        ..*command
                    });
                }
            }
            session
                .process(
                    count,
                    (0..sources).map(|_| Some(&input[..count])),
                    &[],
                    &commands,
                    &mut block[..count * 2],
                )
                .unwrap();
            output.extend_from_slice(&block[..count * 2]);
            position += count;
        }
        output
    };
    let canonical = render(&[512]);
    let fragmented = render(&[1, 173, 733, 4096, 19, 257]);
    let mismatch = canonical
        .iter()
        .zip(&fragmented)
        .position(|(a, b)| a.to_bits() != b.to_bits());
    assert!(
        mismatch.is_none(),
        "fragmented output first differs at {mismatch:?}"
    );
    assert!(canonical.iter().any(|v| v.abs() > 1e-5));
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
    let target = State {
        position: [-0.7, 0.8, 0.2],
        ..State::default()
    };
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
