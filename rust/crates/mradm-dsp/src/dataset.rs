use crate::{Error, Result, data};

pub struct Dataset {
    pub sample_rate: u32,
    pub num_dirs: usize,
    pub ir_len: usize,
    pub directions: Vec<f32>,
    pub impulses: Vec<f32>,
    pub name: String,
}
impl Dataset {
    pub fn kemar() -> Self {
        Self {
            sample_rate: 48000,
            num_dirs: 836,
            ir_len: 256,
            directions: data::floats(data::KEMAR_DIRS),
            impulses: data::floats(data::KEMAR_IR),
            name: "built-in KEMAR".into(),
        }
    }

    #[cfg(feature = "sofa")]
    pub fn sofa(bytes: &[u8]) -> Result<Self> {
        // The raw entry point does not normalize, resample or replace the
        // application's interpolation. GeneralFIR is validated here as well.
        let raw =
            sofar::RawHrtf::from_bytes(bytes).map_err(|_| Error::Io("Invalid SOFA/HDF5 file"))?;
        if !matches!(
            raw.get_attribute("SOFAConventions"),
            Some("SimpleFreeFieldHRIR" | "GeneralFIR")
        ) {
            return Err(Error::Unsupported(
                "SOFA: only SimpleFreeFieldHRIR and GeneralFIR conventions are supported",
            ));
        }
        if raw.get_attribute("DataType") != Some("FIR") {
            return Err(Error::Unsupported("SOFA: only FIR data is supported"));
        }
        if raw.r() != 2 {
            return Err(Error::Unsupported("SOFA: expected 2 receivers"));
        }
        let num_dirs = raw.m() as usize;
        let ir_len = raw.n() as usize;
        let ir_count = num_dirs.checked_mul(2).and_then(|n| n.checked_mul(ir_len));
        if num_dirs < 4
            || ir_len == 0
            || ir_count != Some(raw.data_ir.values.len())
            || raw.source_position.values.len() != num_dirs * 3
        {
            return Err(Error::Unsupported(
                "SOFA: missing usable FIR directions or invalid dimensions",
            ));
        }
        let rates = &raw.data_sampling_rate.values;
        if rates.is_empty()
            || !rates[0].is_finite()
            || rates[0] < 1.
            || rates[0] >= i32::MAX as f32
            || rates.iter().any(|r| *r != rates[0])
        {
            return Err(Error::Unsupported(
                "SOFA: invalid or nonuniform sampling rate",
            ));
        }
        if raw
            .data_ir
            .values
            .iter()
            .chain(raw.source_position.values.iter())
            .any(|v| !v.is_finite())
        {
            return Err(Error::Unsupported("SOFA: nonfinite samples or coordinates"));
        }
        let coordinate_type = raw.source_position.get_attribute("Type");
        let units = raw.source_position.get_attribute("Units").unwrap_or("");
        let spherical = coordinate_type == Some("spherical");
        if !spherical && coordinate_type != Some("cartesian") {
            return Err(Error::Unsupported("SOFA: unsupported SourcePosition Type"));
        }
        if !units.contains("met") || (spherical && !units.contains("degree")) {
            return Err(Error::Unsupported("SOFA: invalid SourcePosition units"));
        }
        let name = raw
            .get_attribute("ListenerShortName")
            .unwrap_or("")
            .to_owned();
        let mut directions = Vec::with_capacity(num_dirs * 2);
        for point in raw.source_position.values.as_chunks::<3>().0.iter() {
            if spherical {
                directions.extend_from_slice(&point[..2]);
            } else {
                let [x, y, z] = [point[0] as f64, point[1] as f64, point[2] as f64];
                directions.push(y.atan2(x).to_degrees() as f32);
                directions.push(z.atan2((x * x + y * y).sqrt()).to_degrees() as f32);
            }
        }
        Ok(Self {
            sample_rate: rates[0].round() as u32,
            num_dirs,
            ir_len,
            directions,
            impulses: raw.data_ir.values,
            name,
        })
    }

