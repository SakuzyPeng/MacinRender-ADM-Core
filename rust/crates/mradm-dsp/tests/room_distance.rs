use mradm_dsp::{
    Error,
    room::{
        collection,
        control::{RoomCommand, RoomDescription, State},
        geometry::{self, Rotation},
        hrir::{QueryCache, TimeQuery},
        prepared::ResponseWork,
        quality::Quality,
        resources::sha256,
        session::Session,
    },
};
use std::path::Path;
#[path = "common/room_fixture.rs"]
mod common;

fn load(
    index: &serde_json::Value,
    replacement: Option<&str>,
) -> mradm_dsp::Result<mradm_dsp::room::resources::HrirData> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/fixtures/sofa/distance");
    collection::prepare(&serde_json::to_vec(index).unwrap(), |name, _| {
        std::fs::read(root.join(replacement.unwrap_or(name)))
            .map_err(|_| Error::Io("missing fixture"))
    })
}
fn index() -> serde_json::Value {
    serde_json::from_slice(include_bytes!(
        "../../../../tests/fixtures/sofa/distance/distance_index.json"
    ))
    .unwrap()
}

#[test]
fn authenticated_collection_rejects_missing_tampered_or_incompatible_layers() {
    let full = mradm_dsp::room::sofa::prepare(include_bytes!(
        "../../../../tests/fixtures/sofa/distance/full-heap.sofa"
    ))
    .unwrap();
    let far = mradm_dsp::room::sofa::prepare(include_bytes!(
        "../../../../tests/fixtures/sofa/distance/far.sofa"
    ))
    .unwrap();
    assert_eq!(full.impulses, far.impulses);
    assert_eq!(full.directions, far.directions);
    let original = index();
    let data = load(&original, None).unwrap();
    assert_eq!(data.collection.as_ref().unwrap().layers.len(), 2);
    assert_eq!(data.source_geometry.as_ref().unwrap().radius, 1.5);
    for field in ["bytes", "sha256", "path"] {
        let mut bad = original.clone();
        bad["layers"][0]["files"]["HRIR_DFEQ100"][field] = match field {
            "bytes" => 1.into(),
            "path" => "missing.sofa".into(),
            _ => "0".repeat(64).into(),
        };
        if field == "path" {
            bad["layers"][0]["primary"] = "missing.sofa".into();
        }
        assert!(load(&bad, None).is_err(), "{field}");
    }
    let mut bad = original.clone();
    bad["layers"][0]["radius_m"] = 0.3.into();
    bad["radii_m"][0] = 0.3.into();
    assert!(load(&bad, None).is_err());
    for name in ["bad-grid.sofa", "bad-ears.sofa"] {
        let bytes = std::fs::read(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../../tests/fixtures/sofa/distance")
                .join(name),
        )
        .unwrap();
        let mut bad = original.clone();
        bad["layers"][0]["primary"] = name.into();
        bad["layers"][0]["files"]["HRIR_DFEQ100"] =
            serde_json::json!({"path":name,"bytes":bytes.len(),"sha256":sha256(&bytes)});
        assert!(load(&bad, None).is_err(), "{name}");
    }
    for name in ["../file.sofa", "/file.sofa", "C:/file.sofa", "a\\b.sofa"] {
        assert!(collection::relative_path(name).is_err());
    }
}

#[test]
fn radial_interpolation_preserves_gain_itd_endpoints_and_cache_identity() {
    let prepared = common::fixture_with_collection(Quality::Exact);
    let bank = &prepared.room.hrir;
    let mut query = TimeQuery::new(bank.clone()).unwrap();
    let mut output = vec![0.; bank.data.output_taps * 2];
    for (radius, gain, delay) in [
        (0., 2., 6),
        (0.2, 2., 6),
        (0.85, 1.5, 4),
        (1.5, 1., 2),
        (3., 1., 2),
    ] {
        for direction in &bank.data.directions {
            query
                .query_distance(*direction, Some(radius), &mut { usize::MAX }, &mut output)
                .unwrap();
            for ear in 0..2 {
                for i in 0..bank.data.output_taps {
                    let expected = if i == bank.data.guard + delay + ear * 2 {
                        gain * if ear == 0 { 1. } else { 0.75 }
                    } else {
                        0.
                    };
                    assert!(
                        (output[ear * bank.data.output_taps + i] - expected).abs() < 2e-5,
                        "r={radius},ear={ear},sample={i}"
                    );
                }
            }
        }
    }
    let mut cache = QueryCache::new(bank, 4);
    let mut spectrum = vec![0.; bank.stride() * 4];
    cache
        .query(
            bank,
            0,
            [1., 0., 0.],
            Some(0.2),
            &mut { usize::MAX },
            &mut spectrum,
        )
        .unwrap();
    let near = spectrum.clone();
    cache
        .query(
            bank,
            0,
            [1., 0., 0.],
            Some(1.5),
            &mut { usize::MAX },
            &mut spectrum,
        )
        .unwrap();
    assert_ne!(near, spectrum);
    // The outer-layer query is already cached. A second physical distance
    // beyond it must hit the same entry and leave the caller's face hint alone.
    let outer = spectrum.clone();
    let mut hint = usize::MAX;
    cache
        .query(bank, 0, [1., 0., 0.], Some(4.), &mut hint, &mut spectrum)
        .unwrap();
    assert_eq!(hint, usize::MAX);
    assert_eq!(outer, spectrum);
    assert!(
        cache
            .query(
                bank,
                0,
                [1., 0., 0.],
                Some(f64::from_bits(u64::MAX)),
                &mut hint,
                &mut spectrum
            )
            .is_err()
    );
    let single = common::fixture();
    cache
        .query(
            &single.room.hrir,
            0,
            [1., 0., 0.],
            Some(0.2),
            &mut { usize::MAX },
            &mut spectrum,
        )
        .unwrap();
    let mut expected = spectrum.clone();
    single
        .room
        .hrir
        .query([1., 0., 0.], &mut { usize::MAX }, &mut expected)
        .unwrap();
    assert_eq!(spectrum, expected);
    for boundary in [0.2, 1.5] {
        query
            .query_distance(
                [1., 1., 0.],
                Some(boundary - 1e-8),
                &mut { usize::MAX },
                &mut output,
            )
            .unwrap();
        let left = output.clone();
        query
            .query_distance(
                [1., 1., 0.],
                Some(boundary + 1e-8),
                &mut { usize::MAX },
                &mut output,
            )
            .unwrap();
        assert!(left.iter().zip(&output).all(|(a, b)| (a - b).abs() < 2e-5));
    }
    assert!(
        query
            .query_distance(
                [1., 0., 0.],
                Some(f64::NAN),
                &mut { usize::MAX },
                &mut output
            )
            .is_err()
    );
}

