//! Authenticated distance layers. The enclosing HrirData owns the reference
//! response; other layers retain their original samples and estimated delays.
use super::resources::{HrirData, sha256};
use crate::{Error, Result, geometry::Vec3};
use serde_json::Value;

pub const MAX_BYTES: usize = 1024 * 1024 * 1024;
pub const REVIEW_PATH: &str = "provenance/source_nearfield_quality_report.json";

pub struct SourceGeometry {
    pub radius: f64,
    pub sample_rate: u32,
    pub receivers: [Vec3; 2],
    pub order: [usize; 2],
    pub origins: Value,
}

pub struct Layer {
    pub radius: f64,
    // None denotes the enclosing reference data, avoiding a duplicate FIR bank.
    pub data: Option<HrirData>,
    pub quality: Value,
}

pub struct Collection {
    pub name: String,
    pub reference: usize,
    pub layers: Vec<Layer>,
    pub reviews: Value,
}

impl Collection {
    pub fn bracket(&self, radius: f64) -> (usize, usize, f64) {
        let last = self.layers.len() - 1;
        if radius <= self.layers[0].radius {
            return (0, 0, 0.);
        }
        if radius >= self.layers[last].radius {
            return (last, last, 0.);
        }
        let high = self.layers.partition_point(|layer| layer.radius < radius);
        if self.layers[high].radius == radius {
            return (high, high, 0.);
        }
        let low = high - 1;
        let alpha = (radius - self.layers[low].radius)
            / (self.layers[high].radius - self.layers[low].radius);
        (low, high, alpha)
    }

    pub fn summary(&self) -> Value {
        serde_json::json!({"name":self.name,"variant":"HRIR_DFEQ100",
            "radii_m":self.layers.iter().map(|l|l.radius).collect::<Vec<_>>(),
            "reference_radius_m":self.layers[self.reference].radius,
            "direct_interpolation":"delay-aligned-linear-radius",
            "early_interpolation":"delay-aligned-image-path-radius",
            "late_interpolation":"fixed-reference-layer",
            "outside_range":"clamp-hrtf-only", "prepared_budget_bytes":MAX_BYTES,
            "layers":self.layers.iter().map(|l|serde_json::json!({
                "radius_m":l.radius,"provenance":l.quality})).collect::<Vec<_>>(),
            "nearfield_review":self.reviews})
    }
}

impl HrirData {
    pub fn layer(&self, index: usize) -> &Self {
        self.collection.as_ref().unwrap().layers[index]
            .data
            .as_ref()
            .unwrap_or(self)
    }

    pub fn delay_bounds(&self) -> (f64, f64) {
        let mut low = f64::INFINITY;
        let mut high = f64::NEG_INFINITY;
        let count = self.collection.as_ref().map_or(1, |c| c.layers.len());
        for index in 0..count {
            let data = if self.collection.is_some() {
                self.layer(index)
            } else {
                self
            };
            for delay in data.delays.iter().flatten().copied() {
                low = low.min(delay);
                high = high.max(delay);
            }
        }
        (low, high)
    }
}

fn invalid() -> Error {
    Error::InvalidArgument("距离 HRTF 索引、文件或层间几何不一致")
}

/// Collection paths may contain subdirectories, but stay relative to the index.
pub fn relative_path(name: &str) -> Result<&str> {
    if name.is_empty()
        || name.contains(['\\', ':', '\0'])
        || name.split('/').any(|part| matches!(part, "" | "." | ".."))
    {
        return Err(invalid());
    }
    Ok(name)
}

pub fn prepare(bytes: &[u8], read: impl FnMut(&str, usize) -> Result<Vec<u8>>) -> Result<HrirData> {
    prepare_at_rate(bytes, super::SAMPLE_RATE, read)
}

