//! Validate every measured HRIR node through the runtime interpolation path.
use mradm_dsp::{
    Error, Result,
    room::{
        hrir::{HrirBank, TimeQuery},
        resources::HrirData,
    },
};
use std::{fs, path::Path, sync::Arc};

fn read(root: &Path, name: &str, expected: usize) -> Result<Vec<u8>> {
    let path = root.join(name);
    let metadata = fs::symlink_metadata(&path).map_err(|_| Error::Io("Missing HRIR asset"))?;
    if !metadata.is_file() || metadata.len() != expected as u64 {
        return Err(Error::Io("HRIR asset length/type mismatch"));
    }
    fs::read(path).map_err(|_| Error::Io("Cannot read HRIR asset"))
}

fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    let argument = std::env::args()
        .nth(1)
        .ok_or("usage: room_hrir_check SOFA_OR_HRIR_JSON")?;
    let path = Path::new(&argument);
    let root = path.parent().ok_or("missing parent")?;
    let bytes = fs::read(path)?;
    let data = Arc::new(if bytes.starts_with(b"\x89HDF\r\n\x1a\n") {
        mradm_dsp::room::sofa::prepare(&bytes)?
    } else {
        HrirData::parse(&bytes, |n, s| read(root, n, s))?
    });
    let bank = Arc::new(HrirBank::new(
        data.clone(),
        data.output_taps.next_power_of_two(),
    )?);
    let mut query = TimeQuery::new(bank)?;
    let mut output = vec![0.; data.output_taps * 2];
    let mut maximum = 0f64;
    let mut worst_node = 0;
    for (index, direction) in data.directions.iter().enumerate() {
        let mut face = usize::MAX;
        query.query(*direction, &mut face, &mut output)?;
        let mut error = 0.;
        let mut energy = 0.;
        for ear in 0..2 {
            for i in 0..data.output_taps {
                let original = if i >= data.guard && i - data.guard < data.taps {
                    data.impulses[(index * 2 + ear) * data.taps + i - data.guard]
                } else {
                    0.
                };
                energy += f64::from(original).powi(2);
                error += f64::from(output[ear * data.output_taps + i] - original).powi(2);
            }
        }
        let relative = (error / energy).sqrt();
        if !relative.is_finite() {
            return Err("Invalid or silent HRIR node".into());
        }
        if relative > maximum {
            maximum = relative;
            worst_node = index;
        }
    }
    let passed = maximum <= 2e-5;
    let mut report = serde_json::json!({"schema":"mradm.hrir-node-validation", "version":1,
        "hrir_hash":data.identity, "nodes":data.directions.len(), "passed":passed,
        "maximum_relative_l2":maximum, "threshold_relative_l2":2e-5, "worst_node":worst_node,
        "preprocessing_guard_frames":data.preprocessing_guard, "interpolation_guard_frames":data.guard,
        "scope":"Measured-node amplitude/time reconstruction; no acoustic or device certification."});
    if bytes.starts_with(b"\x89HDF\r\n\x1a\n") {
        use mradm_dsp::room::resources::sha256;
        report["native_preparation"] = serde_json::json!({
            "source_sha256":data.source_sofa_sha256,"taps":data.taps,"output_taps":data.output_taps,
            "impulses":sha256(&data.impulses.iter().flat_map(|v|v.to_le_bytes()).collect::<Vec<_>>()),
            "directions":sha256(&data.directions.iter().flatten().flat_map(|v|v.to_le_bytes()).collect::<Vec<_>>()),
            "delays":sha256(&data.delays.iter().flatten().flat_map(|v|v.to_le_bytes()).collect::<Vec<_>>()),
            "faces":sha256(&data.faces.iter().flatten().flat_map(|v|v.to_le_bytes()).collect::<Vec<_>>())});
    }
    println!("{report}");
    if !passed {
        return Err("HRIR node reconstruction exceeds tolerance".into());
    }
    Ok(())
}
