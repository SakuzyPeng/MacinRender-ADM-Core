use mradm_dsp::triple_balance::{
    Event, Layout, Position,
    d_mode::{self, Plan, Session},
    panner,
    session::TbRowInput,
};
use std::sync::Arc;
#[path = "fixtures/triple_balance_d_reference.rs"]
mod reference;

fn samples(bytes: &[u8]) -> Vec<f32> {
    bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|b| f32::from_le_bytes(*b))
        .collect()
}

fn plan() -> Arc<Plan> {
    let mut rows = Vec::new();
    let mut bed = vec![0.; 10 * 16];
    for (i, channel) in [0, 1, 2, 3, 4, 5, 6, 7, 12, 13].into_iter().enumerate() {
        bed[i * 16 + channel] = 1.;
        rows.push(TbRowInput {
            input: i,
            event_offset: 0,
            event_count: 0,
            bed_offset: i * 16,
            size_index: 0,
            kind: 0,
            gain: 1.,
        });
    }
    let mut events = reference::EVENTS_0.to_vec();
    events.extend_from_slice(&reference::EVENTS_1);
    for (input, offset, count) in [
        (10, 0, reference::EVENTS_0.len()),
        (11, reference::EVENTS_0.len(), reference::EVENTS_1.len()),
    ] {
        rows.push(TbRowInput {
            input,
            event_offset: offset,
            event_count: count,
            bed_offset: 0,
            size_index: 0,
            kind: 2,
            gain: 1.,
        });
    }
    Arc::new(Plan::new(12, Layout::Nine, 216000, &rows, &events, &bed).unwrap())
}

#[test]
fn independent_static_reference() {
    for (xyz, size, expected) in reference::SPATIAL {
        let actual = d_mode::gains(
            Position {
                x: xyz[0],
                y: xyz[1],
                z: xyz[2],
            },
            size,
        )
        .unwrap();
        let error: f64 = actual
            .iter()
            .zip(expected)
            .map(|(a, b)| f64::from(*a - b).powi(2))
            .sum();
        let energy: f64 = expected.iter().map(|v| f64::from(*v).powi(2)).sum();
        assert!(
            (error / energy).sqrt() < 0.001,
            "position {xyz:?}, size {size}"
        );
        assert_eq!(actual[3], 0.);
    }
}

#[test]
fn independent_dense_two_object_pcm_and_partition_invariance() {
    let input = samples(include_bytes!("fixtures/triple_balance_d_input.f32"));
    let expected = samples(include_bytes!("fixtures/triple_balance_d_output.f32"));
    let plan = plan();
    let mut full = vec![0.; expected.len()];
    Session::new(Arc::clone(&plan))
        .process(&input, &mut full, 0, 2048)
        .unwrap();
    let error: f64 = full
        .iter()
        .zip(&expected)
        .map(|(a, b)| f64::from(*a - *b).powi(2))
        .sum();
    let energy: f64 = expected.iter().map(|v| f64::from(*v).powi(2)).sum();
    assert!(
        (error / energy).sqrt() < 0.001,
        "PCM relative error {}",
        (error / energy).sqrt()
    );
    let mut split = vec![0.; expected.len()];
    let mut state = Session::new(plan);
    let mut start = 0;
    let blocks = [1, 17, 257, 1024, 31];
    let mut index = 0;
    while start < 2048 {
        let n = blocks[index % blocks.len()].min(2048 - start);
        state
            .process(
                &input[start * 12..(start + n) * 12],
                &mut split[start * 16..(start + n) * 16],
                start as u64,
                n,
            )
            .unwrap();
        start += n;
        index += 1;
    }
    assert_eq!(full, split);
}

#[test]
fn rejected_process_preserves_state_and_output() {
    let input = samples(include_bytes!("fixtures/triple_balance_d_input.f32"));
    let plan = plan();
    let mut state = Session::new(Arc::clone(&plan));
    let mut output = vec![0.; 2048 * 16];
    let mut bad = input.clone();
    *bad.last_mut().unwrap() = f32::NAN;
    assert!(state.process(&bad, &mut output, 0, 2048).is_err());
    assert!(output.iter().all(|v| *v == 0.));
    assert!(state.process(&input, &mut output, 1, 2048).is_err());
    state.process(&input, &mut output, 0, 2048).unwrap();
    let mut control = vec![0.; output.len()];
    Session::new(plan)
        .process(&input, &mut control, 0, 2048)
        .unwrap();
    assert_eq!(output, control);
}

#[test]
fn invalid_geometry_and_topology_are_rejected() {
    assert!(
        d_mode::gains(
            Position {
                x: f32::NAN,
                y: 0.,
                z: 0.
            },
            0.
        )
        .is_err()
    );
    assert!(d_mode::gains(Position::default(), 1.1).is_err());
    let row = TbRowInput {
        input: 1,
        event_offset: 0,
        event_count: 0,
        bed_offset: 0,
        size_index: 0,
        kind: 0,
        gain: 1.,
    };
    assert!(Plan::new(1, Layout::Nine, 100, &[row], &[], &[0.; 16]).is_err());
}

