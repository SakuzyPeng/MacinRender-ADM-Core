use mradm_dsp::ear_post::{DELAY, FilterBank, Processor, TAPS};
use std::sync::Arc;

fn bank(channels: usize) -> Arc<FilterBank> {
    let firs: Vec<_> = (0..channels * TAPS)
        .map(|i| ((i * 13 % 31) as f32 - 15.) / 1024.)
        .collect();
    Arc::new(FilterBank::new(channels, &firs, DELAY).unwrap())
}

#[test]
fn one_frame_calls_retain_the_unconsumed_tail() {
    let mut firs = [0.; TAPS];
    firs[..3].copy_from_slice(&[1., 0.5, 0.25]);
    let mut processor =
        Processor::new(Arc::new(FilterBank::new(1, &firs, DELAY).unwrap()), 1).unwrap();
    for (diffuse, expected) in [(1., 1.), (0., 0.5), (0., 0.25), (0., 0.)] {
        let mut direct = [0.];
        processor.process(&mut direct, &[diffuse], 1).unwrap();
        assert!(
            (direct[0] - expected).abs() < 1e-6,
            "lost a short-block FIR tail"
        );
    }
}

#[test]
fn irregular_chunks_match_an_independent_double_time_domain_fir() {
    let frames = 1700;
    for channels in [1, 2, 6] {
        let firs: Vec<_> = (0..channels * TAPS)
            .map(|i| ((i * 13 % 31) as f32 - 15.) / 1024.)
            .collect();
        let bank = Arc::new(FilterBank::new(channels, &firs, DELAY).unwrap());
        let mut dry = vec![0.; frames * channels];
        let mut wet = dry.clone();
        for t in 0..1000 {
            for ch in 0..channels {
                dry[t * channels + ch] = ((t * 7 + ch * 3) % 31) as f32 / 32. - 0.5;
                wet[t * channels + ch] = ((t * 17 + ch * 7) % 31) as f32 / 32. - 0.5;
            }
        }
        let mut reference = vec![0_f64; dry.len()];
        for t in 0..frames {
            for ch in 0..channels {
                let mut value = if t >= DELAY {
                    dry[(t - DELAY) * channels + ch] as f64
                } else {
                    0.
                };
                for tap in 0..TAPS.min(t + 1) {
                    value += wet[(t - tap) * channels + ch] as f64 * firs[ch * TAPS + tap] as f64;
                }
                reference[t * channels + ch] = value;
            }
        }
        for capacity in [1, 37, 1024, 2048] {
            let mut processor = Processor::new(Arc::clone(&bank), capacity).unwrap();
            let mut output = dry.clone();
            let mut start = 0;
            let mut step = 0;
            let pattern = [
                1, 7, 37, 127, 254, 255, 256, 510, 511, 512, 1023, 1024, 2048,
            ];
            while start < frames {
                let count = pattern[step % pattern.len()]
                    .min(capacity)
                    .min(frames - start);
                let range = start * channels..(start + count) * channels;
                processor
                    .process(&mut output[range.clone()], &wet[range], count)
                    .unwrap();
                start += count;
                step += 1;
            }
            for (&actual, &expected) in output.iter().zip(&reference) {
                assert!((actual as f64 - expected).abs() <= 2e-5 + 2e-5 * expected.abs());
            }
        }
    }
}

