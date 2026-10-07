use mradm_dsp::triple_balance::{
    Event, Layout, Position,
    live::{LEVEL, Mixer, POSITION, SIZE, TbLiveCommand, TbLiveElement},
    processor::{Processor, Track},
};
use std::sync::Arc;

fn initial(size: f32) -> TbLiveCommand {
    TbLiveCommand {
        fields: POSITION | SIZE | LEVEL,
        position: Position {
            x: -0.375,
            y: 0.25,
            z: 0.5,
        },
        size,
        level: 1.,
        ..Default::default()
    }
}
fn replay(layout: Layout, chunks: &[usize]) -> Vec<f32> {
    let input: Vec<f32> = (0..2309)
        .map(|i| {
            if i < 1501 {
                (i % 71) as f32 * 0.001 + 0.01
            } else {
                0.
            }
        })
        .collect();
    let events = [
        TbLiveCommand {
            offset: 17,
            duration: 197,
            fields: SIZE,
            size: 0.7,
            ..initial(0.)
        },
        TbLiveCommand {
            offset: 37,
            duration: 53,
            fields: POSITION,
            position: Position {
                x: 0.8,
                y: -0.4,
                z: 0.25,
            },
            ..initial(0.)
        },
        TbLiveCommand {
            offset: 50,
            duration: 79,
            fields: LEVEL,
            level: 0.2,
            ..initial(0.)
        },
        TbLiveCommand {
            offset: 79,
            fields: POSITION,
            position: Position {
                x: 0.1,
                y: 0.4,
                z: 0.8,
            },
            ..initial(0.)
        },
        TbLiveCommand {
            offset: 511,
            duration: 61,
            fields: LEVEL,
            level: 0.,
            ..initial(0.)
        },
        TbLiveCommand {
            offset: 1111,
            duration: 103,
            fields: SIZE,
            ..initial(0.)
        },
        TbLiveCommand {
            offset: 1247,
            fields: SIZE,
            ..initial(0.3)
        },
        TbLiveCommand {
            offset: 1247,
            fields: LEVEL,
            ..initial(0.)
        },
    ];
    let mut mixer = Mixer::new(layout, 48000, &[TbLiveElement::default()]).unwrap();
    let mut result = vec![0.; input.len() * layout.channels()];
    let mut start = 0;
    let mut chunk = 0;
    while start < input.len() {
        let count = chunks[chunk % chunks.len()].min(input.len() - start);
        let local: Vec<_> = events
            .iter()
            .filter(|e| (start..start + count).contains(&(e.offset as usize)))
            .map(|e| TbLiveCommand {
                offset: e.offset - start as u32,
                ..*e
            })
            .collect();
        let init = if start == 0 {
            vec![initial(0.2)]
        } else {
            vec![]
        };
        mixer
            .process(
                count as u32,
                [Some(&input[start..start + count])].into_iter(),
                &init,
                &local,
                &mut result[start * layout.channels()..(start + count) * layout.channels()],
            )
            .unwrap();
        start += count;
        chunk += 1;
    }
    result
}

#[test]
fn arbitrary_partitions_and_instances_are_exact() {
    for layout in [Layout::Seven, Layout::Nine, Layout::Room222] {
        let expected = replay(layout, &[2309]);
        assert!(expected.iter().all(|v| v.is_finite()));
        assert!(expected.iter().any(|v| *v != 0.));
        assert_eq!(
            expected,
            replay(layout, &[1, 31, 32, 257, 511, 512, 513, 1024])
        );
        assert_eq!(expected, replay(layout, &[512]));
    }
}

