use super::*;
use crate::hrtf::Grid;

fn bank(scale: f32) -> Arc<Filters> {
    let grid =
        Arc::new(Grid::new(&[0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.]).unwrap());
    let mut ir = [0.; 192];
    for d in 0..12 {
        ir[d * 16 + d] = (d as f32 + 1.) * scale / 16.;
        ir[d * 16 + 15] = -scale / 32.;
    }
    Arc::new(Filters::new(grid, &ir, 16, 64, true).unwrap())
}

fn pair(
    bank: &Arc<Filters>,
    spread: u32,
    contract: bool,
    corners: usize,
    queries: usize,
) -> (Session, Session) {
    let descriptions = [
        Description::default(),
        Description {
            role: 1,
            ..Description::default()
        },
        Description {
            role: 2,
            ..Description::default()
        },
    ];
    let mut cached =
        Session::new(Arc::clone(bank), &descriptions, 48000, spread, contract).unwrap();
    let mut reference =
        Session::new(Arc::clone(bank), &descriptions, 48000, spread, contract).unwrap();
    cached.filters = CachedQueries::with_limits(Arc::clone(bank), corners, queries);
    reference.filters = CachedQueries::with_limits(Arc::clone(bank), 0, 0);
    (cached, reference)
}

fn equal_bits(a: &[f32], b: &[f32]) {
    for (i, (a, b)) in a.iter().zip(b).enumerate() {
        assert_eq!(a.to_bits(), b.to_bits(), "PCM {i}: {a} != {b}");
    }
}

#[test]
fn cached_pcm_controls_and_diagnostics_are_bit_identical() {
    for scale in [1., 0.125] {
        let bank = bank(scale);
        for (spread, contract, corners, queries) in
            [(0, false, 512, 128), (1, true, 3, 2), (2, false, 1, 1)]
        {
            let (mut cached, mut reference) = pair(&bank, spread, contract, corners, queries);
            let input: Vec<_> = (0..1025)
                .map(|i| (i as f32 * 0.0625).sin() * 0.125)
                .collect();
            let mut actual = vec![17.; 2052];
            let mut expected = actual.clone();
            let mut initial = [Command::default(); 3];
            for (i, c) in initial.iter_mut().enumerate() {
                c.element = i as u32;
                c.has_direction = u32::from(i == 1);
                c.direction = [179.75, 89.5];
            }
            for step in 0..35 {
                let frames = [1, 31, 511, 512, 513, 1024, 1025][step % 7];
                let state = State {
                    gain: 0.5,
                    position: [step as f32 * 0.25 - 3., 1., 0.5],
                    extent: [0.3, 0.4, 0.5],
                    divergence: 0.6,
                    diffuse: 0.3,
                    head_locked: (step % 2) as u32,
                    screen_reference: 1,
                    channel_lock: u32::from(step % 3 == 0),
                    ..State::default()
                };
                let events = [
                    Command {
                        element: 0,
                        changed: FIELDS,
                        duration: 37,
                        state,
                        ..Command::default()
                    },
                    Command {
                        element: 1,
                        offset: frames / 2,
                        changed: 4,
                        duration: 19,
                        direction: [-179.75 + step as f32, -89.5],
                        has_direction: 1,
                        ..Command::default()
                    },
                ];
                let planes = if (10..14).contains(&step) {
                    [None; 3]
                } else {
                    [Some(&input[..]); 3]
                };
                let pose = [step as f32 * 11.125 - 180., step as f32 * 3.25, -5.];
                let init = if step == 0 { &initial[..] } else { &[] };
                let a = cached
                    .process(
                        frames,
                        planes.into_iter(),
                        init,
                        &events,
                        pose,
                        0,
                        &mut actual,
                    )
                    .unwrap();
                let b = reference
                    .process(
                        frames,
                        planes.into_iter(),
                        init,
                        &events,
                        pose,
                        0,
                        &mut expected,
                    )
                    .unwrap();
                assert_eq!(a, b);
                equal_bits(&actual, &expected);
                for i in 0..3 {
                    assert_eq!(cached.control(i).unwrap(), reference.control(i).unwrap());
                }
                if step == 7 {
                    // A late numerical failure after earlier queries warmed the
                    // cache must leave PCM, controls and both signal histories intact.
                    let mut hot = [0.125; 512];
                    hot[511] = f32::MAX;
                    let bad = Command {
                        offset: 511,
                        changed: 2,
                        state: State {
                            gain: f32::MAX,
                            ..state
                        },
                        ..Command::default()
                    };
                    let before = cached.control(0).unwrap();
                    actual.fill(17.);
                    expected.fill(17.);
                    assert!(
                        cached
                            .process(
                                512,
                                [Some(&hot[..]); 3].into_iter(),
                                &[],
                                &[bad],
                                pose,
                                0,
                                &mut actual
                            )
                            .is_err()
                    );
                    assert_eq!(cached.control(0).unwrap(), before);
                    equal_bits(&actual, &expected);
                }
            }
            cached.reset();
            reference.reset();
            cached
                .process(512, std::iter::empty(), &[], &[], [0.; 3], 0, &mut actual)
                .unwrap();
            reference
                .process(512, std::iter::empty(), &[], &[], [0.; 3], 0, &mut expected)
                .unwrap();
            equal_bits(&actual, &expected);
        }
    }
}
