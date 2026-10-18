use mradm_dsp::{
    Error,
    room::{
        collection, geometry,
        hrir::{HrirBank, TimeQuery},
        partition::Dynamic,
        quality::Quality,
        session::{Instance, Session},
        sofa,
    },
};
use std::{path::Path, sync::Arc};
#[allow(dead_code)]
#[path = "common/room_fixture.rs"]
mod common;

fn index() -> serde_json::Value {
    serde_json::from_slice(include_bytes!(
        "../../../../tests/fixtures/sofa/distance-96/distance_index.json"
    ))
    .unwrap()
}
fn load(
    value: &serde_json::Value,
    rate: u32,
) -> mradm_dsp::Result<mradm_dsp::room::resources::HrirData> {
    let root =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/fixtures/sofa/distance-96");
    collection::prepare_at_rate(&serde_json::to_vec(value).unwrap(), rate, |name, _| {
        std::fs::read(root.join(name)).map_err(|_| Error::Io("fixture"))
    })
}

#[test]
fn native_bank_retains_samples_high_band_and_authenticated_geometry() {
    let data = Arc::new(load(&index(), 96000).unwrap());
    let original = sofa::prepare(include_bytes!(
        "../../../../tests/fixtures/sofa/distance/far.sofa"
    ))
    .unwrap();
    assert_eq!(data.impulses, original.impulses);
    assert_eq!(data.taps, original.taps);
    assert_eq!(data.preprocessing_guard, 0);
    assert_eq!(data.sample_rate, 96000);
    assert_eq!(data.source_geometry.as_ref().unwrap().sample_rate, 96000);
    assert_eq!(data.collection.as_ref().unwrap().reference, 1);
    assert_eq!(
        data.collection.as_ref().unwrap().layers[0].quality["low_band"]["2"],
        1
    );
    let converted = load(&index(), 48000).unwrap();
    assert_ne!(converted.identity, data.identity);
    assert_eq!(converted.sample_rate, 48000);
    assert_eq!(converted.preprocessing_guard, 10);
    let bank = Arc::new(HrirBank::new(data.clone(), data.output_taps.next_power_of_two()).unwrap());
    let mut query = TimeQuery::new(bank).unwrap();
    let mut out = vec![0.; data.output_taps * 2];
    query
        .query_distance(data.directions[0], Some(1.5), &mut { usize::MAX }, &mut out)
        .unwrap();
    for hz in [30000., 40000.] {
        for ear in 0..2 {
            let magnitude = |row: &[f32]| {
                let (mut re, mut im) = (0., 0.);
                for (i, v) in row.iter().enumerate() {
                    let (s, c) = (std::f64::consts::TAU * hz * i as f64 / 96000.).sin_cos();
                    re += f64::from(*v) * c;
                    im += f64::from(*v) * s;
                }
                re.hypot(im)
            };
            let expected = magnitude(&data.impulses[ear * data.taps..(ear + 1) * data.taps]);
            assert!(expected >= 0.125);
            assert!(
                (magnitude(&out[ear * data.output_taps..(ear + 1) * data.output_taps]) / expected
                    - 1.)
                    .abs()
                    < 2e-5
            );
        }
    }
    for field in ["sampling_rate_hz", "reference", "sha256"] {
        let mut bad = index();
        match field {
            "sampling_rate_hz" => bad[field] = 48000.into(),
            "reference" => {
                bad["layers"].as_array_mut().unwrap().pop();
                bad["radii_m"].as_array_mut().unwrap().pop();
            }
            _ => bad["layers"][0]["files"]["HRIR_DFEQ100"][field] = "0".repeat(64).into(),
        }
        assert!(load(&bad, 96000).is_err(), "{field}");
    }
}

#[test]
fn clocks_propagation_and_room_switches_are_session_local() {
    for quality in [Quality::Exact, Quality::Realtime] {
        let a = common::fixture_at_rate(quality, 48000);
        let b = common::fixture_at_rate(quality, 96000);
        let position = geometry::position(&a.room.profile, [0., 1., 0.], 1., [1., 0., 0.]).unwrap();
        let image = geometry::images(1)[0];
        let pa = geometry::path(&a.room.profile, position, image);
        let pb = geometry::path(&b.room.profile, position, image);
        assert_eq!(pb.samples, pa.samples * 2.);
        assert_eq!(pb.gain, pa.gain);
        assert_eq!(
            quality.reflection_frames_at(96000),
            quality.reflection_frames() * 2
        );
        let session = Session::new(b, 1.5, true, false).unwrap();
        assert!(
            session
                .validate_room(&Instance::new(a, 0).unwrap(), 1)
                .is_err()
        );
    }
}

#[test]
fn filter_fade_and_diffuse_keep_physical_duration_at_96k() {
    use mradm_dsp::{diffuse::DiffuseDelay, fft::FftPolicy};
    for rate in [48000, 96000] {
        let fade = rate as usize / 100;
        let mut filter = Dynamic::with_rate(1, 1, FftPolicy::Exact, rate).unwrap();
        filter.set_target(&[1.]).unwrap();
        let mut output = vec![0.; 256];
        filter.process(&[1.; 256], &mut output).unwrap();
        filter.set_target(&[0.]).unwrap();
        let mut ramp = Vec::new();
        for _ in 0..fade.div_ceil(256) + 1 {
            filter.process(&[1.; 256], &mut output).unwrap();
            ramp.extend_from_slice(&output);
        }
        for (i, &value) in ramp.iter().enumerate() {
            assert!((value - (1. - i as f32 / fade as f32).max(0.)).abs() < 2e-6);
        }
        let mut delay = DiffuseDelay::with_sample_rate(rate).unwrap();
        let mut input = vec![0.; 80];
        input[0] = 1.;
        let mut out = input.clone();
        delay.process(&input, &mut out).unwrap();
        let hits: Vec<_> = out
            .iter()
            .enumerate()
            .filter(|(_, v)| **v != 0.)
            .map(|(i, _)| i)
            .collect();
        assert_eq!(
            hits,
            [3, 7, 11, 17, 19, 23, 29, 31].map(|n| n * (rate / 48000) as usize)
        );
        delay.reset();
        delay.process(&input, &mut out).unwrap();
        assert_eq!(
            out.iter().rposition(|v| *v != 0.).unwrap(),
            31 * (rate / 48000) as usize
        );
    }
}
