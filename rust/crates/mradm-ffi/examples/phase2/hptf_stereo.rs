use mradm_dsp::hptf::{Band, BandKind, PreampMode, Processor, StereoSnapshot, design_stereo};
use std::path::Path;

pub fn run(out: &Path) {
    for rate in [44100, 48000, 96000] {
        let left = [Band {
            kind: BandKind::LowShelf,
            enabled: true,
            frequency: 105.0,
            gain_db: 9.0,
            q: 0.7,
        }];
        let right = [Band {
            kind: BandKind::Peaking,
            enabled: true,
            frequency: 1700.0,
            gain_db: 6.0,
            q: 0.75,
        }];
        let coefficients =
            design_stereo([(&left, -3.0), (&right, -2.0)], rate, PreampMode::AutoTrim).unwrap();
        for (ear, c) in coefficients.into_iter().enumerate() {
            let mut values = vec![
                c.preamp_gain,
                c.preamp_db,
                c.auto_trim_db,
                c.max_response_db,
            ];
            for s in &c.sections[..c.band_count] {
                values.extend([s.b0, s.b1, s.b2, s.a1, s.a2]);
            }
            super::floats(
                out,
                &format!(
                    "hptf-stereo-{rate}.{}",
                    if ear == 0 {
                        "20-left.f32"
                    } else {
                        "21-right.f32"
                    }
                ),
                &values,
            );
        }
        let input = super::signal(16384);
        super::floats(out, &format!("hptf-stereo-{rate}.10-input.f32"), &input);
        let mut captures = Vec::new();
        let mut revisions = Vec::new();
        for (name, sizes) in [
            ("30-fixed", &[512][..]),
            ("31-fragmented", &[1, 137, 2049, 31][..]),
        ] {
            let mut p = Processor::new(2, rate).unwrap();
            let mut pcm = input.clone();
            let events = [0, 127, 2047, 4096, 6144];
            let mut position = 0;
            let mut packet = 0;
            let mut event = 0;
            while position < pcm.len() / 2 {
                let target = if events.get(event) == Some(&position) {
                    let mut target = StereoSnapshot {
                        coefficients,
                        revision: event as u64 + 1,
                        independent: true,
                    };
                    if event == 1 || event == 2 {
                        target.coefficients[0] = design_stereo(
                            [(&[], -9.0), (&right, -2.0)],
                            rate,
                            PreampMode::WarnOnly,
                        )
                        .unwrap()[0];
                    }
                    if event == 4 {
                        target.coefficients =
                            design_stereo([(&[], 0.0); 2], rate, PreampMode::WarnOnly).unwrap();
                    }
                    event += 1;
                    Some(target)
                } else {
                    None
                };
                let end = (position + sizes[packet % sizes.len()])
                    .min(pcm.len() / 2)
                    .min(events.get(event).copied().unwrap_or(usize::MAX));
                p.process_stereo(&mut pcm[position * 2..end * 2], target)
                    .unwrap();
                position = end;
                packet += 1;
            }
            revisions.push(p.stereo_update(false).snapshot.revision as i32);
            super::floats(out, &format!("hptf-stereo-{rate}.{name}.f32"), &pcm);
            captures.push(pcm);
        }
        assert_eq!(captures[0], captures[1]);
        super::integers(
            out,
            &format!("hptf-stereo-{rate}.40-revisions.i32"),
            &revisions,
        );
    }
}
