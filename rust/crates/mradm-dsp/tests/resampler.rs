use mradm_dsp::resampler::Resampler;

fn convert(resampler: &mut Resampler, input: &[f32], chunk: usize, capacity: usize) -> Vec<f32> {
    let channels = resampler.channels();
    let mut result = Vec::new();
    let mut scratch = vec![0.0; capacity * channels];
    for data in input.chunks(chunk * channels) {
        let mut consumed = 0;
        loop {
            let progress = resampler.process(&data[consumed..], &mut scratch).unwrap();
            consumed += progress.input_frames * channels;
            result.extend_from_slice(&scratch[..progress.output_frames * channels]);
            if consumed == data.len() && progress.output_frames == 0 {
                break;
            }
            assert!(progress.input_frames != 0 || progress.output_frames != 0);
        }
    }
    loop {
        let frames = resampler.finish(&mut scratch).unwrap();
        result.extend_from_slice(&scratch[..frames * channels]);
        if frames == 0 {
            break;
        }
    }
    result
}

#[test]
fn arbitrary_partitions_small_outputs_exact_lengths_and_reset() {
    for (from, to) in [
        (48000, 44100),
        (44100, 48000),
        (48000, 96000),
        (96000, 48000),
        (192000, 8000),
        (8000, 192000),
        (48000, 48000),
    ] {
        for channels in [1, 2, 12] {
            let input: Vec<_> = (0..1537 * channels)
                .map(|i| ((i * 37 % 101) as f32 - 50.0) / 100.0)
                .collect();
            let mut r = Resampler::new(channels, from, to).unwrap();
            let reference = convert(&mut r, &input, 1024, 4096);
            assert_eq!(reference.len(), r.output_length(1537).unwrap() * channels);
            r.reset();
            let candidate = convert(&mut r, &input, 7, 17);
            assert_eq!(reference.len(), candidate.len());
            let error = reference
                .iter()
                .zip(&candidate)
                .map(|(a, b)| (*a - *b).abs())
                .fold(0.0_f32, f32::max);
            assert!(
                error < 2e-6,
                "{from}->{to}, channels={channels}, error={error}"
            );
            r.reset();
            assert_eq!(convert(&mut r, &input, 1024, 4096), reference);
            if from == to {
                assert_eq!(reference, input);
            }
        }
    }
}

#[test]
fn single_frame_empty_input_and_channel_isolation() {
    for (from, to) in [
        (48000, 44100),
        (44100, 48000),
        (8000, 192000),
        (192000, 8000),
    ] {
        let mut r = Resampler::new(2, from, to).unwrap();
        assert!(convert(&mut r, &[], 1, 1).is_empty());
        r.reset();
        let result = convert(&mut r, &[1.0, 0.0], 1, 1);
        assert_eq!(result.len(), r.output_length(1).unwrap() * 2);
        assert!(result.iter().all(|v| v.is_finite()));
        assert!(
            result[0].abs() > 0.5 * (to as f32 / from as f32).min(1.0),
            "tap-zero impulse lost: {from}->{to}"
        );
        assert!(
            result
                .as_chunks::<2>()
                .0
                .iter()
                .all(|frame| frame[1] == 0.0)
        );
    }
}

fn tone(rate: u32, frequency: f64) -> Vec<f32> {
    (0..rate)
        .map(|i| (0.5 * (std::f64::consts::TAU * frequency * i as f64 / rate as f64).sin()) as f32)
        .collect()
}
fn rms(signal: &[f32]) -> f64 {
    (signal.iter().map(|v| (*v as f64).powi(2)).sum::<f64>() / signal.len() as f64).sqrt()
}