#[test]
fn direct_delay_is_exact_and_channels_do_not_leak() {
    let channels = 2;
    let mut processor = Processor::new(
        Arc::new(FilterBank::new(channels, &[0.; TAPS * 2], DELAY).unwrap()),
        1024,
    )
    .unwrap();
    let input: Vec<_> = (0..1800)
        .flat_map(|i| [((i % 17) as f32 - 8.) / 16., 0.])
        .collect();
    let mut output = input.clone();
    let mut start = 0;
    for count in [1, 7, 255, 256, 511, 512, 258] {
        let range = start * channels..(start + count) * channels;
        processor
            .process(
                &mut output[range.clone()],
                &vec![0.; count * channels],
                count,
            )
            .unwrap();
        start += count;
    }
    assert_eq!(start, 1800);
    for t in 0..1800 {
        for ch in 0..channels {
            let expected = if t >= DELAY {
                input[(t - DELAY) * channels + ch]
            } else {
                0.
            };
            assert_eq!(output[t * channels + ch].to_bits(), expected.to_bits());
        }
    }
    let mut firs = [0.; TAPS * 2];
    firs[0] = 1.;
    firs[TAPS + 3] = 0.5;
    let mut p = Processor::new(Arc::new(FilterBank::new(2, &firs, DELAY).unwrap()), 8).unwrap();
    let mut dry = [0.; 16];
    let mut wet = [0.; 16];
    wet[1] = 1.;
    p.process(&mut dry, &wet, 8).unwrap();
    for t in 0..8 {
        assert_eq!(dry[t * 2], 0.);
        assert!((dry[t * 2 + 1] - if t == 3 { 0.5 } else { 0. }).abs() < 1e-6);
    }
}

#[test]
fn reset_independent_instances_and_invalid_calls_preserve_state() {
    let bank = bank(2);
    let mut a = Processor::new(Arc::clone(&bank), 32).unwrap();
    let mut b = Processor::new(bank, 64).unwrap();
    let input = [0.25; 14];
    let mut first = [0.125; 14];
    let mut second = first;
    a.process(&mut first, &input, 7).unwrap();
    b.process(&mut second, &input, 7).unwrap();
    let mut sentinel = [42.; 14];
    assert!(a.process(&mut sentinel, &input, 33).is_err());
    assert!(a.process(&mut sentinel[..13], &input, 6).is_err());
    assert!(a.process(&mut sentinel, &input[..1], 7).is_err());
    assert!(a.process(&mut sentinel, &input, usize::MAX).is_err());
    assert_eq!(sentinel, [42.; 14]);
    a.process(&mut [], &[], 0).unwrap();
    first.fill(0.125);
    second = first;
    a.process(&mut first, &input, 7).unwrap();
    b.process(&mut second, &input, 7).unwrap();
    for (x, y) in first.iter().zip(second) {
        assert!((x - y).abs() < 2e-6);
    }
    a.process(&mut [f32::NAN; 2], &[f32::INFINITY; 2], 1)
        .unwrap();
    a.reset();
    b.reset();
    first.fill(0.);
    second = first;
    a.process(&mut first, &[0.; 14], 7).unwrap();
    b.process(&mut second, &[0.; 14], 7).unwrap();
    assert_eq!(first, [0.; 14]);
    assert_eq!(second, first);
}

#[test]
fn invalid_preparation_and_unused_output_prefix() {
    assert!(FilterBank::new(0, &[], DELAY).is_err());
    assert!(FilterBank::new(usize::MAX, &[], DELAY).is_err());
    assert!(FilterBank::new(1, &[0.; TAPS - 1], DELAY).is_err());
    assert!(FilterBank::new(1, &[0.; TAPS], DELAY - 1).is_err());
    assert!(FilterBank::new(1, &[f32::NAN; TAPS], DELAY).is_err());
    let bank = bank(1);
    assert!(Processor::new(Arc::clone(&bank), 0).is_err());
    assert!(Processor::new(Arc::clone(&bank), usize::MAX).is_err());
    assert!(Processor::new(Arc::clone(&bank), 1 << 24).is_err());
    let huge = Arc::new(FilterBank::new(1, &[f32::MAX; TAPS], DELAY).unwrap());
    assert!(Processor::new(huge, 32).is_err());
    let mut p = Processor::new(bank, 32).unwrap();
    let mut out = [42.; 12];
    p.process(&mut out, &[0.; 12], 2).unwrap();
    assert_eq!(&out[2..], &[42.; 10]);
}
