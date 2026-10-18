//! Validate every measured HRIR node through the runtime interpolation path.
use mradm_dsp::{
    Error, Result,
    room::{
        collection,
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
    let supplied = Path::new(&argument);
    let resolved = if supplied.is_dir() {
        supplied.join("distance_index.json")
    } else {
        supplied.to_owned()
    };
    let path = resolved.as_path();
    let root = path.parent().ok_or("missing parent")?;
    let bytes = fs::read(path)?;
    let manifest = serde_json::from_slice::<serde_json::Value>(&bytes).ok();
    let args: Vec<_> = std::env::args().collect();
    let rate = if let Some(index) = args.iter().position(|v| v == "--sample-rate") {
        args.get(index + 1)
            .ok_or("Missing sample rate")?
            .parse::<u32>()?
    } else {
        manifest
            .as_ref()
            .and_then(|v| {
                v["sampling_rate_hz"]
                    .as_u64()
                    .or_else(|| v["sample_rate"].as_u64())
            })
            .unwrap_or(48000) as u32
    };
    if manifest.as_ref().is_some_and(|v| {
        matches!(
            v["schema"].as_str(),
            Some("mradm.hrtf-collection" | "mradm.sofa-bank")
        )
    }) {
        return check_collection(root, &bytes, rate);
    }
    let data = Arc::new(if bytes.starts_with(b"\x89HDF\r\n\x1a\n") {
        mradm_dsp::room::sofa::prepare_at_rate(&bytes, rate)?
    } else {
        HrirData::parse(&bytes, |n, s| read(root, n, s))?
    });
    if data.sample_rate != rate {
        return Err("Prepared HRIR sample rate differs from the requested rate".into());
    }
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
        "sample_rate":data.sample_rate,"hrir_hash":data.identity, "nodes":data.directions.len(), "passed":passed,
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

fn check_collection(
    root: &Path,
    bytes: &[u8],
    rate: u32,
) -> std::result::Result<(), Box<dyn std::error::Error>> {
    let started = std::time::Instant::now();
    let mut data = collection::prepare_at_rate(bytes, rate, |n, s| read(root, n, s))?;
    let manifest: serde_json::Value = serde_json::from_slice(bytes)?;
    let review = root.join(if manifest["schema"] == "mradm.sofa-bank" {
        "quality_report.json"
    } else {
        collection::REVIEW_PATH
    });
    if review.try_exists()? {
        collection::attach_reviews(&mut data, &fs::read(review)?)?;
    }
    let data = Arc::new(data);
    let bank = Arc::new(HrirBank::new(
        data.clone(),
        data.output_taps.next_power_of_two(),
    )?);
    let preparation_seconds = started.elapsed().as_secs_f64();
    let bank_bytes = bank.storage_bytes();
    let mut query = TimeQuery::new(bank)?;
    let mut output = vec![0.; data.output_taps * 2];
    let mut adjacent = output.clone();
    let collection = data.collection.as_ref().unwrap();
    let mut layers = Vec::new();
    let mut maximum = 0f64;
    let mut continuity = 0f64;
    let mut middle_count = 0;
    let began = std::time::Instant::now();
    for (index, layer) in collection.layers.iter().enumerate() {
        let original = data.layer(index);
        let mut worst = 0f64;
        let mut worst_node = 0;
        for (node, direction) in data.directions.iter().enumerate() {
            query.query_distance(
                *direction,
                Some(layer.radius),
                &mut { usize::MAX },
                &mut output,
            )?;
            let mut error = 0.;
            let mut energy = 0.;
            for ear in 0..2 {
                for i in 0..data.output_taps {
                    let expected = if i >= data.guard && i - data.guard < original.taps {
                        original.impulses[(node * 2 + ear) * original.taps + i - data.guard]
                    } else {
                        0.
                    };
                    error += f64::from(output[ear * data.output_taps + i] - expected).powi(2);
                    energy += f64::from(expected).powi(2);
                }
            }
            let relative = (error / energy).sqrt();
            if !relative.is_finite() {
                return Err("Invalid HRIR reconstruction".into());
            }
            if relative > worst {
                worst = relative;
                worst_node = node;
            }
            query.query_distance(
                *direction,
                Some(layer.radius - 1e-7),
                &mut { usize::MAX },
                &mut adjacent,
            )?;
            query.query_distance(
                *direction,
                Some(layer.radius + 1e-7),
                &mut { usize::MAX },
                &mut output,
            )?;
            let error: f64 = output
                .iter()
                .zip(&adjacent)
                .map(|(a, b)| f64::from(a - b).powi(2))
                .sum();
            continuity = continuity.max((error / energy).sqrt());
            if index + 1 < collection.layers.len() {
                let radius = (layer.radius + collection.layers[index + 1].radius) * 0.5;
                query.query_distance(*direction, Some(radius), &mut { usize::MAX }, &mut output)?;
                if output.iter().any(|v| !v.is_finite()) || output.iter().all(|v| *v == 0.) {
                    return Err("Invalid midpoint HRIR".into());
                }
                middle_count += 1;
            }
        }
        maximum = maximum.max(worst);
        layers.push(serde_json::json!({"radius_m":layer.radius,"nodes":data.directions.len(),"maximum_relative_l2":worst,
            "worst_node":worst_node,"passed":worst<=2e-5}));
    }
    let passed = maximum <= 2e-5 && continuity <= 2e-4;
    let report = serde_json::json!({"schema":"mradm.hrtf-collection-validation","version":1,"sample_rate":data.sample_rate,"passed":passed,
        "collection":collection.summary(),"layers":layers,"maximum_relative_l2":maximum,"threshold_relative_l2":2e-5,
        "boundary_probe_step_metres":1e-7,"maximum_boundary_relative_l2":continuity,"boundary_threshold_relative_l2":2e-4,
        "midpoint_queries":middle_count,"interpolation_guard_frames":data.guard,"output_taps":data.output_taps,
        "hrir_bank_bytes":bank_bytes,"preparation_seconds":preparation_seconds,"query_scan_seconds":began.elapsed().as_secs_f64(),
        "profile":if cfg!(debug_assertions){"debug"}else{"release"},
        "scope":"Node reconstruction and numerical continuity; inherited acoustic flags are unchanged."});
    println!("{report}");
    if !passed {
        return Err("Distance HRIR validation exceeds tolerance".into());
    }
    Ok(())
}