    #[cfg(not(feature = "sofa"))]
    pub fn sofa(_bytes: &[u8]) -> Result<Self> {
        Err(Error::Unsupported("SOFA loading is disabled in this build"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn kemar_and_invalid_input() {
        let data = Dataset::kemar();
        assert_eq!(data.impulses.len(), 836 * 2 * 256);
        assert_eq!(data.directions.len(), 836 * 2);
        assert_eq!(data.sample_rate, 48000);
        assert!(Dataset::sofa(b"not a SOFA file").is_err());
    }

    #[test]
    #[cfg(feature = "sofa")]
    fn parses_both_conventions_without_normalization_or_resampling() {
        for (bytes, rate) in [
            (
                include_bytes!("../../../../tests/fixtures/sofa/simple.sofa").as_slice(),
                48000,
            ),
            (
                include_bytes!("../../../../tests/fixtures/sofa/general-compressed.sofa")
                    .as_slice(),
                44100,
            ),
        ] {
            let data = Dataset::sofa(bytes).unwrap();
            assert_eq!(data.sample_rate, rate);
            assert_eq!((data.num_dirs, data.ir_len), (6, 16));
            assert_eq!(data.name, "MacinRender synthetic");
            for dir in 0..6 {
                for tap in 0..16 {
                    assert_eq!(
                        data.impulses[(dir * 2) * 16 + tap],
                        if tap == 2 { (dir + 1) as f32 / 8. } else { 0. }
                    );
                    assert_eq!(
                        data.impulses[(dir * 2 + 1) * 16 + tap],
                        if tap == 4 { (6 - dir) as f32 / 8. } else { 0. }
                    );
                }
            }
            for (actual, expected) in data
                .directions
                .iter()
                .zip([0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.])
            {
                assert!((actual - expected).abs() < 1e-5);
            }
        }
    }

    #[test]
    #[cfg(feature = "sofa")]
    fn large_compressed_ir_is_independent_of_chunk_partition() {
        let single = Dataset::sofa(include_bytes!(
            "../../../../tests/fixtures/sofa/large-single-chunk.sofa"
        ))
        .unwrap();
        let split = Dataset::sofa(include_bytes!(
            "../../../../tests/fixtures/sofa/large-split-chunks.sofa"
        ))
        .unwrap();
        assert_eq!(
            (single.num_dirs, single.ir_len, single.sample_rate),
            (836, 1024, 48000)
        );
        assert_eq!(
            (split.num_dirs, split.ir_len, split.sample_rate),
            (836, 1024, 48000)
        );
        assert_eq!(single.directions, split.directions);
        assert_eq!(single.impulses, split.impulses);
        for dir in 0..single.num_dirs {
            for tap in 0..single.ir_len {
                assert_eq!(
                    single.impulses[(dir * 2) * single.ir_len + tap],
                    if tap == 2 { 0.125 } else { 0. }
                );
                assert_eq!(
                    single.impulses[(dir * 2 + 1) * single.ir_len + tap],
                    if tap == 4 { 0.75 } else { 0. }
                );
            }
        }
    }

    #[test]
    #[cfg(feature = "sofa")]
    fn invalid_chunk_dimensions_are_rejected_without_panicking() {
        let fixture = include_bytes!("../../../../tests/fixtures/sofa/large-single-chunk.sofa");
        // HDF5 v3 chunk layout stores M, R, N and the element size as little-endian u32s.
        let shape: Vec<u8> = [836_u32, 2, 1024, 8]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect();
        let offset = fixture
            .windows(shape.len())
            .position(|bytes| bytes == shape)
            .expect("fixture contains its chunk layout");
        for invalid in [
            [0, 2, 1024, 8],
            [836, 2, 1024, 0],
            [836, 2, 1 << 20, 8],              // Decoded chunk exceeds 256 MiB.
            [u32::MAX, u32::MAX, u32::MAX, 8], // Dimension product overflows.
        ] {
            let mut bytes = fixture.to_vec();
            for (slot, value) in bytes[offset..offset + shape.len()]
                .as_chunks_mut::<4>()
                .0
                .iter_mut()
                .zip(invalid)
            {
                slot.copy_from_slice(&value.to_le_bytes());
            }
            assert!(Dataset::sofa(&bytes).is_err(), "chunk layout={invalid:?}");
        }
    }
}