#[test]
fn independent_passband_stopband_and_impulse_timing() {
    for (from, to) in [
        (48000, 44100),
        (44100, 48000),
        (96000, 48000),
        (192000, 8000),
        (8000, 192000),
    ] {
        let mut r = Resampler::new(1, from, to).unwrap();
        let margin = (to as usize / 10).max(256);
        for proportion in [0.1, 0.4, 0.45] {
            let input = tone(from, from.min(to) as f64 * proportion);
            r.reset();
            let output = convert(&mut r, &input, 511, 4096);
            let gain = 20.0
                * (rms(&output[margin..output.len() - margin]) / (0.5 / 2.0_f64.sqrt())).log10();
            eprintln!("passband {from}->{to} f={proportion} gain_db={gain}");
            assert!(gain.abs() < 0.1, "passband {from}->{to}: {gain} dB");
        }
        if from > to {
            let frequency = (to as f64 * 0.53).min((from + to) as f64 / 4.0);
            r.reset();
            let output = convert(&mut r, &tone(from, frequency), 511, 4096);
            let attenuation = 20.0
                * (rms(&output[margin..output.len() - margin]) / (0.5 / 2.0_f64.sqrt())).log10();
            eprintln!("stopband {from}->{to} freq={frequency} rejection_db={attenuation}");
            assert!(attenuation < -90.0, "alias {from}->{to}: {attenuation} dB");
        }
        let mut impulse = vec![0.0; from as usize];
        impulse[from as usize / 2] = 1.0;
        r.reset();
        let output = convert(&mut r, &impulse, 1024, 4096);
        let peak = output
            .iter()
            .enumerate()
            .max_by(|(_, a), (_, b)| a.abs().total_cmp(&b.abs()))
            .unwrap()
            .0;
        eprintln!("impulse {from}->{to}: peak={peak}, ideal={}", to / 2);
        assert!(peak.abs_diff(to as usize / 2) <= 1);
        let sum: f64 = output.iter().map(|v| *v as f64).sum();
        assert!((sum / (to as f64 / from as f64) - 1.0).abs() < 0.001);
    }
}

#[test]
fn invalid_data_does_not_poison_history_and_eos_is_explicit() {
    for args in [
        (0, 48000, 44100),
        (65, 48000, 44100),
        (1, 0, 44100),
        (1, 48000, 0),
        (1, 1, 48000),
    ] {
        assert!(Resampler::new(args.0, args.1, args.2).is_err());
    }
    let mut r = Resampler::new(2, 48000, 44100).unwrap();
    let mut output = [42.0; 32];
    assert!(r.process(&[f32::NAN, 0.0], &mut output).is_err());
    assert!(r.process(&[1.0], &mut output).is_err());
    assert!(r.process(&[1.0, 0.0], &mut output[..3]).is_err());
    assert!(r.process(&[], &mut []).is_err());
    assert_eq!(output, [42.0; 32]);
    let input = vec![0.2; 514];
    let result = convert(&mut r, &input, 127, 17);
    let mut fresh = Resampler::new(2, 48000, 44100).unwrap();
    assert_eq!(result, convert(&mut fresh, &input, 127, 17));
    assert!(r.process(&[0.0, 0.0], &mut output).is_err());
    assert_eq!(r.finish(&mut output).unwrap(), 0);
}

#[test]
fn compensated_phase_is_nearest_to_the_input_time_origin() {
    for (from, to) in [
        (48000, 44100),
        (44100, 48000),
        (48000, 44101),
        (47999, 48000),
        (96000, 48000),
        (192000, 8000),
        (8000, 192000),
    ] {
        let mut r = Resampler::new(1, from, to).unwrap();
        let frequency = 500.0;
        let output = convert(&mut r, &tone(from, frequency), 127, 193);
        let margin = to as usize / 10;
        let (mut ss, mut cc, mut sc, mut xs, mut xc) = (0.0, 0.0, 0.0, 0.0, 0.0);
        let omega = std::f64::consts::TAU * frequency / to as f64;
        for (i, x) in output
            .iter()
            .enumerate()
            .skip(margin)
            .take(output.len() - 2 * margin)
        {
            let (s, c) = (omega * i as f64).sin_cos();
            ss += s * s;
            cc += c * c;
            sc += s * c;
            xs += *x as f64 * s;
            xc += *x as f64 * c;
        }
        let a = (xs * cc - xc * sc) / (ss * cc - sc * sc);
        let b = (xc * ss - xs * sc) / (ss * cc - sc * sc);
        let shift = b.atan2(a) / omega;
        eprintln!("phase {from}->{to}: {shift} output samples");
        assert!(
            shift.abs() < 0.51,
            "{from}->{to}: phase origin shift={shift} samples"
        );
    }
}
