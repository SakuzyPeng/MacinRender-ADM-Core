//! Physical-coordinate oracles for automatic room mapping. No private assets.
use mradm_dsp::room::{
    BLOCK,
    control::{RoomCommand, RoomDescription, State},
    geometry::position,
    prepared::PreparedRoom,
    quality::Quality,
    screen::{RoomScreenGeometry, RoomScreens},
    session::{Instance, PreparedSet, Session},
};
use std::sync::Arc;
#[path = "common/room_fixture.rs"]
mod common;

fn close(a: f64, b: f64) {
    assert!((a - b).abs() < 2e-6, "{a} != {b}");
}

#[test]
fn signed_axes_land_on_usable_walls_and_diagonals_use_each_axis() {
    let base = common::fixture();
    let room = &base.room.profile;
    let cases = [
        ([0., 1., 0.], [6.85, 3., 1.2]),
        ([0., -1., 0.], [0.15, 3., 1.2]),
        ([1., 0., 0.], [3.4, 0.15, 1.2]),
        ([-1., 0., 0.], [3.4, 5.85, 1.2]),
        ([0., 0., 1.], [3.4, 3., 3.05]),
        ([0., 0., -1.], [3.4, 3., 0.15]),
        ([0.5, 0.5, 0.5], [5.125, 1.575, 2.125]),
    ];
    for (input, expected) in cases {
        let actual = position(room, input, 0., [1., 0., 0.]).unwrap();
        for (a, b) in actual.point.into_iter().zip(expected) {
            close(a, b);
        }
    }
    let mut larger = (**room).clone();
    larger.dimensions[0] = 11.;
    larger.listener[0] = 1.;
    close(
        position(&larger, [0., 0.5, 0.], 0., [1., 0., 0.])
            .unwrap()
            .distance,
        4.925,
    );
    for room in [room.as_ref(), &larger] {
        close(
            position(room, [0., 1., 0.], 1.5, [1., 0., 0.])
                .unwrap()
                .distance,
            1.5,
        );
    }
}

#[test]
fn boundaries_origin_and_invalid_scales_are_explicit() {
    let base = common::fixture();
    let room = &base.room.profile;
    for scale in [-1., 0.001, 100.1, f64::NAN, f64::INFINITY] {
        assert!(position(room, [0., 1., 0.], scale, [1., 0., 0.]).is_err());
        assert!(Session::new(base.clone(), scale, true, true).is_err());
    }
    assert!(position(room, [f32::NAN, 0., 0.], 0., [1., 0., 0.]).is_err());
    let near = position(room, [0., 0.01, 0.], 0., [1., 0., 0.]).unwrap();
    close(near.distance, 0.25);
    assert!(near.constrained);
    let zero = position(room, [0.; 3], 0., [0., -1., 0.]).unwrap();
    assert_eq!(zero.direction, [0., -1., 0.]);
    close(zero.distance, 0.25);
    let far = position(room, [f32::MAX, f32::MAX, f32::MAX], 0., [1., 0., 0.]).unwrap();
    assert!(far.constrained);
    assert!(far.point.iter().all(|x| x.is_finite()));
    close(far.point[0], 6.85);
    close(far.point[1], 0.15);
    close(far.point[2], 3.05);
}

fn session(
    base: Arc<PreparedSet>,
    scale: f64,
    point: [f32; 3],
    scene: bool,
    screens: RoomScreens,
) -> Session {
    let mut session = Session::new(base, scale, scene, true).unwrap();
    session
        .configure(&[RoomDescription {
            id: 1,
            position: point,
            screens,
            ..RoomDescription::default()
        }])
        .unwrap();
    session
}

#[test]
fn automatic_pcm_matches_physical_sources_in_both_spatial_flavors_and_rates() {
    for quality in [Quality::Exact, Quality::Realtime] {
        for rate in [48000, 96000] {
            for scene in [false, true] {
                let base = common::fixture_at_rate(quality, rate);
                // +Y = half the 3.45 metre front span: a 1.725 metre source.
                let mut automatic = session(
                    base.clone(),
                    0.,
                    [0., 0.5, 0.],
                    scene,
                    RoomScreens::default(),
                );
                let mut physical =
                    session(base, 1., [0., 1.725, 0.], scene, RoomScreens::default());
                let mut a = [0.; BLOCK * 2];
                let mut b = a;
                let mut input = [0.; BLOCK];
                input[0] = 0.1;
                let mut peak = 0f32;
                for block in 0..32 {
                    automatic
                        .process(
                            BLOCK,
                            [Some(input.as_slice())].into_iter(),
                            &[],
                            &[],
                            &mut a,
                        )
                        .unwrap();
                    physical
                        .process(
                            BLOCK,
                            [Some(input.as_slice())].into_iter(),
                            &[],
                            &[],
                            &mut b,
                        )
                        .unwrap();
                    for (a, b) in a.iter().zip(b) {
                        peak = peak.max(a.abs());
                        assert!(
                            (a - b).abs() < 2e-5,
                            "{quality:?}/{rate}/{scene}/{block}: {a} != {b}"
                        );
                    }
                    input.fill(0.);
                }
                assert!(peak > 0.001);
                close(automatic.source_position(1).unwrap().distance, 1.725);
            }
        }
    }
}

