use mradm_dsp::{
    gain::GainBank,
    peak_guard::{CEILING, StereoPeakGuard},
};

#[test]
fn gain_endpoints_retarget_missing_targets_and_reset() {
    let mut bank = GainBank::new(2, 1000, 4).unwrap();
    bank.set_targets(&[0.5, 0.25]).unwrap();
    bank.fill(&mut []).unwrap(); // does not start either ramp
    bank.set_targets(&[0.25, 0.5]).unwrap();
    let mut frame = [0.; 2];
    bank.fill(&mut frame).unwrap();
    assert_eq!(frame, [0.25, 0.5]);
    bank.set_targets(&[1.]).unwrap(); // second channel also returns to unity
    let mut curve = [0.; 8];
    bank.fill(&mut curve).unwrap();
    assert_eq!(
        [curve[0], curve[2], curve[4], curve[6]],
        [0.25, 0.5, 0.75, 1.]
    );
    assert_eq!(curve[1], 0.5);
    assert!((curve[7] - 1.).abs() < 1e-6);
    bank.set_targets(&[0.]).unwrap();
    bank.fill(&mut frame).unwrap(); // returns 1, next value is 2/3
    bank.set_targets(&[0.5]).unwrap();
    bank.fill(&mut frame).unwrap();
    assert!((frame[0] - 2. / 3.).abs() < 1e-6);
    bank.reset();
    bank.set_targets(&[0.125, 0.75, f32::NAN]).unwrap(); // excess ignored
    bank.fill(&mut frame).unwrap();
    assert_eq!(frame, [0.125, 0.75]);
    let mut instant = GainBank::new(1, 48000, 0).unwrap();
    instant.fill(&mut [0.]).unwrap();
    instant.set_targets(&[0.]).unwrap();
    instant.fill(&mut frame).unwrap();
    assert_eq!(frame, [0.; 2]);
}

#[test]
fn gain_errors_are_atomic_and_partitioning_is_exact() {
    for channels in [1, 2, 12, 64] {
        let mut whole = GainBank::new(channels, 44100, 20).unwrap();
        let mut split = whole.clone();
        let mut expected = vec![0.25; 1103 * channels];
        let mut actual = expected.clone();
        whole.apply(&mut expected[..channels]).unwrap();
        split.apply(&mut actual[..channels]).unwrap();
        let targets = vec![0.125; channels];
        whole.set_targets(&targets).unwrap();
        split.set_targets(&targets).unwrap();
        whole.apply(&mut expected[channels..]).unwrap();
        for block in actual[channels..].chunks_mut(channels * 17) {
            split.set_targets(&targets).unwrap(); // repeated target must not restart
            split.apply(block).unwrap();
        }
        assert_eq!(actual, expected);
    }
    let mut tested = GainBank::new(2, 48000, 20).unwrap();
    tested.set_targets(&[0.5, 0.25]).unwrap();
    let mut reference = tested.clone();
    assert!(tested.set_targets(&[0.75, f32::NAN]).is_err());
    let mut odd = [42.; 3];
    assert!(tested.apply(&mut odd).is_err());
    assert_eq!(odd, [42.; 3]);
    let (mut a, mut b) = ([1.; 64], [1.; 64]);
    tested.apply(&mut a).unwrap();
    reference.apply(&mut b).unwrap();
    assert_eq!(a, b);
    assert!(GainBank::new(0, 48000, 20).is_err());
    assert!(GainBank::new(2, 0, 20).is_err());
    assert!(GainBank::new(usize::MAX, 48000, 20).is_err());
}

fn protect(input: &[f32], rate: u32, chunk: usize, volume: f32) -> Vec<f32> {
    let mut guard = StereoPeakGuard::new(rate).unwrap();
    assert_eq!(guard.lookahead_frames(), (rate as usize / 200).max(1));
    let mut output = vec![0.; input.len()];
    let (mut read, mut written) = (0, 0);
    while read < input.len() || guard.buffered_frames() != 0 {
        let take = chunk
            .min((input.len() - read) / 2)
            .min(guard.writable_frames());
        guard.push(&input[read..read + take * 2]).unwrap();
        read += take * 2;
        let got = guard
            .pop(&mut output[written..], volume, read == input.len())
            .unwrap();
        assert!(take != 0 || got != 0);
        written += got * 2;
    }
    assert_eq!(written, input.len());
    output
}

#[test]
fn peak_guard_ceiling_stereo_transparency_partitioning_and_wraparound() {
    let input: Vec<_> = (0..11003)
        .flat_map(|i| {
            let sample = if i % 997 == 0 { 4. } else { 0.125 };
            [sample, sample * -0.25]
        })
        .collect();
    for rate in [8000, 44100, 48000, 96000, 192000] {
        let expected = protect(&input, rate, 4096, 0.8);
        for chunk in [1, 37, 512] {
            assert_eq!(protect(&input, rate, chunk, 0.8), expected);
        }
        for frame in expected.as_chunks::<2>().0 {
            assert!(frame[0].abs() <= CEILING + 1e-6);
            assert_eq!(frame[1], frame[0] * -0.25);
        }
        assert_eq!(protect(&input, rate, 257, 0.), vec![0.; input.len()]);
        assert_eq!(
            protect(&input, rate, 17, 0.1),
            input.iter().map(|v| v * 0.1).collect::<Vec<_>>()
        );
    }
}

#[test]
fn peak_guard_short_eos_reset_nonfinite_and_rejected_calls() {
    assert!(StereoPeakGuard::new(0).is_err());
    let mut guard = StereoPeakGuard::new(48000).unwrap();
    guard.push(&[4.; 64]).unwrap();
    let mut output = [42.; 64];
    assert_eq!(guard.pop(&mut output, 1., false).unwrap(), 0);
    let mut reference = guard.clone();
    assert!(guard.push(&[0.; 3]).is_err());
    assert!(
        guard
            .push(&vec![0.; (guard.writable_frames() + 1) * 2])
            .is_err()
    );
    assert!(guard.pop(&mut output, f32::NAN, true).is_err());
    assert!(guard.pop(&mut output[..3], 1., true).is_err());
    assert_eq!(output, [42.; 64]);
    let mut expected = output;
    assert_eq!(reference.pop(&mut expected, 1., true).unwrap(), 32);
    assert_eq!(guard.pop(&mut output, 1., true).unwrap(), 32);
    assert_eq!(output, expected);
    assert_eq!(guard.pop(&mut output, 1., true).unwrap(), 0);
    guard.push(&[4.; 64]).unwrap();
    guard.reset();
    guard
        .push(&[f32::NAN, f32::INFINITY, 0.125, -0.25])
        .unwrap();
    assert_eq!(guard.pop(&mut output, 1., true).unwrap(), 2);
    assert_eq!(output[..4], [0., 0., 0.125, -0.25]);
}
