use mradm_ear::{Calculator, DirectSpeakers, Normalization, Object, decorrelate, standard_layout};
fn near(a: f64, b: f64, t: f64) {
    assert!((a - b).abs() <= t, "{a} != {b}");
}
#[test]
fn routing_power_diffuse_and_symmetry() {
    for name in ["0+5+0", "2+5+0", "4+5+0", "0+7+0", "4+7+0", "9+10+3"] {
        let layout = standard_layout(name).unwrap();
        let mut calc = Calculator::new(layout.clone()).unwrap();
        for (idx, ch) in layout.channels.iter().enumerate().filter(|(_, c)| !c.lfe) {
            let (d, f) = calc
                .objects(Object {
                    position: ch.real,
                    ..Object::default()
                })
                .unwrap();
            for i in 0..d.len() {
                near(d[i], if i == idx { 1.0 } else { 0.0 }, 1e-9);
                assert_eq!(f[i], 0.0);
            }
        }
        for (w, h) in [(0.0, 0.0), (30.0, 60.0), (120.0, 90.0), (360.0, 360.0)] {
            for distance in [0.0, 0.1, 1.0, 2.0] {
                let (d, f) = calc
                    .objects(Object {
                        position: [23.0, 17.0, distance],
                        width: w,
                        height: h,
                        depth: 0.0,
                        gain: 0.7,
                        diffuse: 0.3,
                    })
                    .unwrap();
                near(d.iter().chain(&f).map(|g| g * g).sum(), 0.49, 2e-6);
                for (i, c) in layout.channels.iter().enumerate() {
                    assert!(d[i].is_finite() && f[i].is_finite());
                    if c.lfe {
                        assert_eq!(d[i], 0.0);
                        assert_eq!(f[i], 0.0);
                    }
                }
            }
        }
        let (left, _) = calc
            .objects(Object {
                position: [47.0, 19.0, 1.0],
                ..Object::default()
            })
            .unwrap();
        let (right, _) = calc
            .objects(Object {
                position: [-47.0, 19.0, 1.0],
                ..Object::default()
            })
            .unwrap();
        for (i, c) in layout.channels.iter().enumerate().filter(|(_, c)| !c.lfe) {
            if let Some(j) = layout
                .channels
                .iter()
                .position(|v| v.real[0] == -c.real[0] && v.real[1] == c.real[1] && !v.lfe)
            {
                near(left[i], right[j], 1e-9);
            }
        }
    }
}
#[test]
fn direct_labels_bounds_and_lfe() {
    let layout = standard_layout("0+5+0").unwrap();
    let calc = Calculator::new(layout).unwrap();
    for label in [
        "M+030",
        "urn:itu:bs:2051:0:speaker:M+030",
        "urn:itu:bs:2051:12:speaker:M+030",
    ] {
        assert_eq!(
            calc.direct_speakers(&DirectSpeakers {
                labels: vec![label.into()],
                ..Default::default()
            })
            .unwrap(),
            vec![1.0, 0.0, 0.0, 0.0, 0.0, 0.0]
        );
    }
    let g = calc
        .direct_speakers(&DirectSpeakers {
            labels: vec!["unknown".into(), "M-030".into()],
            ..Default::default()
        })
        .unwrap();
    assert_eq!(g[1], 1.0);
    let g = calc
        .direct_speakers(&DirectSpeakers {
            low_pass: Some(120.0),
            ..Default::default()
        })
        .unwrap();
    assert_eq!(g[3], 1.0);
    assert_eq!(g.iter().sum::<f64>(), 1.0);
    let g = calc
        .direct_speakers(&DirectSpeakers {
            position: [28.0, 0.0, 1.0],
            bounds: [Some(20.0), Some(40.0), None, None, None, None],
            ..Default::default()
        })
        .unwrap();
    assert_eq!(g[0], 1.0);
    let g = calc
        .direct_speakers(&DirectSpeakers {
            position: [15.0, 0.0, 1.0],
            ..Default::default()
        })
        .unwrap();
    near(g.iter().map(|v| v * v).sum(), 1.0, 1e-12);
    assert!(g[0] > 0.0 && g[2] > 0.0);
}
#[test]
fn hoa_normalization_and_invalid_inputs() {
    let mut calc = Calculator::new(standard_layout("4+7+0").unwrap()).unwrap();
    let w = calc.hoa(&[0], &[0], Normalization::Sn3d).unwrap();
    near(w.iter().map(|v| v * v).sum(), 1.0, 1e-10);
    assert_eq!(w[3], 0.0);
    let fuma = calc.hoa(&[0], &[0], Normalization::Fuma).unwrap();
    for (a, b) in w.iter().zip(fuma) {
        near(*a * 2.0f64.sqrt(), b, 1e-12);
    }
    let n = [0, 1, 2, 3, 6];
    let m = [0, -1, 2, -3, 4];
    let sn = calc.hoa(&n, &m, Normalization::Sn3d).unwrap();
    let nn = calc.hoa(&n, &m, Normalization::N3d).unwrap();
    for i in 0..n.len() {
        for ch in 0..12 {
            near(
                sn[i * 12 + ch],
                nn[i * 12 + ch] * ((2 * n[i] + 1) as f64).sqrt(),
                1e-12,
            );
        }
    }
    assert!(calc.hoa(&[1], &[2], Normalization::Sn3d).is_err());
    assert!(calc.hoa(&[4], &[0], Normalization::Fuma).is_err());
    assert!(
        calc.objects(Object {
            diffuse: f64::NAN,
            ..Default::default()
        })
        .is_err()
    );
    let mut invalid = standard_layout("0+5+0").unwrap();
    invalid.channels[1].name = invalid.channels[0].name.clone();
    assert!(Calculator::new(invalid).is_err());
}
#[test]
fn fir_frequency_magnitude_and_name_order() {
    let layout = standard_layout("0+5+0").unwrap();
    let a = decorrelate::filters(&layout);
    for taps in a.as_chunks::<512>().0.iter() {
        for k in [0, 1, 7, 63, 128, 255, 256] {
            let mut re = 0.0;
            let mut im = 0.0;
            for (i, &v) in taps.iter().enumerate() {
                let phase = std::f64::consts::TAU * (i * k) as f64 / 512.0;
                re += v as f64 * phase.cos();
                im += v as f64 * phase.sin();
            }
            near(re.hypot(im), 1.0, 2e-7);
        }
    }
    let mut reversed = layout;
    reversed.channels.reverse();
    let b = decorrelate::filters(&reversed);
    for (x, y) in a
        .as_chunks::<512>()
        .0
        .iter()
        .zip(b.as_chunks::<512>().0.iter().rev())
    {
        assert_eq!(x, y);
    }
    assert_eq!(decorrelate::DELAY, 255);
}
