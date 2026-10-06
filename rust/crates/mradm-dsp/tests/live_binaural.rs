use mradm_dsp::{hrtf::Grid, hrtf_filters::Filters, live_binaural::*};
use std::sync::{Arc, OnceLock};
fn bank() -> Arc<Filters> {
    static BANK: OnceLock<Arc<Filters>> = OnceLock::new();
    Arc::clone(BANK.get_or_init(|| {
        let grid =
            Arc::new(Grid::new(&[0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.]).unwrap());
        let mut impulses = [0.; 6 * 2 * 16];
        for i in 0..12 {
            impulses[i * 16 + i % 2] = 0.5;
        }
        Arc::new(Filters::new(grid, &impulses, 16, 64, true).unwrap())
    }))
}
fn session(role: u32) -> Session {
    Session::new(
        bank(),
        &[Description {
            role,
            ..Description::default()
        }],
        48000,
        0,
        false,
    )
    .unwrap()
}
fn process(
    t: &mut Session,
    n: u32,
    input: Option<&[f32]>,
    initial: &[Command],
    events: &[Command],
    out: &mut [f32],
) -> mradm_dsp::Result<Report> {
    t.process(n, [input].into_iter(), initial, events, [0.; 3], 0, out)
}
#[test]
fn exact_lfe_gain_endpoints_and_output_tail() {
    let mut t = session(2);
    let init = Command::default();
    let mut out = [17.; 10];
    let target = State {
        gain: 0.,
        ..State::default()
    };
    let c = Command {
        duration: 4,
        changed: 2,
        state: target,
        ..Command::default()
    };
    process(&mut t, 4, Some(&[1.; 4]), &[init], &[c], &mut out).unwrap();
    assert_eq!(out, [1., 1., 0.75, 0.75, 0.5, 0.5, 0.25, 0.25, 17., 17.]);
    assert_eq!(t.control(0).unwrap().remaining, [0; 11]);
    assert_eq!(t.control(0).unwrap().current.gain, 0.);
    process(&mut t, 1, Some(&[1.]), &[], &[], &mut out).unwrap();
    assert_eq!(&out[..2], &[0., 0.]);
}
#[test]
fn independent_deadlines_head_only_and_optional_clear() {
    let mut t = session(0);
    process(&mut t, 0, None, &[Command::default()], &[], &mut []).unwrap();
    let mut position = State::default();
    position.position[0] = 1.;
    let pan = Command {
        duration: 8,
        changed: 4,
        state: position,
        ..Command::default()
    };
    let mut level = position;
    level.gain = 0.;
    let gain = Command {
        duration: 4,
        changed: 2,
        state: level,
        ..Command::default()
    };
    let mut head = level;
    head.head_locked = 1;
    let head = Command {
        duration: 99,
        changed: 256,
        state: head,
        ..Command::default()
    };
    process(&mut t, 3, None, &[], &[pan, gain, head], &mut [0.; 6]).unwrap();
    let state = t.control(0).unwrap();
    assert_eq!(state.remaining[1], 1);
    assert_eq!(state.remaining[2], 5);
    assert_eq!(state.current.gain, 0.25);
    assert_eq!(state.current.position[0], 0.375);
    assert_eq!(state.current.head_locked, 1);
    assert_eq!(state.remaining[8], 0);
    process(&mut t, 1, None, &[], &[], &mut [0.; 2]).unwrap();
    assert_eq!(t.control(0).unwrap().current.position[0], 0.5);
    let mut cleared = t.control(0).unwrap().target;
    cleared.valid &= !4;
    process(
        &mut t,
        2,
        None,
        &[],
        &[Command {
            duration: 3,
            cleared: 4,
            state: cleared,
            ..Command::default()
        }],
        &mut [0.; 4],
    )
    .unwrap();
    assert_ne!(t.control(0).unwrap().current.valid & 4, 0);
    process(&mut t, 1, None, &[], &[], &mut [0.; 2]).unwrap();
    assert_eq!(t.control(0).unwrap().current.valid & 4, 0);
}
#[test]
fn direct_speaker_direction_seam_and_repeated_targets() {
    let mut t = session(1);
    let init = Command {
        direction: [170., 0.],
        has_direction: 1,
        ..Command::default()
    };
    let c = Command {
        duration: 4,
        changed: 4,
        direction: [-170., 0.],
        has_direction: 1,
        ..Command::default()
    };
    process(&mut t, 2, None, &[init], &[c], &mut [0.; 4]).unwrap();
    assert_eq!(t.control(0).unwrap().direction, [180., 0.]);
    process(&mut t, 1, None, &[], &[c], &mut [0.; 2]).unwrap();
    assert_eq!(t.control(0).unwrap().remaining[2], 3);
    assert_eq!(t.control(0).unwrap().direction, [182.5, 0.]);
    process(&mut t, 3, None, &[], &[], &mut [0.; 6]).unwrap();
    assert_eq!(t.control(0).unwrap().direction, [-170., 0.]);
}
#[test]
fn failed_late_commands_and_preview_overflow_preserve_signal_history() {
    let mut t = session(0);
    let mut clean = session(0);
    let input = [0.125; 1025];
    let mut out = [0.; 2050];
    let mut reference = [0.; 2050];
    let mut state = State {
        diffuse: 0.5,
        ..State::default()
    };
    let initial = Command {
        state,
        ..Command::default()
    };
    process(&mut t, 33, Some(&input), &[initial], &[], &mut out).unwrap();
    process(
        &mut clean,
        33,
        Some(&input),
        &[initial],
        &[],
        &mut reference,
    )
    .unwrap();
    let before = t.control(0).unwrap();
    out.fill(17.);
    let invalid = Command {
        offset: 511,
        element: 1,
        ..Command::default()
    };
    assert!(process(&mut t, 512, Some(&input), &[], &[invalid], &mut out).is_err());
    state.gain = f32::MAX;
    let overflow = Command {
        offset: 511,
        changed: 2,
        state,
        ..Command::default()
    };
    let mut hot = [0.125; 512];
    hot[511] = f32::MAX;
    assert!(process(&mut t, 512, Some(&hot), &[], &[overflow], &mut out).is_err());
    assert_eq!(out, [17.; 2050]);
    assert_eq!(t.control(0).unwrap(), before);
    for frames in [1, 31, 32, 33, 511, 512, 513, 1023, 1024, 1025] {
        process(&mut t, frames, Some(&input), &[], &[], &mut out).unwrap();
        process(&mut clean, frames, Some(&input), &[], &[], &mut reference).unwrap();
        assert_eq!(
            &out[..frames as usize * 2],
            &reference[..frames as usize * 2]
        );
    }
    let before = t.control(0).unwrap();
    process(&mut t, 0, None, &[], &[], &mut []).unwrap();
    assert_eq!(before, t.control(0).unwrap());
}
#[test]
fn diagnostics_silence_reentry_and_reset() {
    let mut t = Session::new(bank(), &[Description::default()], 48000, 1, false).unwrap();
    let state = State {
        screen_reference: 1,
        extent: [1.; 3],
        ..State::default()
    };
    let initial = Command {
        state,
        ..Command::default()
    };
    let mut out = [0.; 128];
    assert_eq!(
        process(&mut t, 32, None, &[initial], &[], &mut out)
            .unwrap()
            .count,
        0
    );
    let report = process(&mut t, 32, Some(&[0.1; 32]), &[], &[], &mut out).unwrap();
    assert_eq!(report.count, 2);
    assert_eq!(report.records[0].kind, 2);
    assert_eq!(report.records[1].kind, 3);
    let report = t
        .process(
            32,
            [Some(&[0.1; 32][..])].into_iter(),
            &[],
            &[],
            [0.; 3],
            report.mask,
            &mut out,
        )
        .unwrap();
    assert_eq!(report.count, 0);
    for _ in 0..4 {
        process(&mut t, 64, None, &[], &[], &mut out).unwrap();
    }
    process(&mut t, 64, Some(&[0.1; 64]), &[], &[], &mut out).unwrap();
    assert!(out.iter().any(|x| *x != 0.));
    t.reset();
    assert!(t.control(0).is_err());
    t.process(64, std::iter::empty(), &[], &[], [0.; 3], 0, &mut out)
        .unwrap();
    assert_eq!(out, [0.; 128]);
}
