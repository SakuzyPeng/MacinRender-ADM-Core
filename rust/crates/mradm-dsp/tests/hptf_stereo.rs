use mradm_dsp::hptf::{
    BLEND_FRAMES, Band, BandKind, Cascade, Coefficients, PreampMode, Processor, Snapshot,
    StereoSnapshot, design, design_stereo,
};

fn band(frequency: f64, gain_db: f64) -> Band {
    Band {
        kind: BandKind::Peaking,
        enabled: true,
        frequency,
        gain_db,
        q: 1.7,
    }
}
fn pair(revision: u64) -> StereoSnapshot {
    StereoSnapshot {
        coefficients: design_stereo(
            [
                (&[band(61.0 + revision as f64, 6.0)], -3.0),
                (&[band(2400.0, -4.0), band(113.0, 3.0)], -2.0),
            ],
            48000,
            PreampMode::WarnOnly,
        )
        .unwrap(),
        revision,
        independent: true,
    }
}
fn source(frames: usize) -> Vec<f32> {
    let mut seed = 17_u32;
    (0..frames * 2)
        .map(|_| {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            (seed >> 8) as f32 / 16777216.0 - 0.5
        })
        .collect()
}
fn bits(samples: &[f32]) -> Vec<u32> {
    samples.iter().map(|x| x.to_bits()).collect()
}

#[test]
fn common_trim_preserves_balance_and_attenuates_the_empty_ear() {
    for rate in [32000, 44100, 48000, 96000, 192000] {
        let bands = [band(1093.17, 24.0)];
        let warn = design_stereo([(&bands, -3.0), (&[], 0.0)], rate, PreampMode::WarnOnly).unwrap();
        let trim = design_stereo([(&bands, -3.0), (&[], 0.0)], rate, PreampMode::AutoTrim).unwrap();
        for c in trim {
            assert!(c.max_response_db <= 0.0);
            assert!(c.auto_trim_db < -20.0);
        }
        assert_eq!(trim[1].band_count, 0);
        assert!(!trim[1].is_bypass());
        assert!((trim[0].auto_trim_db - trim[1].auto_trim_db).abs() < 4e-6);
        let before = warn[0].preamp_gain as f64 / warn[1].preamp_gain as f64;
        let after = trim[0].preamp_gain as f64 / trim[1].preamp_gain as f64;
        assert!((before - after).abs() < 2e-7);
        for mode in [PreampMode::WarnOnly, PreampMode::AutoTrim] {
            let shared = design(&bands, -3.0, rate, mode).unwrap();
            assert_eq!(
                design_stereo([(&bands, -3.0); 2], rate, mode).unwrap(),
                [shared; 2]
            );
        }
    }
    for side in 0..2 {
        let bad = [band(0.0, 1.0)];
        let mut request: [(&[Band], f64); 2] = [(&[], 0.0); 2];
        request[side] = (&bad, 0.0);
        assert_eq!(
            design_stereo(request, 48000, PreampMode::WarnOnly)
                .unwrap_err()
                .0,
            side
        );
    }
}

#[test]
fn independent_filters_match_two_mono_oracles_including_32_bands() {
    let bands: Vec<_> = (0..32)
        .map(|n| band(100.0 + n as f64 * 330.0, -0.3))
        .collect();
    let coeffs = design_stereo([(&bands, -3.0), (&[], -9.0)], 48000, PreampMode::WarnOnly).unwrap();
    let input = source(6000);
    let mut expected = input.clone();
    for ear in 0..2 {
        let mut mono = Cascade::new(1).unwrap();
        mono.set_coefficients(coeffs[ear]).unwrap();
        let mut samples: Vec<_> = input.iter().skip(ear).step_by(2).copied().collect();
        mono.process(&mut samples).unwrap();
        for (frame, value) in samples.into_iter().enumerate() {
            expected[frame * 2 + ear] = value;
        }
    }
    for chunk in [1, 17, 511, 2049, 6000] {
        let mut p = Processor::new(2, 48000).unwrap();
        p.reset_stereo(Some(StereoSnapshot {
            coefficients: coeffs,
            revision: 4,
            independent: true,
        }))
        .unwrap();
        let mut actual = input.clone();
        for samples in actual.chunks_mut(chunk * 2) {
            p.process_stereo(samples, None).unwrap();
        }
        assert_eq!(bits(&actual), bits(&expected));
    }
}

