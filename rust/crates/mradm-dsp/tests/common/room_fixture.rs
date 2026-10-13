//! Synthetic room fixture shared by correctness/allocation probes.
use mradm_dsp::{
    geometry,
    room::{
        hrir::HrirBank,
        prepared::PreparedRoom,
        quality::Quality,
        resources::{HrirData, RoomProfile},
        session::PreparedSet,
    },
};
use std::sync::Arc;

pub fn fixture() -> Arc<PreparedSet> {
    fixture_with_quality(Quality::Exact)
}
pub fn fixture_with_quality(quality: Quality) -> Arc<PreparedSet> {
    let directions = vec![
        [1., 0., 0.],
        [0., 1., 0.],
        [-1., 0., 0.],
        [0., -1., 0.],
        [0., 0., 1.],
        [0., 0., -1.],
    ];
    let faces = geometry::hull(&directions)
        .unwrap()
        .into_iter()
        .map(|v| v.map(|x| x as u32))
        .collect();
    let mut impulses = vec![0.; 6 * 2 * 8];
    for direction in 0..6 {
        impulses[direction * 16 + 2] = 1.;
        impulses[direction * 16 + 8 + 4] = 0.75;
    }
    let hrir = Arc::new(HrirData {
        identity: "synthetic-hrir".into(),
        source_sofa_sha256: None,
        impulses,
        taps: 8,
        directions: directions.clone(),
        faces,
        delays: vec![[2., 4.]; 6],
        guard: 96,
        preprocessing_guard: 0,
        output_taps: 192,
    });
    let mut phases = vec![0.; 1025 * 81];
    for phase in 0..=1024 {
        let mut sum = 0.;
        for i in 0..81 {
            let x = i as f64 - 40. - phase as f64 / 1024.;
            let sinc = if x.abs() < 1e-12 {
                1.
            } else {
                libm::sin(std::f64::consts::PI * x) / (std::f64::consts::PI * x)
            };
            let value = sinc * (0.5 - 0.5 * libm::cos(2. * std::f64::consts::PI * i as f64 / 80.));
            phases[phase * 81 + i] = value as f32;
            sum += value;
        }
        for value in &mut phases[phase * 81..(phase + 1) * 81] {
            *value /= sum as f32;
        }
    }
    let mut late = vec![0.; 6 * 2301];
    for direction in 0..6 {
        late[direction * 2301 + 17 + direction * 11] = 0.1;
        late[direction * 2301 + 2200 + direction * 9] = 0.01;
    }
    let profile = Arc::new(RoomProfile {
        identity: "synthetic-room".into(),
        id: "test".into(),
        name: "test".into(),
        default_hrir: "hrir.json".into(),
        dimensions: [7., 6., 3.2],
        listener: [3.4, 3., 1.2],
        speed: 344.02,
        reference_distance: 2.1,
        minimum_distance: 0.25,
        clearance: 0.15,
        reflection_order: 4,
        absorption: [[0.94; 8]; 6],
        early_fade: [0.015, 0.025],
        late_predelay: 0.018,
        late_energy: 0.25,
        alignment_guard: 40,
        band_offset: 0,
        band_taps: 1,
        bands: vec![0.125; 8],
        late_taps: 2301,
        late,
        late_directions: directions,
        eq: vec![1., 0., 0., 0.05],
        propagation_phases: phases,
    });
    let (length, _, _) = PreparedRoom::required_fft(&profile, &hrir).unwrap();
    let bank = Arc::new(HrirBank::with_policy(hrir, length, quality.fft()).unwrap());
    Arc::new(
        PreparedSet::new(Arc::new(
            PreparedRoom::with_quality(profile, bank, quality).unwrap(),
        ))
        .unwrap(),
    )
}