fn read_authenticated(
    layer: &Value,
    total: &mut usize,
    read: &mut impl FnMut(&str, usize) -> Result<Vec<u8>>,
) -> Result<(Vec<u8>, String)> {
    let file = &layer["files"]["HRIR_DFEQ100"];
    let name = relative_path(file["path"].as_str().ok_or_else(invalid)?)?;
    if layer["primary"].as_str() != Some(name) {
        return Err(invalid());
    }
    let size = file["bytes"]
        .as_u64()
        .and_then(|s| usize::try_from(s).ok())
        .filter(|s| *s > 0 && *s <= 256 * 1024 * 1024)
        .ok_or_else(invalid)?;
    *total = total
        .checked_add(size)
        .filter(|s| *s <= MAX_BYTES)
        .ok_or_else(invalid)?;
    let bytes = read(name, size)?;
    if bytes.len() != size {
        return Err(Error::InvalidArgument(
            "距离 HRTF 文件长度或 SHA-256 不匹配",
        ));
    }
    let hash = sha256(&bytes);
    if file["sha256"].as_str() != Some(hash.as_str()) {
        return Err(Error::InvalidArgument(
            "距离 HRTF 文件长度或 SHA-256 不匹配",
        ));
    }
    Ok((bytes, hash))
}

/// Reauthenticate the payloads of an already prepared, content-addressed index.
/// This does not replace schema/geometry validation in `prepare_at_rate`.
/// Each payload is released before reading the next; cache hits never retain
/// a second raw SOFA bank or trust file timestamps in place of hashes.
pub fn authenticate_files(
    bytes: &[u8],
    mut read: impl FnMut(&str, usize) -> Result<Vec<u8>>,
) -> Result<()> {
    if bytes.len() > 1024 * 1024 {
        return Err(invalid());
    }
    let value: Value = serde_json::from_slice(bytes).map_err(|_| invalid())?;
    let layers = value["layers"]
        .as_array()
        .filter(|v| (2..=64).contains(&v.len()))
        .ok_or_else(invalid)?;
    let mut total = 0;
    for layer in layers {
        let _ = read_authenticated(layer, &mut total, &mut read)?;
    }
    Ok(())
}

