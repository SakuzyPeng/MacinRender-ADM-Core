use mradm_dsp::live_vbap::{LEVEL, Mixer, PAN, VbapCommand};
fn command(
    element: u32,
    offset: u32,
    duration: u32,
    fields: u32,
    coefficient_offset: u64,
    level: f32,
) -> VbapCommand {
    VbapCommand {
        element,
        offset,
        duration,
        fields,
        coefficient_offset,
        level,
        reserved: 0,
    }
}
#[test]
fn output_precedes_each_step_and_deadlines_are_independent() {
    let mut m = Mixer::new(1, 2).unwrap();
    let initial = [command(0, 0, 0, PAN | LEVEL, 0, 1.)];
    let events = [command(0, 2, 4, LEVEL, 0, 0.), command(0, 3, 2, PAN, 2, 0.)];
    let mut output = [17.; 14];
    m.process(
        6,
        [Some(&[1.; 6][..])].into_iter(),
        &initial,
        &events,
        &[1., 0., 0., 1.],
        &mut output,
    )
    .unwrap();
    assert_eq!(
        output,
        [
            1., 0., 1., 0., 1., 0., 0.75, 0., 0.25, 0.25, 0., 0.25, 17., 17.
        ]
    );
    let mut gains = [0.; 6];
    let state = m.snapshot(0, &mut gains).unwrap();
    assert_eq!(gains, [0., 1., 0., 1., -0.5, 0.5]);
    assert_eq!(
        (state.current_level, state.target_level, state.level_step),
        (0., 0., -0.25)
    );
    assert_eq!((state.pan_remaining, state.level_remaining), (0, 0));
}
#[test]
fn repeated_targets_restart_and_same_offset_order_is_retained() {
    let mut m = Mixer::new(1, 1).unwrap();
    let mut output = [0.; 4];
    m.process(
        2,
        [Some(&[1.; 4][..])].into_iter(),
        &[command(0, 0, 0, 3, 0, 0.)],
        &[command(0, 0, 4, LEVEL, 0, 1.)],
        &[1.],
        &mut output,
    )
    .unwrap();
    assert_eq!(&output[..2], &[0., 0.25]);
    m.process(
        4,
        [Some(&[1.; 4][..])].into_iter(),
        &[],
        &[command(0, 0, 4, LEVEL, 0, 1.)],
        &[],
        &mut output,
    )
    .unwrap();
    assert_eq!(output, [0.5, 0.625, 0.75, 0.875]);
    let mut snapshot = [0.; 3];
    assert_eq!(m.snapshot(0, &mut snapshot).unwrap().current_level, 1.);
    m.process(
        1,
        [Some(&[1.][..])].into_iter(),
        &[],
        &[command(0, 0, 0, 3, 0, 0.5), command(0, 0, 0, 3, 1, 0.25)],
        &[0.5, 1.],
        &mut output,
    )
    .unwrap();
    assert_eq!(output[0], 0.25);
    // Repeating a spatial target also recomputes a full-duration ramp from the current value.
    m.process(
        2,
        [None].into_iter(),
        &[],
        &[command(0, 0, 4, PAN, 0, 0.)],
        &[0.],
        &mut output,
    )
    .unwrap();
    m.process(
        1,
        [None].into_iter(),
        &[],
        &[command(0, 0, 4, PAN, 0, 0.)],
        &[0.],
        &mut output,
    )
    .unwrap();
    let state = m.snapshot(0, &mut snapshot).unwrap();
    assert_eq!(snapshot, [0.375, 0., -0.125]);
    assert_eq!(state.pan_remaining, 3);
}
#[test]
fn silence_zero_frames_initialization_reset_and_empty_generation() {
    let mut m = Mixer::new(1, 2).unwrap();
    let original = m.clone();
    m.process(0, [None].into_iter(), &[], &[], &[], &mut [])
        .unwrap();
    assert_eq!(m, original);
    m.process(
        0,
        [None].into_iter(),
        &[command(0, 0, 0, 3, 0, 0.5)],
        &[],
        &[1., 0.],
        &mut [],
    )
    .unwrap();
    let mut g = [0.; 6];
    assert_eq!(m.snapshot(0, &mut g).unwrap().current_level, 0.5);
    let initialized = m.clone();
    let mut out = [17.; 8];
    m.process(
        3,
        [None].into_iter(),
        &[],
        &[command(0, 0, 3, 3, 0, 1.)],
        &[0., 1.],
        &mut out,
    )
    .unwrap();
    assert_eq!(out, [0., 0., 0., 0., 0., 0., 17., 17.]);
    let state = m.snapshot(0, &mut g).unwrap();
    assert_eq!(state.current_level, 1.);
    assert_eq!(g[..2], [0., 1.]);
    assert_ne!(m, initialized);
    assert_eq!(initialized.snapshot(0, &mut g).unwrap().current_level, 0.5);
    m.reset();
    assert_eq!(m, original);
    let mut empty = Mixer::new(0, 2).unwrap();
    empty
        .process(3, [].into_iter(), &[], &[], &[], &mut out)
        .unwrap();
    assert_eq!(out, [0., 0., 0., 0., 0., 0., 17., 17.]);
}
#[test]
fn late_invalid_commands_preserve_entire_call_and_recovery() {
    let mut m = Mixer::new(1, 2).unwrap();
    let before = m.clone();
    let initial = [command(0, 0, 0, 3, 0, 1.)];
    let good = command(0, 0, 2, LEVEL, 0, 0.5);
    let mut bads = vec![
        command(1, 1, 0, LEVEL, 0, 0.),
        command(0, 2, 0, LEVEL, 0, 0.),
        command(0, 1, 0, 0, 0, 0.),
        command(0, 1, 0, 4, 0, 0.),
        command(0, 1, 0, PAN, u64::MAX, 0.),
        command(0, 1, 0, PAN, 1, 0.),
        command(0, 1, 0, LEVEL, 0, f32::NAN),
    ];
    let mut reserved = good;
    reserved.reserved = 1;
    bads.push(reserved);
    for bad in bads {
        let mut out = [17.; 4];
        assert!(
            m.process(
                2,
                [Some(&[1.; 2][..])].into_iter(),
                &initial,
                &[good, bad],
                &[1., 0.],
                &mut out
            )
            .is_err()
        );
        assert_eq!(out, [17.; 4]);
        assert_eq!(m, before);
    }
    let mut out = [17.; 4];
    assert!(
        m.process(
            2,
            [None].into_iter(),
            &initial,
            &[command(0, 1, 1, LEVEL, 0, 0.), good],
            &[1., 0.],
            &mut out
        )
        .is_err()
    );
    assert!(
        m.process(
            2,
            [Some(&[1.][..])].into_iter(),
            &initial,
            &[],
            &[1., 0.],
            &mut out
        )
        .is_err()
    );
    assert!(
        m.process(2, [].into_iter(), &initial, &[], &[1., 0.], &mut out)
            .is_err()
    );
    assert!(
        m.process(
            2,
            [None].into_iter(),
            &initial,
            &[],
            &[f32::INFINITY, 0.],
            &mut out
        )
        .is_err()
    );
    assert!(
        m.process(
            2,
            [None].into_iter(),
            &initial,
            &[],
            &[1., 0.],
            &mut out[..3]
        )
        .is_err()
    );
    assert!(
        m.process(
            u32::MAX,
            [None].into_iter(),
            &initial,
            &[],
            &[1., 0.],
            &mut out
        )
        .is_err()
    );
    assert!(
        m.process(
            0,
            [None].into_iter(),
            &initial,
            &[good],
            &[1., 0.],
            &mut out
        )
        .is_err()
    );
    let mut invalid_initial = initial;
    invalid_initial[0].duration = 1;
    assert!(
        m.process(
            2,
            [None].into_iter(),
            &invalid_initial,
            &[],
            &[1., 0.],
            &mut out
        )
        .is_err()
    );
    assert_eq!(out, [17.; 4]);
    assert_eq!(m, before);
    m.process(
        2,
        [Some(&[1.; 2][..])].into_iter(),
        &initial,
        &[good],
        &[1., 0.],
        &mut out,
    )
    .unwrap();
    assert_eq!(out, [1., 0., 0.75, 0.]);
    assert!(Mixer::new(1, 0).is_err());
    assert!(Mixer::new(u32::MAX, u32::MAX).is_err());
}
#[test]
fn source_order_and_nonfinite_pcm_are_not_optimized_away() {
    let mut m = Mixer::new(3, 1).unwrap();
    let mut out = [17.];
    let init = (0..3)
        .map(|e| command(e, 0, 0, 3, 0, 1.))
        .collect::<Vec<_>>();
    m.process(
        1,
        [Some(&[1e20][..]), Some(&[-1e20][..]), Some(&[1.][..])].into_iter(),
        &init,
        &[],
        &[1.],
        &mut out,
    )
    .unwrap();
    assert_eq!(out, [1.]);
    let mut m = Mixer::new(1, 1).unwrap();
    m.process(
        1,
        [Some(&[f32::NAN][..])].into_iter(),
        &[command(0, 0, 0, 3, 0, 0.)],
        &[],
        &[0.],
        &mut out,
    )
    .unwrap();
    assert!(out[0].is_nan());
}
#[test]
fn arbitrary_chunking_preserves_pcm_and_exact_states() {
    let frames = 2109;
    let source: Vec<f32> = (0..frames).map(|i| (i % 73) as f32 / 74. - 0.5).collect();
    let coefficients = [1., 0., 0.25, 0.75, 0.5, 0.5];
    let init = [command(0, 0, 0, 3, 0, 1.), command(1, 0, 0, 3, 2, 0.5)];
    let events = [
        command(0, 0, 73, PAN, 2, 0.),
        command(1, 1, 99, LEVEL, 0, 0.125),
        command(0, 8, 2001, 3, 4, 0.5),
        command(0, 511, 1500, LEVEL, 0, 0.75),
        command(1, 1023, 1086, PAN, 0, 0.),
        command(1, 2000, 1000, 3, 2, 0.25),
    ];
    let mut whole = Mixer::new(2, 2).unwrap();
    let mut split = whole.clone();
    let mut a = vec![17.; frames * 2];
    let mut b = a.clone();
    whole
        .process(
            frames as u32,
            [Some(source.as_slice()), Some(source.as_slice())].into_iter(),
            &init,
            &events,
            &coefficients,
            &mut a,
        )
        .unwrap();
    let mut offset = 0;
    for n in [1, 7, 37, 511, 1024, 529] {
        let commands = events
            .iter()
            .filter(|e| e.offset as usize >= offset && (e.offset as usize) < offset + n)
            .map(|e| VbapCommand {
                offset: e.offset - offset as u32,
                ..*e
            })
            .collect::<Vec<_>>();
        split
            .process(
                n as u32,
                [
                    Some(&source[offset..offset + n]),
                    Some(&source[offset..offset + n]),
                ]
                .into_iter(),
                if offset == 0 { &init } else { &[] },
                &commands,
                &coefficients,
                &mut b[offset * 2..(offset + n) * 2],
            )
            .unwrap();
        offset += n;
    }
    assert_eq!(offset, frames);
    assert_eq!(whole, split);
    assert!(
        a.iter()
            .zip(b.iter())
            .all(|(a, b)| a.to_bits() == b.to_bits())
    );
}
