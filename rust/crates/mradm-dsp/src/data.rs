//! Immutable binary32 resources. Provenance and checksums are in assets/manifest.json.
pub fn floats(bytes: &[u8]) -> Vec<f32> {
    bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|b| f32::from_le_bytes(*b))
        .collect()
}
pub const KEMAR_IR: &[u8] = include_bytes!("../assets/kemar_hrirs.f32le");
pub const KEMAR_DIRS: &[u8] = include_bytes!("../assets/kemar_directions.f32le");
pub const AFSTFT_PROTOTYPE: &[u8] = include_bytes!("../assets/afstft_prototype.f32le");
pub const LATTICE_20: &[u8] = include_bytes!("../assets/lattice_o20.f32le");
pub const LATTICE_15: &[u8] = include_bytes!("../assets/lattice_o15.f32le");
pub const LATTICE_6: &[u8] = include_bytes!("../assets/lattice_o6.f32le");
pub const HOA_714: &[u8] = include_bytes!("../assets/hoa3_714_sn3d.f32le");

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn resources_have_expected_shapes_and_finite_values() {
        for (data, count) in [
            (KEMAR_IR, 836 * 2 * 256),
            (KEMAR_DIRS, 836 * 2),
            (AFSTFT_PROTOTYPE, 10240),
            (LATTICE_20, 40),
            (LATTICE_15, 30),
            (LATTICE_6, 12),
            (HOA_714, 176),
        ] {
            assert_eq!(data.len(), count * 4);
            assert!(floats(data).iter().all(|x| x.is_finite()));
        }
    }
}
