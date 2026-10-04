use mradm_dsp::{
    convolution::{LiveConvolver, OlaConvolver},
    diffuse::DiffuseDelay,
};

// Independent double-precision DFT and time-domain FIR oracle. Neither calls
// the production FFT or reads internal convolution state.
fn spectrum(taps: &[Vec<f32>; 2], length: usize) -> Vec<f32> {
    (0..=length / 2)
        .flat_map(|band| {
            taps.iter().flat_map(move |ear| {
                let (mut re, mut im) = (0.0, 0.0);
                for (tap, value) in ear.iter().enumerate() {
                    let phase = -std::f64::consts::TAU * (band * tap) as f64 / length as f64;
                    re += *value as f64 * phase.cos();
                    im += *value as f64 * phase.sin();
                }
                [re as f32, im as f32]
            })
        })
        .collect()
}
fn filters(length: usize) -> [[Vec<f32>; 2]; 3] {
    std::array::from_fn(|filter| {
        std::array::from_fn(|ear| {
            let mut taps = vec![0.0; length];
            taps[(filter * 3 + ear) % length] = 0.4 + ear as f32 * 0.2;
            taps[length - 1 - filter] = -0.3 + filter as f32 * 0.1;
            taps
        })
    })
}
fn signal(length: usize) -> Vec<f32> {
    (0..length)
        .map(|i| (i * 37 % 101) as f32 / 101.0 - 0.5)
        .collect()
}
fn fir(taps: &[f32], input: &[f32], time: usize) -> f64 {
    taps.iter()
        .enumerate()
        .filter(|(i, _)| *i <= time)
        .map(|(i, h)| *h as f64 * input.get(time - i).copied().unwrap_or(0.0) as f64)
        .sum()
}
fn near(actual: f32, expected: f64) {
    assert!(
        (actual as f64 - expected).abs() < 1.5e-6,
        "{actual} != {expected}"
    );
}

#[test]
fn live_full_fir_stereo_partitioning_tail_and_reset() {
    let taps = filters(64)[0].clone();
    let hrtf = spectrum(&taps, 64);
    let mut input = signal(200);
    input.resize(384, 0.0);
    for block in [1, 7, 37, 64] {
        let mut convolver = LiveConvolver::new(64, 64, 48000).unwrap();
        let mut state = convolver.make_state();
        let mut left = vec![0.0; input.len()];
        let mut right = left.clone();
        for start in (0..input.len()).step_by(block) {
            let end = (start + block).min(input.len());
            convolver
                .process(
                    &mut state,
                    &hrtf,
                    &input[start..end],
                    &mut left[start..end],
                    &mut right[start..end],
                    false,
                )
                .unwrap();
        }
        for i in 0..input.len() {
            near(left[i], fir(&taps[0], &input, i));
            near(right[i], fir(&taps[1], &input, i));
        }
        assert_eq!(state.tail_remaining(), 0);
        state.reset();
        assert!(!state.initialized());
        let mut other = convolver.make_state();
        let (mut a, mut b, mut c, mut d) = ([0.0; 37], [0.0; 37], [0.0; 37], [0.0; 37]);
        convolver
            .process(&mut state, &hrtf, &input[..37], &mut a, &mut b, false)
            .unwrap();
        convolver
            .process(&mut other, &hrtf, &input[..37], &mut c, &mut d, false)
            .unwrap();
        assert_eq!((a, b), (c, d));
    }
}