pub fn prepare_at_rate(
    bytes: &[u8],
    sample_rate: u32,
    mut read: impl FnMut(&str, usize) -> Result<Vec<u8>>,
) -> Result<HrirData> {
    super::validate_sample_rate(sample_rate)?;
    if bytes.len() > 1024 * 1024 {
        return Err(invalid());
    }
    let value: Value = serde_json::from_slice(bytes).map_err(|_| invalid())?;
    let bank = value["schema"] == "mradm.sofa-bank";
    if (value["schema"] != "mradm.hrtf-collection" && !bank)
        || value["version"] != 1
        || value["primary_variant"] != "HRIR_DFEQ100"
        || !matches!(value["sampling_rate_hz"].as_u64(), Some(48000 | 96000))
    {
        return Err(Error::Unsupported(
            "仅支持 48/96 kHz HRIR_DFEQ100 距离集合或 SOFA bank v1",
        ));
    }
    let layers = value["layers"]
        .as_array()
        .filter(|a| (2..=64).contains(&a.len()))
        .ok_or_else(invalid)?;
    let radii = value["radii_m"]
        .as_array()
        .filter(|a| a.len() == layers.len())
        .ok_or_else(invalid)?;
    let count = (if bank {
        &layers[0]["directions"]
    } else {
        &value["directions_per_distance"]
    })
    .as_u64()
    .filter(|count| (4..=65536).contains(count))
    .ok_or_else(invalid)? as usize;
    let mut reference = None;
    let mut loaded: Vec<Layer> = Vec::with_capacity(layers.len());
    let mut total = 0usize;
    let mut decoded_bytes = 0usize;
    for (index, layer) in layers.iter().enumerate() {
        let radius = layer["radius_m"]
            .as_f64()
            .filter(|r| r.is_finite() && *r > 0.)
            .ok_or_else(invalid)?;
        if radii[index].as_f64() != Some(radius)
            || loaded.last().is_some_and(|l| l.radius >= radius)
            || layer["directions"].as_u64() != Some(count as u64)
        {
            return Err(invalid());
        }
        if (if bank {
            (radius - 1.5).abs() < 1e-9
        } else {
            layer["reference_layer"].as_bool().ok_or_else(invalid)?
        }) && reference.replace(index).is_some()
        {
            return Err(invalid());
        }
        let (bytes, hash) = read_authenticated(layer, &mut total, &mut read)?;
        let data = super::sofa::prepare_authenticated(&bytes, sample_rate, hash)?;
        decoded_bytes = decoded_bytes
            .checked_add(
                data.impulses.len() * 4
                    + data.delays.len() * 16
                    + data.directions.len() * 24
                    + data.faces.len() * 12,
            )
            .filter(|total| *total <= MAX_BYTES)
            .ok_or(Error::Unsupported("距离 HRTF 解码资源超过 1 GiB 预算"))?;
        let geometry = data.source_geometry.as_ref().ok_or_else(invalid)?;
        if Some(u64::from(geometry.sample_rate)) != value["sampling_rate_hz"].as_u64()
            || (geometry.radius - radius).abs() > 1e-6 + 1e-5 * radius
            || data.directions.len() != count
        {
            return Err(invalid());
        }
        if let Some(first) = loaded.first().and_then(|l| l.data.as_ref()) {
            let first_geometry = first.source_geometry.as_ref().ok_or_else(invalid)?;
            if data.preprocessing_guard != first.preprocessing_guard
                || geometry.order != first_geometry.order
                || geometry
                    .receivers
                    .iter()
                    .flatten()
                    .zip(first_geometry.receivers.iter().flatten())
                    .any(|(a, b)| (a - b).abs() > 1e-9)
                || data
                    .directions
                    .iter()
                    .flatten()
                    .zip(first.directions.iter().flatten())
                    .any(|(a, b)| (a - b).abs() > 1e-9)
            {
                return Err(invalid());
            }
        }
        let quality = if bank {
            geometry.origins.clone()
        } else {
            layer["provenance"].clone()
        };
        loaded.push(Layer {
            radius,
            data: Some(data),
            quality,
        });
    }
    let reference = reference.ok_or_else(invalid)?;
    let mut data = loaded[reference].data.take().ok_or_else(invalid)?;
    data.identity = if sample_rate == super::SAMPLE_RATE {
        sha256(format!("mradm-distance-hrir-v1:{}", sha256(bytes)).as_bytes())
    } else {
        sha256(format!("mradm-distance-hrir-v2:{sample_rate}:{}", sha256(bytes)).as_bytes())
    };
    data.collection = Some(Collection {
        name: if bank {
            value["collection"].as_str().unwrap_or("SOFA bank")
        } else {
            value["collection"].as_str().ok_or_else(invalid)?
        }
        .to_owned(),
        reference,
        layers: loaded,
        reviews: Value::Null,
    });
    let (low, high) = data.delay_bounds();
    let spread = libm::ceil(high - low) as usize;
    let taps = (0..data.collection.as_ref().unwrap().layers.len())
        .map(|i| data.layer(i).taps)
        .max()
        .unwrap();
    data.guard = 96.max(40 + spread);
    data.output_taps = (taps + 40 + data.guard + spread).div_ceil(64) * 64;
    if data.guard > 4096 || data.output_taps > 8192 {
        return Err(invalid());
    }
    Ok(data)
}

/// Optional delivery evidence, reported verbatim; never used to alter samples.
pub fn attach_reviews(data: &mut HrirData, bytes: &[u8]) -> Result<()> {
    if bytes.len() > 1024 * 1024 {
        return Err(invalid());
    }
    let report: Value = serde_json::from_slice(bytes).map_err(|_| invalid())?;
    data.collection.as_mut().ok_or_else(invalid)?.reviews = report;
    Ok(())
}