#[test]
fn room_222_nodes_power_symmetry_and_floor() {
    let evaluate = |p, s| d_mode::gains_for_layout(p, s, Layout::Room222).unwrap();
    for node in panner::NODES {
        let gains = evaluate(node.position, 0.);
        for (i, actual) in gains.iter().enumerate() {
            let expected = if i == node.channel { 1. } else { 0. };
            assert!(
                (actual - expected).abs() < 1e-6,
                "node {} to {i}: {actual}",
                node.channel
            );
        }
    }
    let reflection = [
        1, 0, 2, 3, 5, 4, 7, 6, 8, 9, 11, 10, 13, 12, 14, 15, 17, 16, 19, 18, 20, 21, 23, 22,
    ];
    for x in [-1., -0.6, 0., 0.6, 1.] {
        for y in [-1., 0., 0.6, 1.] {
            for z in [-1., -0.5, 0., 0.5, 1.] {
                for size in [0., 0.001, 0.05, 0.2, 0.5, 1.] {
                    let gains = evaluate(Position { x, y, z }, size);
                    let mirror = evaluate(Position { x: -x, y, z }, size);
                    assert!(
                        gains.iter().all(|v| v.is_finite() && *v >= 0.),
                        "invalid gain at {x}/{y}/{z}/{size}: {gains:?}"
                    );
                    let energy: f32 = gains.iter().map(|g| g * g).sum();
                    assert!((energy - 1.).abs() < 1e-5, "{x}/{y}/{z}/{size}: {energy}");
                    assert_eq!((gains[3], gains[9]), (0., 0.));
                    for (c, other) in reflection.iter().enumerate() {
                        assert!(
                            (gains[c] - mirror[*other]).abs() < 0.003,
                            "reflection {x}/{y}/{z}/{size} channel {c}: {} vs {}",
                            gains[c],
                            mirror[*other]
                        );
                    }
                }
            }
        }
    }
    // The lower layer projects Y onto its front row; size reaches it from the middle layer.
    assert_eq!(
        evaluate(
            Position {
                x: 0.,
                y: -1.,
                z: -1.
            },
            0.
        ),
        evaluate(
            Position {
                x: 0.,
                y: 1.,
                z: -1.
            },
            0.
        )
    );
    let middle = evaluate(Position::default(), 0.);
    assert_eq!((middle[21], middle[22], middle[23]), (0., 0., 0.));
    let sized = evaluate(Position::default(), 0.5);
    assert!(sized[21] > 0. && sized[22] > 0. && sized[23] > 0.);
    assert!(
        d_mode::gains_for_layout(
            Position {
                z: -1.01,
                ..Position::default()
            },
            0.,
            Layout::Room222
        )
        .is_err()
    );
    assert!(d_mode::gains_for_layout(Position::default(), 0., Layout::Seven).is_err());
}

#[test]
fn room_222_partition_coherence_and_rejection_atomicity() {
    const FRAMES: usize = 4609;
    let events = [
        Event {
            start: 0,
            position: Position {
                x: -0.6,
                y: 0.2,
                z: 0.5,
            },
            size: 0.3,
        },
        Event {
            start: 511,
            position: Position {
                x: 0.4,
                y: -0.3,
                z: -0.8,
            },
            size: 0.8,
        },
        Event {
            start: 512,
            position: Position {
                x: 0.2,
                y: 0.1,
                z: -1.,
            },
            size: 0.1,
        },
        Event {
            start: 1537,
            position: Position {
                x: 0.,
                y: 0.,
                z: 1.,
            },
            size: 0.,
        },
        Event {
            start: 3073,
            position: Position::default(),
            size: 1.,
        },
    ];
    let rows = [TbRowInput {
        input: 0,
        event_offset: 0,
        event_count: events.len(),
        bed_offset: 0,
        size_index: 0,
        kind: 2,
        gain: 1.,
    }];
    let plan = Arc::new(Plan::new(1, Layout::Room222, FRAMES as u64, &rows, &events, &[]).unwrap());
    // Isolated impulses must remain time-aligned and coherent in all output channels.
    let input: Vec<f32> = (0..FRAMES)
        .map(|f| if f % 71 == 0 { 0.25 } else { 0. })
        .collect();
    let mut full = vec![0.; FRAMES * 24];
    Session::new(Arc::clone(&plan))
        .process(&input, &mut full, 0, FRAMES)
        .unwrap();
    for (sample, frame) in input.iter().zip(full.as_chunks::<24>().0) {
        if *sample == 0. {
            assert!(
                frame.iter().all(|v| *v == 0.),
                "coherent mode must not create a filter tail"
            );
        }
    }
    let mut session = Session::new(plan);
    let mut split = vec![0.; full.len()];
    let mut bad = input.clone();
    bad[FRAMES - 1] = f32::NAN;
    assert!(session.process(&bad, &mut split, 0, FRAMES).is_err());
    assert!(split.iter().all(|v| *v == 0.));
    let mut at = 0;
    for n in [1, 31, 511, 513, 1024, 1, 1023, 1505] {
        assert!(
            session
                .process(
                    &input[at..at + n],
                    &mut split[at * 24..(at + n) * 24 - 1],
                    at as u64,
                    n
                )
                .is_err()
        );
        session
            .process(
                &input[at..at + n],
                &mut split[at * 24..(at + n) * 24],
                at as u64,
                n,
            )
            .unwrap();
        at += n;
    }
    assert_eq!(at, FRAMES);
    assert_eq!(full, split);
}
