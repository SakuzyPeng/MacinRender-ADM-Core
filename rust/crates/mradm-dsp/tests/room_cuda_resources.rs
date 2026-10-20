#![cfg(feature = "room-gpu-experiment")]
use mradm_dsp::room::{hrir::PHASES, quality::Quality, resources::sha256};
#[path = "common/room_fixture.rs"]
#[allow(dead_code)]
mod common;

#[test]
fn resident_upload_windows_preserve_all_prepared_spectral_bits() {
    for rate in [48000, 96000] {
        let prepared = common::fixture_at_rate(Quality::Realtime, rate);
        let bank = &prepared.room.hrir;
        let bins = bank.fft_length / 2 + 1;
        let rows = bank.data.directions.len() * 2;
        let fingerprint = bank.fingerprint();
        let layers = bank.data.collection.as_ref().unwrap().layers.len();
        for layer in 0..layers {
            let mut values = vec![0.; rows * bins * 2];
            for first in (0..rows).step_by(5) {
                let count = 5.min(rows - first);
                bank.gpu_copy_rows(
                    layer * rows + first,
                    &mut values[first * bins * 2..(first + count) * bins * 2],
                )
                .unwrap();
            }
            let bytes: Vec<_> = values.iter().flat_map(|v| v.to_le_bytes()).collect();
            assert_eq!(
                sha256(&bytes),
                fingerprint["distance_layers"]["spectra"][layer]
                    .as_str()
                    .unwrap()
            );
        }
        let mut values = vec![0.; (PHASES + 1) * bins * 2];
        for first in (0..=PHASES).step_by(7) {
            let count = 7.min(PHASES + 1 - first);
            bank.gpu_copy_fractions(
                first,
                &mut values[first * bins * 2..(first + count) * bins * 2],
            )
            .unwrap();
        }
        let bytes: Vec<_> = values.iter().flat_map(|v| v.to_le_bytes()).collect();
        assert_eq!(sha256(&bytes), fingerprint["fractional"].as_str().unwrap());
        assert!(
            bank.gpu_copy_rows(layers * rows, &mut values[..bins * 2])
                .is_err()
        );
        assert!(bank.gpu_copy_rows(0, &mut values[..1]).is_err());
        assert!(
            bank.gpu_copy_fractions(PHASES + 1, &mut values[..bins * 2])
                .is_err()
        );
    }
}
