//! Versioned, bounded resource decoding. All allocations happen at preparation.
use crate::{Error, Result, geometry::Vec3};
use serde_json::Value;
use sha2::{Digest, Sha256};

const MAX_ASSET: usize = 256 * 1024 * 1024;

fn invalid() -> Error {
    Error::InvalidArgument("Invalid room resource manifest or array")
}

pub fn sha256(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

pub(crate) fn hash_f32(values: impl Iterator<Item = f32>) -> String {
    let mut hash = Sha256::new();
    for value in values {
        hash.update(value.to_le_bytes());
    }
    format!("{:x}", hash.finalize())
}
pub(crate) fn hash_f64(values: impl Iterator<Item = f64>) -> String {
    let mut hash = Sha256::new();
    for value in values {
        hash.update(value.to_le_bytes());
    }
    format!("{:x}", hash.finalize())
}
pub(crate) fn hash_complex(values: &[crate::Complex32]) -> String {
    hash_f32(values.iter().flat_map(|v| [v.re, v.im]))
}

pub fn manifest(bytes: &[u8], schema: &str, kind: &str) -> Result<Value> {
    if bytes.len() > 1024 * 1024 {
        return Err(invalid());
    }
    let value: Value = serde_json::from_slice(bytes).map_err(|_| invalid())?;
    if value["schema"] != schema
        || value["version"] != 1
        || value["kind"] != kind
        || !matches!(value["sample_rate"].as_u64(), Some(48000 | 96000))
    {
        return Err(Error::Unsupported(
            "Unsupported room resource schema, kind or rate",
        ));
    }
    Ok(value)
}

pub fn number(value: &Value) -> Result<f64> {
    value.as_f64().filter(|v| v.is_finite()).ok_or_else(invalid)
}

pub fn integer(value: &Value, maximum: usize) -> Result<usize> {
    value
        .as_u64()
        .and_then(|v| usize::try_from(v).ok())
        .filter(|v| *v <= maximum)
        .ok_or_else(invalid)
}

fn vec3(value: &Value) -> Result<Vec3> {
    let array = value
        .as_array()
        .filter(|v| v.len() == 3)
        .ok_or_else(invalid)?;
    Ok([number(&array[0])?, number(&array[1])?, number(&array[2])?])
}

pub fn asset_name(value: &Value) -> Result<&str> {
    value
        .as_str()
        .filter(|s| {
            !s.is_empty()
                && *s != "."
                && *s != ".."
                && !s
                    .chars()
                    .any(|c| c == '/' || c == '\\' || c == ':' || c == '\0')
        })
        .ok_or_else(invalid)
}

pub struct Array {
    pub shape: Vec<usize>,
    pub bytes: Vec<u8>,
}

impl Array {
    fn read(
        value: &Value,
        dtype: &str,
        width: usize,
        read: &mut impl FnMut(&str, usize) -> Result<Vec<u8>>,
    ) -> Result<Self> {
        if value["dtype"] != dtype {
            return Err(invalid());
        }
        let dimensions = value["shape"]
            .as_array()
            .filter(|s| !s.is_empty() && s.len() <= 3)
            .ok_or_else(invalid)?;
        let shape: Vec<_> = dimensions
            .iter()
            .map(|v| integer(v, MAX_ASSET))
            .collect::<Result<_>>()?;
        let length = shape
            .iter()
            .try_fold(width, |a, b| a.checked_mul(*b))
            .filter(|v| *v > 0 && *v <= MAX_ASSET)
            .ok_or_else(invalid)?;
        if integer(&value["bytes"], MAX_ASSET)? != length {
            return Err(invalid());
        }
        let digest = value["sha256"].as_str().ok_or_else(invalid)?;
        if digest.len() != 64 || !digest.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(invalid());
        }
        let bytes = read(asset_name(&value["file"])?, length)?;
        if bytes.len() != length || sha256(&bytes) != digest.to_ascii_lowercase() {
            return Err(Error::InvalidArgument(
                "Room resource length or SHA-256 mismatch",
            ));
        }
        Ok(Self { shape, bytes })
    }

    pub fn f32(
        value: &Value,
        read: &mut impl FnMut(&str, usize) -> Result<Vec<u8>>,
    ) -> Result<(Vec<usize>, Vec<f32>)> {
        let data = Self::read(value, "<f4", 4, read)?;
        let values: Vec<_> = data
            .bytes
            .as_chunks::<4>()
            .0
            .iter()
            .map(|v| f32::from_le_bytes(*v))
            .collect();
        if values.iter().any(|v| !v.is_finite()) {
            return Err(invalid());
        }
        Ok((data.shape, values))
    }

    pub fn f64(
        value: &Value,
        read: &mut impl FnMut(&str, usize) -> Result<Vec<u8>>,
    ) -> Result<(Vec<usize>, Vec<f64>)> {
        let data = Self::read(value, "<f8", 8, read)?;
        let values: Vec<_> = data
            .bytes
            .as_chunks::<8>()
            .0
            .iter()
            .map(|v| f64::from_le_bytes(*v))
            .collect();
        if values.iter().any(|v| !v.is_finite()) {
            return Err(invalid());
        }
        Ok((data.shape, values))
    }

    pub fn u32(
        value: &Value,
        read: &mut impl FnMut(&str, usize) -> Result<Vec<u8>>,
    ) -> Result<(Vec<usize>, Vec<u32>)> {
        let data = Self::read(value, "<u4", 4, read)?;
        Ok((
            data.shape,
            data.bytes
                .as_chunks::<4>()
                .0
                .iter()
                .map(|v| u32::from_le_bytes(*v))
                .collect(),
        ))
    }
}