#[test]
fn static_size_matches_existing_kernel() {
    for layout in [Layout::Seven, Layout::Nine, Layout::Room222] {
        for (position, size) in [
            (
                Position {
                    x: -0.375,
                    y: 0.25,
                    z: 0.5,
                },
                0.3,
            ),
            (
                Position {
                    x: -1.,
                    y: -1.,
                    z: 0.,
                },
                0.001,
            ),
            (
                Position {
                    x: 1.,
                    y: 1.,
                    z: 1.,
                },
                1.,
            ),
            (
                Position {
                    x: 0.,
                    y: -0.75,
                    z: 0.75,
                },
                0.2,
            ),
            (
                Position {
                    x: -0.75,
                    y: 0.5,
                    z: if layout == Layout::Room222 {
                        -0.5
                    } else {
                        0.25
                    },
                },
                0.7,
            ),
        ] {
            let c = TbLiveCommand {
                position,
                ..initial(size)
            };

            let input = vec![0.125; 1037];
            let track = Arc::new(
                Track::new(
                    vec![Event {
                        start: 0,
                        position: c.position.internal(),
                        size: c.size,
                    }],
                    layout,
                    48000,
                )
                .unwrap(),
            );
            let mut old = Processor::new(track);
            let mut expected = vec![0.; input.len() * layout.channels()];
            old.process(&input, &mut expected, true).unwrap();
            let mut live = Mixer::new(layout, 48000, &[TbLiveElement::default()]).unwrap();
            let mut actual = vec![0.; expected.len()];
            live.process(
                input.len() as u32,
                [Some(input.as_slice())].into_iter(),
                &[c],
                &[],
                &mut actual,
            )
            .unwrap();
            for (i, (a, b)) in actual.iter().zip(&expected).enumerate() {
                assert!(
                    (a - b).abs() <= 2e-6 + 2e-6 * b.abs(),
                    "{layout:?} sample {i}: {a} != {b}"
                );
            }
        }
    }
}

#[test]
fn sample_clock_gain_and_failure_atomicity() {
    let c = TbLiveCommand {
        position: Position {
            x: -1.,
            y: 1.,
            z: 0.,
        },
        ..initial(0.)
    };
    let ramp = TbLiveCommand {
        fields: LEVEL,
        offset: 17,
        duration: 8,
        level: 0.,
        ..c
    };
    let mut live = Mixer::new(Layout::Seven, 96000, &[TbLiveElement::default()]).unwrap();
    let input = [1.; 64];
    let mut actual = [0.; 64 * 12];
    live.process(
        64,
        [Some(input.as_slice())].into_iter(),
        &[c],
        &[ramp],
        &mut actual,
    )
    .unwrap();
    for f in 0..64 {
        let expected = if f <= 17 {
            1.
        } else if f < 25 {
            1. - (f - 17) as f32 / 8.
        } else {
            0.
        };
        assert_eq!(actual[f * 12], expected);
        assert!(actual[f * 12 + 1..(f + 1) * 12].iter().all(|v| *v == 0.));
    }
    live.reset();
    let mut sentinel = [7.; 64 * 12];
    let bad = TbLiveCommand {
        fields: SIZE,
        offset: 31,
        size: 0.5,
        ..c
    };
    assert!(
        live.process(
            64,
            [Some(input.as_slice())].into_iter(),
            &[c],
            &[ramp, bad],
            &mut sentinel
        )
        .is_err()
    );
    assert_eq!(sentinel, [7.; 64 * 12]);
    live.process(
        64,
        [Some(input.as_slice())].into_iter(),
        &[c],
        &[ramp],
        &mut sentinel,
    )
    .unwrap();
    assert_eq!(actual, sentinel);
}

#[test]
fn level_changes_do_not_restart_spatial_ramps_or_filter_history() {
    let input = vec![0.125; 900];
    let c = initial(0.3);
    let size = TbLiveCommand {
        fields: SIZE,
        offset: 13,
        duration: 179,
        size: 0.7,
        ..c
    };
    let position = TbLiveCommand {
        fields: POSITION,
        offset: 29,
        duration: 113,
        position: Position {
            x: 0.8,
            y: -0.3,
            z: 0.2,
        },
        ..c
    };
    let render = |mute: bool| {
        let mut live = Mixer::new(Layout::Nine, 48000, &[TbLiveElement::default()]).unwrap();
        let mut events = vec![size, position];
        if mute {
            events.push(TbLiveCommand {
                fields: LEVEL,
                offset: 51,
                level: 0.,
                ..c
            });
            events.push(TbLiveCommand {
                fields: LEVEL,
                offset: 211,
                level: 1.,
                ..c
            });
        }
        let mut out = vec![0.; input.len() * 16];
        live.process(
            input.len() as u32,
            [Some(input.as_slice())].into_iter(),
            &[c],
            &events,
            &mut out,
        )
        .unwrap();
        out
    };
    let normal = render(false);
    let muted = render(true);
    assert_eq!(&normal[211 * 16..], &muted[211 * 16..]);
    assert!(muted[51 * 16..211 * 16].iter().all(|v| *v == 0.));
}
