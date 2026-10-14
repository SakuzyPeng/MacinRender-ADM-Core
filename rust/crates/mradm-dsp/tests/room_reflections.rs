//! Independent time-domain image-path oracle for distance-aware early sound.
use mradm_dsp::{
    geometry::{dot, norm, scale, sub},
    room::{
        BLOCK, collection,
        geometry::{self, Image, Position, Rotation},
        hrir::HrirBank,
        prepared::{PreparedRoom, ResponseWork},
        quality::Quality,
        resources::HrirData,
        session::PreparedSet,
        signal::WorldTail,
    },
};
use std::{path::Path, sync::Arc};
#[path = "common/room_fixture.rs"]
mod common;

fn data(collection_enabled: bool) -> HrirData {
    if !collection_enabled {
        return mradm_dsp::room::sofa::prepare(include_bytes!(
            "../../../../tests/fixtures/sofa/distance/far.sofa"
        ))
        .unwrap();
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/fixtures/sofa/distance");
    collection::prepare(
        include_bytes!("../../../../tests/fixtures/sofa/distance/distance_index.json"),
        |n, _| Ok(std::fs::read(root.join(n)).unwrap()),
    )
    .unwrap()
}
fn room(quality: Quality, single_wall: bool, listener_x: f64, distance: bool) -> Arc<PreparedRoom> {
    let base = common::fixture();
    let mut profile = (*base.room.profile).clone();
    profile.identity = format!("reflection-oracle-{single_wall}-{listener_x}");
    profile.dimensions = [1., 1., 1.];
    profile.listener = [listener_x, 0.4, 0.5];
    profile.minimum_distance = 0.02;
    profile.clearance = 0.02;
    profile.reflection_order = if single_wall { 1 } else { 4 };
    profile.band_taps = 3;
    profile.bands = (0..8)
        .flat_map(|b| [0.1 + 0.001 * b as f64, 0.02, 0.005])
        .collect();
    profile.absorption = if single_wall {
        [[1.; 8]; 6]
    } else {
        [[0.3; 8]; 6]
    };
    profile.absorption[0] = std::array::from_fn(|i| 0.1 + 0.03 * i as f64);
    let data = Arc::new(data(distance));
    let (length, _, _) = PreparedRoom::required_fft(&profile, &data).unwrap();
    let bank = Arc::new(HrirBank::with_policy(data, length, quality.fft()).unwrap());
    Arc::new(PreparedRoom::with_quality(Arc::new(profile), bank, quality).unwrap())
}
fn convolution(a: &[f64], b: &[f64]) -> Vec<f64> {
    let mut out = vec![0.; a.len() + b.len() - 1];
    for (i, a) in a.iter().enumerate() {
        for (j, b) in b.iter().enumerate() {
            out[i + j] += a * b;
        }
    }
    out
}
fn fractional(room: &PreparedRoom, fraction: f64) -> Vec<f64> {
    let position = fraction * 1024.;
    let lo = (position as usize).min(1023);
    let alpha = (position - lo as f64) as f32;
    (0..81)
        .map(|i| {
            let a = room.profile.propagation_phases[lo * 81 + i];
            let b = room.profile.propagation_phases[(lo + 1) * 81 + i];
            f64::from(a + (b - a) * alpha)
        })
        .collect()
}

// The fixture's octahedral grid has a closed-form angular response. Its near
// impulses are twice the far amplitudes, delayed by four additional samples.
// This oracle uses no HrirBank query or frequency-domain response assembly.
fn reference(
    room: &PreparedRoom,
    source: Position,
    image: Image,
    pose: Rotation,
) -> (Vec<f64>, f64, bool) {
    let profile = &room.profile;
    let virtual_source = std::array::from_fn(|i| {
        f64::from(image.parity[i]) * source.point[i]
            + 2. * f64::from(image.lattice[i]) * profile.dimensions[i]
    });
    let vector = sub(virtual_source, profile.listener);
    let length = norm(vector);
    let direction = scale(vector, 1. / length);
    let departure = std::array::from_fn(|i| -f64::from(image.parity[i]) * direction[i]);
    let beam = dot(scale(source.direction, -1.), departure).max(0.);
    let relative = ((length - source.distance) / profile.speed).max(0.);
    let fade = if relative >= profile.early_fade[1] {
        0.
    } else if relative <= profile.early_fade[0] {
        1.
    } else {
        0.5 * (1.
            + libm::cos(
                std::f64::consts::PI * (relative - profile.early_fade[0])
                    / (profile.early_fade[1] - profile.early_fade[0]),
            ))
    };
    let gain = f64::from((beam * beam * fade * profile.reference_distance / length) as f32);
    let mut bands = [1f32; 8];
    for (wall, count) in image.walls.iter().enumerate() {
        for (b, v) in bands.iter_mut().enumerate() {
            for _ in 0..*count {
                *v *= (1. - profile.absorption[wall][b]).sqrt() as f32;
            }
        }
    }
    let material: Vec<_> = (0..profile.band_taps)
        .map(|i| {
            f64::from(
                (bands
                    .iter()
                    .enumerate()
                    .map(|(b, g)| profile.bands[b * profile.band_taps + i] * f64::from(*g))
                    .sum::<f64>()) as f32,
            )
        })
        .collect();
    let active = gain > 0. && material.iter().any(|v| *v != 0.);
    let mut output = vec![0.; 2 * room.short_taps];
    if !active {
        return (output, length, false);
    }
    let radius = if image.order == 0 {
        source.distance
    } else {
        length
    };
    let near = if room.hrir.data.collection.is_some() {
        (1.5 - radius.clamp(0.2, 1.5)) / 1.3
    } else {
        0.
    };
    let direction = pose.apply(direction);
    let divisor = direction.iter().map(|v| v.abs()).sum::<f64>();
    let nodes = [
        if direction[0] >= 0. { 0 } else { 2 },
        if direction[1] >= 0. { 1 } else { 3 },
        if direction[2] >= 0. { 4 } else { 5 },
    ];
    let arrival = length / profile.speed * 48000.;
    let integer = arrival.floor() as usize;
    let propagation = fractional(room, f64::from((arrival - integer as f64) as f32));
    for ear in 0..2 {
        let angular = nodes
            .iter()
            .enumerate()
            .map(|(axis, node)| {
                let amplitude = if ear == 0 {
                    (*node + 1) as f64 / 8.
                } else {
                    (6 - *node) as f64 / 8.
                };
                amplitude * direction[axis].abs() / divisor
            })
            .sum::<f64>();
        let delay = 2. + ear as f64 * 2. + 4. * near;
        let hrir = fractional(room, delay - delay.floor());
        let combined = convolution(&convolution(&hrir, &propagation), &material);
        let start =
            integer + profile.alignment_guard + room.hrir.data.guard + delay.floor() as usize - 40;
        for (i, value) in combined.iter().enumerate() {
            output[ear * room.short_taps + start + i] += value * angular * (1. + near) * gain;
        }
    }
    (output, length, true)
}
fn relative(actual: &[f32], expected: &[f64]) -> f64 {
    let energy = expected.iter().map(|v| v * v).sum::<f64>();
    let error = actual
        .iter()
        .zip(expected)
        .map(|(a, b)| (f64::from(*a) - b).powi(2))
        .sum::<f64>();
    (error / energy).sqrt()
}

#[test]
fn a_single_wall_uses_the_whole_path_not_direct_distance_or_last_leg() {
    for quality in [Quality::Exact, Quality::Realtime] {
        let room = room(quality, true, 0.3, true);
        let source = geometry::position(&room.profile, [0., 0.25, 0.], 1., [1., 0., 0.]).unwrap();
        let mut work = ResponseWork::new(&room).unwrap();
        for angles in [[0., 0., 0.], [90., 15., -8.]] {
            let rotation = Rotation::new(angles).unwrap();
            work.build(&room, source, &rotation).unwrap();
            let mut active = 0;
            for image in room.images.iter().filter(|i| i.order > 0) {
                let (expected, distance, nonzero) = reference(&room, source, *image, rotation);
                if nonzero {
                    active += 1;
                    assert!((distance - 0.85).abs() < 1e-12);
                    assert!(
                        (geometry::path(&room.profile, source, *image).distance - distance).abs()
                            < 1e-12
                    );
                    assert!(relative(&work.impulse[2 * room.short_taps..], &expected) < 2e-5);
                }
            }
            assert_eq!(active, 1);
        }
    }
}

#[test]
fn higher_orders_boundaries_materials_and_zero_paths_match_time_convolution() {
    for quality in [Quality::Exact, Quality::Realtime] {
        for listener in [0.05, 0.3] {
            let room = room(quality, false, listener, true);
            let mut work = ResponseWork::new(&room).unwrap();
            for radius in [0.05, 0.25, 0.5] {
                let source =
                    geometry::position(&room.profile, [0., radius, 0.], 1., [1., 0., 0.]).unwrap();
                let pose = Rotation::new([31., -9., 7.]).unwrap();
                work.build(&room, source, &pose).unwrap();
                let mut early = vec![0.; 2 * room.short_taps];
                let mut higher = 0;
                let mut zero = 0;
                let mut beyond = 0;
                for image in &room.images {
                    let (expected, length, active) = reference(&room, source, *image, pose);
                    if image.order == 0 {
                        assert!(relative(&work.impulse[..2 * room.short_taps], &expected) < 2e-5);
                        continue;
                    }
                    if !active {
                        zero += 1;
                    } else {
                        higher += usize::from(image.order > 1);
                        beyond += usize::from(length > 1.5);
                    }
                    for (a, b) in early.iter_mut().zip(expected) {
                        *a += b;
                    }
                }
                assert!(higher > 0 && zero > 0 && beyond > 0);
                let error = relative(&work.impulse[2 * room.short_taps..], &early);
                assert!(
                    error < 2e-4,
                    "{quality:?}, listener={listener}, radius={radius}: {error}"
                );
                let before = work.impulse[..2 * room.short_taps].to_vec();
                work.build_branches(
                    &room,
                    &[geometry::WeightedPosition {
                        position: source,
                        weight: 1.,
                    }],
                    &pose,
                    true,
                    false,
                )
                .unwrap();
                assert_eq!(before, &work.impulse[..2 * room.short_taps]);
            }
        }
    }
}

#[test]
fn late_decoder_keeps_reference_layer_and_lfe_retains_the_full_room_response() {
    for quality in [Quality::Exact, Quality::Realtime] {
        let collection = room(quality, false, 0.3, true);
        let mut fixed = room(quality, false, 0.3, false);
        // Compare the same FFT support: single-layer preparation normally
        // creates a shorter decoder, whereas the collection shares one bank.
        let fixed_room = Arc::get_mut(&mut fixed).unwrap();
        fixed_room.decoder_hrir = fixed_room.hrir.clone();
        let mut actual = WorldTail::new(collection.clone()).unwrap();
        let mut expected = WorldTail::new(fixed).unwrap();
        let mut a = [0.; 2 * BLOCK];
        let mut b = a;
        for block in 0..24 {
            let pose = [block as f32 * 2., 0., 0.];
            actual.update(pose).unwrap();
            expected.update(pose).unwrap();
            let mut input = [0.; BLOCK];
            if block == 0 {
                input[0] = 1.;
            }
            actual.process(&input, &mut a).unwrap();
            expected.process(&input, &mut b).unwrap();
            assert_eq!(a, b);
        }
        let prepared = PreparedSet::new(collection).unwrap();
        assert!(prepared.lfe.taps > 1000);
        assert!(prepared.tail_frames() >= prepared.lfe.taps);
    }
}

#[test]
fn near_wall_motion_expansion_pose_switch_and_reset_are_packet_independent() {
    use mradm_dsp::room::{
        control::{RoomCommand, RoomDescription, State},
        session::{Instance, Session},
    };
    for quality in [Quality::Exact, Quality::Realtime] {
        let first = Arc::new(PreparedSet::new(room(quality, false, 0.3, true)).unwrap());
        let next = Arc::new(PreparedSet::new(room(quality, false, 0.4, true)).unwrap());
        for head in [0, 1] {
            let descriptions = [
                RoomDescription {
                    id: 7,
                    position: [0., 0.5, 0.],
                    ..Default::default()
                },
                RoomDescription {
                    id: 8,
                    role: 2,
                    ..Default::default()
                },
            ];
            let initial = RoomCommand {
                state: State {
                    valid: 4 | 8 | 32 | 256,
                    position: [0., 0.5, 0.],
                    head_locked: head,
                    extent: [0.04, 0.03, 0.02],
                    divergence: 0.1,
                    divergence_range: [15., 0.],
                    ..Default::default()
                },
                ..Default::default()
            };
            let events = [
                (
                    17,
                    RoomCommand {
                        duration: 1800,
                        changed: 4,
                        state: State {
                            position: [0.1, 0.05, 0.02],
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                ),
                (
                    2401,
                    RoomCommand {
                        duration: 1500,
                        changed: 4,
                        state: State {
                            position: [-0.1, 0.5, -0.02],
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                ),
            ];
            let pcm: Vec<_> = (0..22000)
                .map(|i| ((i * 31 % 127) as f32 - 63.) / 4096.)
                .collect();
            let render = |chunks: &[usize]| {
                let mut session = Session::new(first.clone(), 1., true, true).unwrap();
                session.configure(&descriptions).unwrap();
                let mut output = Vec::new();
                let mut at = 0;
                let mut sequence = 0;
                while at < pcm.len() {
                    if at == 1200 {
                        session.set_pose([10., 5., 0.]).unwrap();
                    }
                    if at == 2700 {
                        session.set_pose([-20., 3., 1.]).unwrap();
                    }
                    if at == 2048 {
                        let _ = session
                            .queue_room(Box::new(Instance::new(next.clone(), 2).unwrap()), 1)
                            .unwrap();
                    }
                    let boundary = [1200, 2048, 2700, pcm.len()]
                        .into_iter()
                        .find(|x| *x > at)
                        .unwrap();
                    let count = chunks[sequence % chunks.len()].min(boundary - at);
                    let commands: Vec<_> = events
                        .iter()
                        .filter(|(time, _)| *time >= at && *time < at + count)
                        .map(|(time, event)| RoomCommand {
                            offset: (time - at) as u32,
                            ..*event
                        })
                        .collect();
                    let mut out = vec![0.; 2 * count];
                    let plane = Some(&pcm[at..at + count]);
                    session
                        .process(
                            count,
                            [plane, plane].into_iter(),
                            if at == 0 {
                                std::slice::from_ref(&initial)
                            } else {
                                &[]
                            },
                            &commands,
                            &mut out,
                        )
                        .unwrap();
                    output.extend(out);
                    at += count;
                    sequence += 1;
                }
                assert_eq!(session.info().phase, 0);
                assert_eq!(session.info().applied_request, 1);
                // Reset retains both the applied world room and the session's
                // original head-locked frame. Establish that same binding in a
                // clean session using silent input, then reset both histories.
                session.reset(0).unwrap();
                let mut fresh = Session::new(first.clone(), 1., true, true).unwrap();
                fresh.configure(&descriptions).unwrap();
                let mut a = [0.; 2 * BLOCK];
                let mut b = a;
                let _ = fresh
                    .queue_room(Box::new(Instance::new(next.clone(), 2).unwrap()), 1)
                    .unwrap();
                for _ in 0..100 {
                    fresh
                        .process(BLOCK, [None, None].into_iter(), &[], &[], &mut b)
                        .unwrap();
                }
                assert_eq!(fresh.info().phase, 0);
                assert_eq!(fresh.info().applied_request, 1);
                fresh.reset(0).unwrap();
                session.set_pose([0.; 3]).unwrap();
                for block in 0..24 {
                    let plane = Some(&pcm[block * BLOCK..(block + 1) * BLOCK]);
                    let init = if block == 0 {
                        std::slice::from_ref(&initial)
                    } else {
                        &[]
                    };
                    session
                        .process(BLOCK, [plane, plane].into_iter(), init, &[], &mut a)
                        .unwrap();
                    fresh
                        .process(BLOCK, [plane, plane].into_iter(), init, &[], &mut b)
                        .unwrap();
                    assert!(
                        a == b,
                        "reset {quality:?} head={head} block={block} max={}",
                        a.iter()
                            .zip(b)
                            .map(|(a, b)| (a - b).abs())
                            .fold(0f32, f32::max)
                    );
                }
                output
            };
            assert_eq!(render(&[512]), render(&[1, 173, 777, 19, 256]));
        }
    }
}