#[derive(Clone)]
pub struct RoomProfile {
    pub sample_rate: u32,
    pub identity: String,
    pub id: String,
    pub name: String,
    pub default_hrir: String,
    pub dimensions: Vec3,
    pub listener: Vec3,
    pub speed: f64,
    pub reference_distance: f64,
    pub minimum_distance: f64,
    pub clearance: f64,
    pub reflection_order: usize,
    /// west, east, south, north, floor, ceiling; eight octave bands each.
    pub absorption: [[f64; 8]; 6],
    pub early_fade: [f64; 2],
    pub late_predelay: f64,
    pub late_energy: f64,
    pub alignment_guard: usize,
    pub band_offset: usize,
    pub band_taps: usize,
    pub bands: Vec<f64>,
    pub late_taps: usize,
    pub late: Vec<f32>,
    pub late_directions: Vec<Vec3>,
    pub eq: Vec<f32>,
    pub propagation_phases: Vec<f32>,
}

impl RoomProfile {
    pub fn parse(
        bytes: &[u8],
        mut read: impl FnMut(&str, usize) -> Result<Vec<u8>>,
    ) -> Result<Self> {
        let value = manifest(bytes, "mradm.room", "hybrid-shoebox-v1")?;
        if value["source_directivity"] != "cosine2-listener" {
            return Err(Error::Unsupported("Unsupported room source directivity"));
        }
        let dimensions = vec3(&value["dimensions"])?;
        let listener = vec3(&value["listener"])?;
        let minimum_distance = number(&value["minimum_distance"])?;
        let clearance = number(&value["boundary_clearance"])?;
        if !(0.01..=2.).contains(&minimum_distance) || !(0.01..=2.).contains(&clearance) {
            return Err(invalid());
        }
        for axis in 0..3 {
            if !(1.0..=50.0).contains(&dimensions[axis])
                || listener[axis] < minimum_distance + clearance
                || listener[axis] > dimensions[axis] - minimum_distance - clearance
            {
                return Err(invalid());
            }
        }
        let speed = number(&value["speed_of_sound"])?;
        let reference_distance = number(&value["reference_distance"])?;
        let reflection_order = integer(&value["reflection_order"], 8)?;
        let fade = value["early_fade_seconds"]
            .as_array()
            .filter(|v| v.len() == 2)
            .ok_or_else(invalid)?;
        let early_fade = [number(&fade[0])?, number(&fade[1])?];
        let late_predelay = number(&value["late_predelay_seconds"])?;
        let late_energy = number(&value["late_energy"])?;
        if !(300.0..=400.0).contains(&speed)
            || !(0.1..=10.0).contains(&reference_distance)
            || reflection_order == 0
            || early_fade[0] < 0.
            || early_fade[0] >= early_fade[1]
            || early_fade[1] > 0.1
            || !(0.0..=0.2).contains(&late_predelay)
            || !(0.0..=16.0).contains(&late_energy)
        {
            return Err(invalid());
        }
        let mut absorption = [[0.; 8]; 6];
        let walls = value["absorption"]
            .as_array()
            .filter(|a| a.len() == 6)
            .ok_or_else(invalid)?;
        for (out, wall) in absorption.iter_mut().zip(walls) {
            let bands = wall
                .as_array()
                .filter(|a| a.len() == 8)
                .ok_or_else(invalid)?;
            for (out, band) in out.iter_mut().zip(bands) {
                *out = number(band)?;
                if !(0.0..=1.0).contains(out) {
                    return Err(invalid());
                }
            }
        }
        let assets = &value["assets"];
        let (shape, bands) = Array::f64(&assets["bands"], &mut read)?;
        if shape.len() != 2 || shape[0] != 8 || shape[1] > 4096 {
            return Err(invalid());
        }
        let band_taps = shape[1];
        let band_offset = integer(&value["band_filter_offset"], band_taps - 1)?;
        let (shape, late) = Array::f32(&assets["late"], &mut read)?;
        if shape.len() != 2
            || shape[0] > 64
            || shape[1] > value["sample_rate"].as_u64().unwrap() as usize * 10
        {
            return Err(invalid());
        }
        let count = shape[0];
        let late_taps = shape[1];
        let (shape, directions) = Array::f64(&assets["late_directions"], &mut read)?;
        if shape != [count, 3] {
            return Err(invalid());
        }
        let late_directions = directions.as_chunks::<3>().0.to_vec();
        if late_directions
            .iter()
            .any(|v| (crate::geometry::norm(*v) - 1.).abs() > 1e-6)
        {
            return Err(invalid());
        }
        let (shape, eq) = Array::f64(&assets["eq"], &mut read)?;
        if shape.len() != 1 || shape[0] > 65536 || eq.iter().any(|v| !(*v as f32).is_finite()) {
            return Err(invalid());
        }
        let (shape, propagation_phases) = Array::f32(&assets["propagation_phases"], &mut read)?;
        if shape != [1025, 81] {
            return Err(invalid());
        }
        Ok(Self {
            sample_rate: value["sample_rate"].as_u64().unwrap() as u32,
            identity: sha256(&serde_json::to_vec(&value).map_err(|_| invalid())?),
            id: value["id"]
                .as_str()
                .filter(|s| !s.is_empty())
                .ok_or_else(invalid)?
                .to_owned(),
            name: value["name"].as_str().ok_or_else(invalid)?.to_owned(),
            default_hrir: asset_name(&value["default_hrir"])?.to_owned(),
            dimensions,
            listener,
            speed,
            reference_distance,
            minimum_distance,
            clearance,
            reflection_order,
            absorption,
            early_fade,
            late_predelay,
            late_energy,
            alignment_guard: integer(&value["alignment_guard_samples"], 256)?,
            band_offset,
            band_taps,
            bands,
            late_taps,
            late,
            late_directions,
            eq: eq.into_iter().map(|v| v as f32).collect(),
            propagation_phases,
        })
    }
}