#[test]
fn live_interrupted_control_and_explicit_ramp_use_same_input_history() {
    let taps = filters(32);
    let hrtfs = taps.each_ref().map(|t| spectrum(t, 32));
    let input = signal(73);
    let (mut left, mut right) = (vec![0.0; 73], vec![0.0; 73]);
    let mut convolver = LiveConvolver::new(32, 37, 1000).unwrap();
    let mut state = convolver.make_state();
    // Ten-sample control fade interrupted halfway, then a four-sample ramp.
    let mut offset = 0;
    for (frames, filter, ramp) in [
        (37, 0, false),
        (3, 1, false),
        (2, 1, false),
        (4, 2, true),
        (10, 2, false),
        (17, 2, false),
    ] {
        let end = offset + frames;
        convolver
            .process(
                &mut state,
                &hrtfs[filter],
                &input[offset..end],
                &mut left[offset..end],
                &mut right[offset..end],
                ramp,
            )
            .unwrap();
        offset = end;
    }
    for time in 0..73 {
        let weights = if time < 37 {
            [1.0, 0.0, 0.0]
        } else if time < 42 {
            let a = (time - 37) as f64 / 10.0;
            [1.0 - a, a, 0.0]
        } else if time < 46 {
            let a = (time - 42) as f64 / 4.0;
            [0.5 * (1.0 - a), 0.5 * (1.0 - a), a]
        } else {
            [0.0, 0.0, 1.0]
        };
        for (ear, output) in [&left, &right].into_iter().enumerate() {
            let expected = (0..3)
                .map(|f| weights[f] * fir(&taps[f][ear], &input, time))
                .sum();
            near(output[time], expected);
        }
    }
}

#[test]
fn ola_short_segments_crossfades_gains_and_silent_gaps_match_direct_fir() {
    let taps = filters(24);
    let hrtfs = taps.each_ref().map(|t| spectrum(t, 64));
    let input = signal(90);
    let mut convolver = OlaConvolver::new(64, 23, 32).unwrap();
    let (mut left, mut right) = (vec![0.125; 160], vec![-0.25; 160]);
    let mut expected = [vec![0.125_f64; 160], vec![-0.25_f64; 160]];
    let mut offset = 0;
    for (frames, start, end, gain0, gain1) in [
        (1, 0, Some(1), 0.5, -0.2),
        (2, 1, None, 0.7, 0.7),
        (7, 1, Some(2), -0.4, 0.3),
        (1, 2, Some(0), 1.2, 0.9),
        (17, 0, None, 0.8, 0.8),
        (32, 2, Some(1), 0.0, 0.6),
    ] {
        let samples = &input[offset..offset + frames];
        convolver
            .process(
                samples,
                &hrtfs[start],
                gain0,
                end.map(|f| (hrtfs[f].as_slice(), gain1)),
                &mut left[offset..offset + frames],
                &mut right[offset..offset + frames],
            )
            .unwrap();
        for ear in 0..2 {
            for t in 0..frames + 23 {
                let start_value = gain0 as f64 * fir(&taps[start][ear], samples, t);
                let value = if let Some(end_filter) = end {
                    let end_value = gain1 as f64 * fir(&taps[end_filter][ear], samples, t);
                    let a = if t >= frames {
                        1.0
                    } else if frames == 1 {
                        0.0
                    } else {
                        t as f64 / (frames - 1) as f64
                    };
                    start_value * (1.0 - a) + end_value * a
                } else {
                    start_value
                };
                expected[ear][offset + t] += value;
            }
        }
        offset += frames;
        // A gap shorter than the overlap exercises residual tail advancement.
        convolver
            .advance_silence(
                &mut left[offset..offset + 3],
                &mut right[offset..offset + 3],
            )
            .unwrap();
        offset += 3;
    }
    while offset < left.len() {
        let end = (offset + 19).min(left.len());
        convolver
            .advance_silence(&mut left[offset..end], &mut right[offset..end])
            .unwrap();
        offset = end;
    }
    for i in 0..left.len() {
        near(left[i], expected[0][i]);
        near(right[i], expected[1][i]);
    }
    convolver.reset();
    let (mut a, mut b) = ([0.0; 32], [0.0; 32]);
    convolver.advance_silence(&mut a, &mut b).unwrap();
    assert_eq!((a, b), ([0.0; 32], [0.0; 32]));
}