#[test]
fn direct_propagation_gain_and_time_are_applied_once() {
    let collection = common::fixture_with_collection(Quality::Exact);
    let fixed = common::fixture();
    let mut work = ResponseWork::new(&collection.room).unwrap();
    let mut reference = ResponseWork::new(&fixed.room).unwrap();
    let radius = 0.85;
    let position =
        geometry::position(&collection.room.profile, [0., radius, 0.], 1., [1., 0., 0.]).unwrap();
    let rotation = Rotation::new([0.; 3]).unwrap();
    work.build(&collection.room, position, &rotation).unwrap();
    reference.build(&fixed.room, position, &rotation).unwrap();
    let taps = collection.room.short_taps;
    // This fixture's reflection paths exceed the outer (reference) layer.
    assert_eq!(&work.impulse[taps * 2..], &reference.impulse[taps * 2..]);
    assert_ne!(&work.impulse[..taps * 2], &reference.impulse[..taps * 2]);
    let arrival = position.distance / collection.room.profile.speed * 48000.;
    let integer = arrival.floor() as usize;
    let mut kernel = [0.; 81];
    collection.room.propagation_kernel(
        f64::from((arrival - integer as f64) as f32),
        false,
        &mut kernel,
    );
    let gain = (collection.room.profile.reference_distance / position.distance) as f32 * 1.5;
    let begin = integer + collection.room.profile.alignment_guard + 96 + 4;
    let mut error = 0f64;
    let mut energy = 0f64;
    for i in 0..taps {
        let expected = if (begin..begin + 81).contains(&i) {
            kernel[i - begin] * gain
        } else {
            0.
        };
        error += f64::from(work.impulse[i] - expected).powi(2);
        energy += f64::from(expected).powi(2);
    }
    assert!((error / energy).sqrt() < 2e-5);
}

#[test]
fn radial_scene_motion_head_lock_extent_and_reset_are_packet_independent() {
    for rate in [48000, 96000] {
        for quality in [Quality::Exact, Quality::Realtime] {
            let prepared = common::fixture_at_rate(quality, rate);
            for head in [0, 1] {
                let render = |chunks: &[usize]| {
                    let mut session = Session::new(prepared.clone(), 1., true, true).unwrap();
                    session
                        .configure(&[RoomDescription {
                            id: 7,
                            position: [-1.5, 0., 0.],
                            ..Default::default()
                        }])
                        .unwrap();
                    let initial = RoomCommand {
                        state: State {
                            valid: 4 | 8 | 32 | 256,
                            position: [-1.5, 0., 0.],
                            head_locked: head,
                            extent: [0.04, 0.02, 0.03],
                            divergence: 0.15,
                            divergence_range: [15., 0.],
                            ..Default::default()
                        },
                        ..Default::default()
                    };
                    let event = RoomCommand {
                        duration: 5000,
                        changed: 4,
                        state: State {
                            position: [-0.2, 0., 0.],
                            ..Default::default()
                        },
                        ..Default::default()
                    };
                    let pcm: Vec<_> = (0..7000)
                        .map(|i| ((i * 31 % 127) as f32 - 63.) / 4096.)
                        .collect();
                    let mut first = Vec::new();
                    for pass in 0..2 {
                        session.reset(0).unwrap();
                        session.set_pose([19., 3., 0.]).unwrap();
                        let mut at = 0;
                        let mut sequence = 0;
                        let mut result = Vec::new();
                        while at < pcm.len() {
                            let n = chunks[sequence % chunks.len()].min(pcm.len() - at);
                            let mut out = vec![0.; n * 2];
                            session
                                .process(
                                    n,
                                    [Some(&pcm[at..at + n])].into_iter(),
                                    if at == 0 {
                                        std::slice::from_ref(&initial)
                                    } else {
                                        &[]
                                    },
                                    if at == 0 {
                                        std::slice::from_ref(&event)
                                    } else {
                                        &[]
                                    },
                                    &mut out,
                                )
                                .unwrap();
                            result.extend(out);
                            at += n;
                            sequence += 1;
                        }
                        if pass == 0 {
                            first = result;
                        } else {
                            assert_eq!(first, result);
                        }
                    }
                    first
                };
                assert_eq!(render(&[256]), render(&[1, 173, 777, 19, 512]));
            }
        }
    }
}