pub struct HrirData {
    pub sample_rate: u32,
    pub source_sample_rate: Option<u32>,
    pub collection: Option<super::collection::Collection>,
    pub source_geometry: Option<super::collection::SourceGeometry>,
    pub identity: String,
    pub source_sofa_sha256: Option<String>,
    pub impulses: Vec<f32>,
    pub taps: usize,
    pub directions: Vec<Vec3>,
    pub faces: Vec<[u32; 3]>,
    pub delays: Vec<[f64; 2]>,
    pub guard: usize,
    pub preprocessing_guard: usize,
    pub output_taps: usize,
}

impl HrirData {
    pub fn parse(
        bytes: &[u8],
        mut read: impl FnMut(&str, usize) -> Result<Vec<u8>>,
    ) -> Result<Self> {
        let value = manifest(bytes, "mradm.hrir", "delay-aligned-v1")?;
        if value["fractional_taps"] != 81 {
            return Err(invalid());
        }
        let assets = &value["assets"];
        let (shape, impulses) = Array::f32(&assets["impulses"], &mut read)?;
        if shape.len() != 3 || shape[0] < 4 || shape[0] > 65536 || shape[1] != 2 || shape[2] > 4096
        {
            return Err(invalid());
        }
        let count = shape[0];
        let taps = shape[2];
        let (shape, directions) = Array::f64(&assets["directions"], &mut read)?;
        if shape != [count, 3] {
            return Err(invalid());
        }
        let directions = directions.as_chunks::<3>().0.to_vec();
        if directions
            .iter()
            .any(|v| (crate::geometry::norm(*v) - 1.).abs() > 1e-7)
        {
            return Err(invalid());
        }
        let (shape, faces) = Array::u32(&assets["faces"], &mut read)?;
        if shape.len() != 2
            || shape[1] != 3
            || shape[0] > count * 4
            || faces.iter().any(|v| *v as usize >= count)
        {
            return Err(invalid());
        }
        let (shape, delays) = Array::f64(&assets["delays"], &mut read)?;
        if shape != [count, 2] {
            return Err(invalid());
        }
        let guard = integer(&value["guard_samples"], 4096)?;
        let output_taps = integer(&value["output_taps"], 8192)?;
        let lo = delays.iter().copied().fold(f64::INFINITY, f64::min);
        let hi = delays.iter().copied().fold(f64::NEG_INFINITY, f64::max);
        if lo < -(taps as f64)
            || hi > taps as f64
            || guard as f64 - libm::ceil(hi - lo) < 40.
            || output_taps < taps + 80 + guard + libm::ceil(hi - lo) as usize - 40
        {
            return Err(Error::InvalidArgument(
                "HRIR guards do not retain complete fractional-delay support",
            ));
        }
        Ok(Self {
            sample_rate: value["sample_rate"].as_u64().unwrap() as u32,
            source_sample_rate: value["preparation"]["source_sample_rate"]
                .as_u64()
                .filter(|v| (1..=384000).contains(v))
                .map(|v| v as u32),
            identity: sha256(&serde_json::to_vec(&value).map_err(|_| invalid())?),
            collection: None,
            source_geometry: None,
            source_sofa_sha256: value
                .get("source_sofa_sha256")
                .and_then(Value::as_str)
                .map(str::to_owned),
            impulses,
            taps,
            directions,
            faces: faces.as_chunks::<3>().0.to_vec(),
            delays: delays.as_chunks::<2>().0.to_vec(),
            guard,
            preprocessing_guard: value
                .get("preprocessing_guard_samples")
                .map(|v| integer(v, 4096))
                .transpose()?
                .unwrap_or(0),
            output_taps,
        })
    }
}
