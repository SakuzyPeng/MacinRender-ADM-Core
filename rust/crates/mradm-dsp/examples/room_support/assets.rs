//! File I/O shared only by offline validation/benchmark executables.
use mradm_dsp::{
    Error, Result,
    room::{
        collection,
        resources::{HrirData, RoomProfile},
        sofa,
    },
};
use std::{fs, path::Path};

pub fn asset(root: &Path, name: &str, expected: usize) -> Result<Vec<u8>> {
    let path = root.join(name);
    if fs::metadata(&path)
        .map_err(|_| Error::Io("Missing room asset"))?
        .len()
        != expected as u64
    {
        return Err(Error::Io("Room asset length mismatch"));
    }
    fs::read(path).map_err(|_| Error::Io("Cannot read room asset"))
}
pub fn profile(path: &Path) -> Result<RoomProfile> {
    let root = path.parent().unwrap_or(Path::new("."));
    RoomProfile::parse(
        &fs::read(path).map_err(|_| Error::Io("Missing room manifest"))?,
        |n, s| asset(root, n, s),
    )
}
#[allow(dead_code)]
pub fn hrir(path: &Path) -> Result<HrirData> {
    hrir_at_rate(path, 48000)
}
pub fn hrir_at_rate(path: &Path, rate: u32) -> Result<HrirData> {
    let resolved = if path.is_dir() {
        path.join("distance_index.json")
    } else {
        path.to_owned()
    };
    let path = resolved.as_path();
    let root = path.parent().unwrap_or(Path::new("."));
    let size = fs::metadata(path)
        .map_err(|_| Error::Io("Missing HRIR resource"))?
        .len();
    if size > 256 * 1024 * 1024 {
        return Err(Error::Unsupported("HRIR resource exceeds budget"));
    }
    let bytes = fs::read(path).map_err(|_| Error::Io("Cannot read HRIR resource"))?;
    if bytes.starts_with(b"\x89HDF\r\n\x1a\n") {
        return sofa::prepare_at_rate(&bytes, rate);
    }
    let manifest: serde_json::Value =
        serde_json::from_slice(&bytes).map_err(|_| Error::InvalidArgument("Invalid HRIR JSON"))?;
    if matches!(
        manifest["schema"].as_str(),
        Some("mradm.hrtf-collection" | "mradm.sofa-bank")
    ) {
        let mut data = collection::prepare_at_rate(&bytes, rate, |n, s| asset(root, n, s))?;
        let review = root.join(if manifest["schema"] == "mradm.sofa-bank" {
            "quality_report.json"
        } else {
            collection::REVIEW_PATH
        });
        if review
            .try_exists()
            .map_err(|_| Error::Io("Cannot check HRIR review"))?
        {
            collection::attach_reviews(
                &mut data,
                &fs::read(review).map_err(|_| Error::Io("Cannot read HRIR review"))?,
            )?;
        }
        Ok(data)
    } else {
        HrirData::parse(&bytes, |n, s| asset(root, n, s))
    }
}