#[test]
fn screen_warp_preserves_physical_radius_and_origin_keeps_previous_screen_direction() {
    let base = common::fixture();
    let y = 0.3f32;
    let x = (-libm::tan(29f64.to_radians()) * f64::from(y) * 3.45 / 2.85) as f32;
    let point = [x, y, 0.];
    let distance = position(&base.room.profile, point, 0., [1., 0., 0.])
        .unwrap()
        .distance;
    let screens = RoomScreens {
        enabled: 1,
        reproduction: RoomScreenGeometry {
            width: 90.,
            ..RoomScreenGeometry::default()
        },
        ..RoomScreens::default()
    };
    let mut session = session(base, 0., point, true, screens);
    let mut output = [0.; 2];
    let first = RoomCommand {
        state: State {
            position: point,
            screen_reference: 1,
            ..State::default()
        },
        ..RoomCommand::default()
    };
    session
        .process(1, [None].into_iter(), &[first], &[], &mut output)
        .unwrap();
    let mapped = session.source_position(1).unwrap();
    close(mapped.distance, distance);
    close(mapped.direction[0], mapped.direction[1]);
    let zero = RoomCommand {
        changed: 4 | 128,
        state: State {
            position: [0.; 3],
            screen_reference: 0,
            ..State::default()
        },
        ..RoomCommand::default()
    };
    session
        .process(1, [None].into_iter(), &[], &[zero], &mut output)
        .unwrap();
    let mapped = session.source_position(1).unwrap();
    close(mapped.distance, 0.25);
    close(mapped.direction[0], mapped.direction[1]);
}

#[test]
fn ramps_use_the_interpolated_coordinate_and_switches_remap_only_world_sources() {
    for quality in [Quality::Exact, Quality::Realtime] {
        let base = common::fixture_at_rate(quality, 96000);
        let mut profile = (*base.room.profile).clone();
        profile.dimensions[0] = 5.;
        profile.identity.push_str("-smaller");
        let other = Arc::new(
            PreparedSet::new(Arc::new(
                PreparedRoom::with_quality(Arc::new(profile), base.room.hrir.clone(), quality)
                    .unwrap(),
            ))
            .unwrap(),
        );
        let mut session = Session::new(base, 0., true, true).unwrap();
        session
            .configure(&[
                RoomDescription {
                    id: 1,
                    position: [0., 0.2, 0.],
                    ..RoomDescription::default()
                },
                RoomDescription {
                    id: 2,
                    position: [0., 0.2, 0.],
                    ..RoomDescription::default()
                },
            ])
            .unwrap();
        let initial = [
            RoomCommand {
                state: State {
                    position: [0., 0.2, 0.],
                    ..State::default()
                },
                ..RoomCommand::default()
            },
            RoomCommand {
                element: 1,
                state: State {
                    position: [0., 0.2, 0.],
                    head_locked: 1,
                    ..State::default()
                },
                ..RoomCommand::default()
            },
        ];
        let events = [RoomCommand {
            changed: 4,
            duration: 1000,
            state: State {
                position: [0., 0.8, 0.],
                ..State::default()
            },
            ..RoomCommand::default()
        }];
        let mut out = vec![0.; 1000];
        session
            .process(500, [None, None].into_iter(), &initial, &events, &mut out)
            .unwrap();
        close(session.source_position(1).unwrap().distance, 1.725);
        session
            .process(500, [None, None].into_iter(), &[], &[], &mut out)
            .unwrap();
        close(session.source_position(1).unwrap().distance, 2.76);
        session
            .queue_room(Box::new(Instance::new(other, 2).unwrap()), 1)
            .unwrap();
        for _ in 0..600 {
            session
                .process(500, [None, None].into_iter(), &[], &[], &mut out)
                .unwrap();
            assert!(out.iter().all(|v| v.is_finite()));
            drop(session.take_retired());
            if session.info().applied_request == 1 && session.info().phase == 0 {
                break;
            }
        }
        assert_eq!(session.info().applied_request, 1);
        close(session.source_position(1).unwrap().distance, 1.16);
        close(session.source_position(2).unwrap().distance, 0.69);
    }
}