#[test]
fn one_ear_edit_preserves_the_other_history_and_pcm() {
    for chunk in [1, 13, 512, 2049] {
        let mut changed = Processor::new(2, 48000).unwrap();
        let mut reference = Processor::new(2, 48000).unwrap();
        for p in [&mut changed, &mut reference] {
            p.reset_stereo(Some(pair(1))).unwrap();
        }
        let mut warm = source(1337);
        reference.process_stereo(&mut warm.clone(), None).unwrap();
        changed.process_stereo(&mut warm, None).unwrap();
        let mut a = source(9000);
        let mut b = a.clone();
        let mut target = Some(pair(2));
        for (a, b) in a.chunks_mut(chunk * 2).zip(b.chunks_mut(chunk * 2)) {
            changed.process_stereo(a, target.take()).unwrap();
            reference.process_stereo(b, None).unwrap();
        }
        assert_eq!(
            a.iter()
                .skip(1)
                .step_by(2)
                .map(|x| x.to_bits())
                .collect::<Vec<_>>(),
            b.iter()
                .skip(1)
                .step_by(2)
                .map(|x| x.to_bits())
                .collect::<Vec<_>>()
        );
        assert_ne!(bits(&a), bits(&b));
        assert_eq!(changed.stereo_update(false).snapshot.revision, 2);
    }
}

fn replay(partition: &[usize]) -> (Vec<u32>, Vec<(usize, u64)>) {
    let mut processor = Processor::new(2, 48000).unwrap();
    let mut pcm = source(10000);
    let events = [(0, 1), (50, 2), (333, 3), (2047, 4), (2500, 5), (4300, 6)];
    let mut next = 0;
    let mut position = 0;
    let mut block = 0;
    let mut statuses = Vec::new();
    while position < pcm.len() / 2 {
        let target = if next < events.len() && events[next].0 == position {
            let value = pair(events[next].1);
            next += 1;
            Some(value)
        } else {
            None
        };
        let end = (position + partition[block % partition.len()])
            .min(pcm.len() / 2)
            .min(events.get(next).map_or(usize::MAX, |e| e.0));
        processor
            .process_stereo(&mut pcm[position * 2..end * 2], target)
            .unwrap();
        position = end;
        block += 1;
        if events.iter().any(|e| e.0 == position) || position == 10000 {
            statuses.push((position, processor.stereo_update(false).snapshot.revision));
        }
    }
    (bits(&pcm), statuses)
}

#[test]
fn pair_queue_and_pcm_are_partition_invariant() {
    let expected = replay(&[1]);
    for sizes in [&[17][..], &[512], &[4097], &[13, 1024, 7, 2049]] {
        assert_eq!(replay(sizes), expected);
    }
}

#[test]
fn reset_errors_bypass_and_shared_interoperation() {
    let mut p = Processor::new(2, 48000).unwrap();
    let mut samples = source(100);
    p.process_stereo(&mut samples, Some(pair(1))).unwrap();
    p.process_stereo(&mut samples, Some(pair(2))).unwrap();
    assert_eq!(p.reset_stereo(None).unwrap().snapshot, pair(2));
    p.process_stereo(&mut samples, Some(pair(3))).unwrap();
    p.process_stereo(&mut samples, Some(pair(4))).unwrap();
    assert_eq!(p.reset_stereo(Some(pair(5))).unwrap().snapshot, pair(5));
    let saved = p.stereo_update(false);
    let mut bad = pair(9);
    bad.coefficients[1].sample_rate = 96000;
    let input = samples.clone();
    assert!(p.process_stereo(&mut samples, Some(bad)).is_err());
    assert_eq!(bits(&samples), bits(&input));
    assert_eq!(p.stereo_update(false), saved);
    assert!(p.reset_stereo(Some(bad)).is_err());
    let bypass = Coefficients {
        sample_rate: 48000,
        ..Coefficients::default()
    };
    p.reset_stereo(Some(StereoSnapshot {
        coefficients: [bypass; 2],
        revision: 10,
        independent: true,
    }))
    .unwrap();
    let mut exact = [0.0, -0.0, f32::from_bits(1), -0.3];
    let before = bits(&exact);
    p.process_stereo(&mut exact, None).unwrap();
    assert_eq!(bits(&exact), before);
    let shared = Snapshot {
        coefficients: pair(1).coefficients[0],
        revision: 11,
    };
    p.process(&mut vec![0.1; BLEND_FRAMES * 2], Some(shared))
        .unwrap();
    assert_eq!(p.update(false).snapshot, shared);
    assert!(!p.stereo_update(false).snapshot.independent);
}