#[test]
fn invalid_and_empty_calls_do_not_advance_state_or_modify_output() {
    for (n, block, rate) in [
        (0, 1, 48000),
        (3, 1, 48000),
        (64, 0, 48000),
        (64, usize::MAX, 48000),
        (64, 8, 0),
    ] {
        assert!(LiveConvolver::new(n, block, rate).is_err());
    }
    for (n, overlap, block) in [(3, 1, 1), (64, 64, 1), (64, 63, 2), (64, 0, 0)] {
        assert!(OlaConvolver::new(n, overlap, block).is_err());
    }
    let hrtf = spectrum(&filters(16)[0], 64);
    let mut convolver = LiveConvolver::new(64, 32, 48000).unwrap();
    let mut state = convolver.make_state();
    convolver
        .process(&mut state, &hrtf, &[], &mut [], &mut [], true)
        .unwrap();
    assert!(!state.initialized());
    let (mut left, mut right) = ([42.0; 32], [43.0; 32]);
    let mut foreign = LiveConvolver::new(32, 32, 48000).unwrap().make_state();
    assert!(
        convolver
            .process(
                &mut foreign,
                &hrtf,
                &[0.0; 32],
                &mut left,
                &mut right,
                false
            )
            .is_err()
    );
    assert!(
        convolver
            .process(
                &mut state,
                &hrtf,
                &[f32::NAN; 32],
                &mut left,
                &mut right,
                false
            )
            .is_err()
    );
    assert!(!state.initialized());
    assert_eq!((left, right), ([42.0; 32], [43.0; 32]));
    let mut ola = OlaConvolver::new(64, 15, 32).unwrap();
    let mut reference = OlaConvolver::new(64, 15, 32).unwrap();
    for c in [&mut ola, &mut reference] {
        c.process(&[0.3; 32], &hrtf, 0.8, None, &mut left, &mut right)
            .unwrap();
    }
    assert!(
        ola.process(
            &[0.0; 32],
            &hrtf,
            1.0,
            Some((&hrtf[..8], 1.0)),
            &mut left,
            &mut right
        )
        .is_err()
    );
    assert!(
        ola.process(
            &[0.0; 32],
            &hrtf,
            f32::INFINITY,
            None,
            &mut left,
            &mut right
        )
        .is_err()
    );
    ola.process(&[], &hrtf, 1.0, None, &mut [], &mut [])
        .unwrap();
    let (mut a, mut b, mut c, mut d) = ([0.0; 32], [0.0; 32], [0.0; 32], [0.0; 32]);
    ola.advance_silence(&mut a, &mut b).unwrap();
    reference.advance_silence(&mut c, &mut d).unwrap();
    assert_eq!((a, b), (c, d));
}

#[test]
fn diffuse_delay_and_live_ramps_match_direct_tapped_delay() {
    let input = signal(128);
    let mut delay = DiffuseDelay::default();
    let mut delayed = vec![0.0; input.len()];
    for start in (0..input.len()).step_by(7) {
        let end = (start + 7).min(input.len());
        delay
            .process(&input[start..end], &mut delayed[start..end])
            .unwrap();
    }
    let mut taps = vec![0.0; 32];
    for (i, sign) in [
        (3, 1.0),
        (7, -1.0),
        (11, 1.0),
        (17, 1.0),
        (19, -1.0),
        (23, 1.0),
        (29, -1.0),
        (31, -1.0),
    ] {
        taps[i] = sign * 0.35355339;
    }
    for (i, sample) in delayed.iter().enumerate() {
        near(*sample, fir(&taps, &input, i));
    }
    delay.reset();
    let mut mixed = input.clone();
    delay.mix(&mut mixed, [0.2, 1.0], [0.0, 1.0]).unwrap();
    for i in 0..mixed.len() {
        let a = i as f64 / mixed.len() as f64;
        near(
            mixed[i],
            (0.2 + 0.8 * a) * (input[i] as f64 * (1.0 - a) + fir(&taps, &input, i) * a),
        );
    }
    delay.mix(&mut mixed, [0.0, 0.0], [1.0, 1.0]).unwrap();
    assert!(mixed.iter().all(|v| *v == 0.0));
    delay.process(&[0.0; 32], &mut delayed[..32]).unwrap();
    assert!(delayed[..32].iter().all(|v| *v == 0.0));
}
