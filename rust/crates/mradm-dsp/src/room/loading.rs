//! Room package and HRIR loading (PCM consistency 4.6, moved from the private FFI): bounded file
//! reads, the room profile with its arrays, native SOFA, HRTF collection and prepared-bank
//! manifests, and the weak preparation caches that share immutable data between Scene and file
//! sessions without keeping inactive rooms alive. Paths arrive as the caller's UTF-8 bytes and
//! are checked where they are used.
use super::{
    collection,
    hrir::HrirBank,
    prepared::PreparedRoom,
    quality::Quality,
    resources::{HrirData, RoomProfile, sha256},
    session::PreparedSet,
};
use crate::{Error, Result};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
    sync::Arc,
};

fn invalid() -> Error {
    Error::InvalidArgument("Invalid Scene DSP buffer or parameter")
}

/// A regular file of at most `maximum` bytes (exactly `expected` when given).
pub fn bounded_read(path: &Path, expected: Option<usize>, maximum: usize) -> Result<Vec<u8>> {
    let metadata =
        fs::symlink_metadata(path).map_err(|_| Error::Io("Cannot inspect room resource"))?;
    if !metadata.file_type().is_file()
        || metadata.len() > maximum as u64
        || expected.is_some_and(|n| metadata.len() != n as u64)
    {
        return Err(Error::Io(
            "Room resource is not a regular file of the declared size",
        ));
    }
    let file = fs::File::open(path).map_err(|_| Error::Io("Cannot open room resource"))?;
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.take(maximum as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| Error::Io("Cannot read room resource"))?;
    if bytes.len() > maximum || expected.is_some_and(|n| bytes.len() != n) {
        return Err(Error::Io("Room resource changed while loading"));
    }
    Ok(bytes)
}

fn array(root: &Path, name: &str, expected: usize) -> Result<Vec<u8>> {
    bounded_read(&root.join(name), Some(expected), expected)
}

/// A nonempty UTF-8 path of at most 32 KiB without NUL.
pub fn path(bytes: &[u8]) -> Result<PathBuf> {
    if bytes.is_empty() || bytes.len() > 32768 || bytes.contains(&0) {
        return Err(invalid());
    }
    Ok(PathBuf::from(std::str::from_utf8(bytes).map_err(|_| {
        Error::InvalidArgument("Room paths must be UTF-8")
    })?))
}

/// A room package: its profile, the directory its arrays and default HRIR resolve against, and
/// the manifest bytes.
pub struct Package {
    pub profile: Arc<RoomProfile>,
    pub root: PathBuf,
    pub manifest: Vec<u8>,
}

pub fn load_package(name: &[u8]) -> Result<Package> {
    let path = path(name)?;
    let root = path.parent().unwrap_or(Path::new(".")).to_owned();
    let manifest = bounded_read(&path, None, 1024 * 1024)?;
    let profile = Arc::new(RoomProfile::parse(&manifest, |n, s| array(&root, n, s))?);
    Ok(Package {
        profile,
        root,
        manifest,
    })
}

// Preparation-only cache: live sessions own the data. No disk cache or Python
// process is needed, and an inactive SOFA bank can be reclaimed immediately.
pub(crate) fn native_hrir(bytes: &[u8], rate: u32) -> Result<Arc<HrirData>> {
    type Cache = std::collections::BTreeMap<String, std::sync::Weak<HrirData>>;
    static CACHE: std::sync::OnceLock<std::sync::Mutex<Cache>> = std::sync::OnceLock::new();
    let key = format!("{rate}:{}", sha256(bytes));
    let cache = CACHE.get_or_init(Default::default);
    {
        let mut entries = cache
            .lock()
            .map_err(|_| Error::RenderFailed("SOFA preparation cache failed"))?;
        entries.retain(|_, entry| entry.strong_count() != 0);
        if let Some(data) = entries.get(&key).and_then(std::sync::Weak::upgrade) {
            return Ok(data);
        }
    }
    // Parsing may unwind on malformed HDF5 in debug builds. The FFI boundary
    // catches it; do not poison the cache needed by subsequent sessions.
    let data = Arc::new(super::sofa::prepare_at_rate(bytes, rate)?);
    let mut entries = cache
        .lock()
        .map_err(|_| Error::RenderFailed("SOFA preparation cache failed"))?;
    // Another preparation may have finished while the lock was released.
    if let Some(existing) = entries.get(&key).and_then(std::sync::Weak::upgrade) {
        return Ok(existing);
    }
    entries.insert(key, Arc::downgrade(&data));
    Ok(data)
}

pub(crate) fn load_hrir(file: &Path, rate: u32) -> Result<Arc<HrirData>> {
    let resolved = if file.is_dir() {
        file.join("distance_index.json")
    } else {
        file.to_owned()
    };
    let file = resolved.as_path();
    let bytes = bounded_read(file, None, 256 * 1024 * 1024)?;
    if bytes.starts_with(b"\x89HDF\r\n\x1a\n") {
        return native_hrir(&bytes, rate);
    }
    let root = file.parent().unwrap_or(Path::new("."));
    let manifest: serde_json::Value = serde_json::from_slice(&bytes).map_err(|_| invalid())?;
    if matches!(
        manifest["schema"].as_str(),
        Some("mradm.hrtf-collection" | "mradm.sofa-bank")
    ) {
        let bank = manifest["schema"] == "mradm.sofa-bank";
        let review = root.join(if bank {
            "quality_report.json"
        } else {
            collection::REVIEW_PATH
        });
        let review = if review
            .try_exists()
            .map_err(|_| Error::Io("无法检查 HRTF 来源报告"))?
        {
            Some(bounded_read(&review, None, 1024 * 1024)?)
        } else {
            None
        };
        let name = bank.then(|| {
            root.file_name()
                .map_or_else(|| "SOFA bank".into(), |n| n.to_string_lossy().into_owned())
        });
        type Key = (String, u32, Option<String>, Option<String>);
        type Cache = std::collections::BTreeMap<Key, std::sync::Weak<HrirData>>;
        static CACHE: std::sync::OnceLock<std::sync::Mutex<Cache>> = std::sync::OnceLock::new();
        let key = (
            sha256(&bytes),
            rate,
            name.clone(),
            review.as_deref().map(sha256),
        );
        let cache = CACHE.get_or_init(Default::default);
        let existing = {
            let mut entries = cache
                .lock()
                .map_err(|_| Error::RenderFailed("Collection preparation cache failed"))?;
            entries.retain(|_, value| value.strong_count() != 0);
            entries.get(&key).and_then(std::sync::Weak::upgrade)
        };
        if let Some(data) = existing {
            // File bytes, not timestamps or an index-only key, authenticate a
            // hit. The original successful preparation validated its geometry.
            collection::authenticate_files(&bytes, |n, s| array(root, n, s))?;
            return Ok(data);
        }
        let mut data = collection::prepare_at_rate(&bytes, rate, |n, s| array(root, n, s))?;
        if let Some(name) = name {
            data.collection.as_mut().unwrap().name = name;
        }
        if let Some(review) = review {
            collection::attach_reviews(&mut data, &review)?;
        }
        let data = Arc::new(data);
        let mut entries = cache
            .lock()
            .map_err(|_| Error::RenderFailed("Collection preparation cache failed"))?;
        if let Some(existing) = entries.get(&key).and_then(std::sync::Weak::upgrade) {
            return Ok(existing);
        }
        entries.insert(key, Arc::downgrade(&data));
        return Ok(data);
    }
    Ok(Arc::new(HrirData::parse(&bytes, |n, s| array(root, n, s))?))
}

pub(crate) fn hrir_binding_identity(data: &HrirData) -> Result<Option<String>> {
    data.collection
        .as_ref()
        .map(|collection| {
            serde_json::to_vec(&collection.summary())
                .map(|bytes| sha256(&bytes))
                .map_err(|_| invalid())
        })
        .transpose()
}

/// The prepared room of `package` at `quality`. `reuse` pins the existing listener HRIR across a
/// room change. An empty `hrir` with a nonempty `sofa` prepares the SOFA natively; otherwise the
/// HRIR (by default the package's) loads, and a supplied SOFA must be its source.
pub fn prepare(
    package: &Package,
    hrir: &[u8],
    sofa: &[u8],
    reuse: Option<&PreparedSet>,
    quality: Quality,
) -> Result<Arc<PreparedSet>> {
    let profile = &package.profile;
    let data = if let Some(reuse) = reuse {
        if !hrir.is_empty() {
            return Err(Error::InvalidArgument(
                "Room switch cannot replace its HRIR binding",
            ));
        }
        reuse.room.hrir.data.clone()
    } else if hrir.is_empty() && !sofa.is_empty() {
        let bytes = bounded_read(&path(sofa)?, None, 256 * 1024 * 1024)?;
        native_hrir(&bytes, profile.sample_rate)?
    } else {
        let hrir_path = if hrir.is_empty() {
            package.root.join(&profile.default_hrir)
        } else {
            path(hrir)?
        };
        load_hrir(&hrir_path, profile.sample_rate)?
    };
    if !sofa.is_empty() {
        let bytes = bounded_read(&path(sofa)?, None, 256 * 1024 * 1024)?;
        if data.source_sofa_sha256.as_deref() != Some(sha256(&bytes).as_str()) {
            return Err(Error::InvalidArgument(
                "所选 HRIR 包与 SOFA 不匹配；请清除 HRIR 覆盖以直接使用 SOFA",
            ));
        }
    }
    let (length, _, _) = PreparedRoom::required_fft(profile, &data)?;
    type Cache = std::collections::BTreeMap<
        (String, String, Option<String>, usize, Quality),
        std::sync::Weak<PreparedSet>,
    >;
    static CACHE: std::sync::OnceLock<std::sync::Mutex<Cache>> = std::sync::OnceLock::new();
    // Preparation-only weak cache. It shares immutable data between Scene
    // and file sessions without keeping inactive rooms alive or touching DSP locks.
    let key = (
        profile.identity.clone(),
        data.identity.clone(),
        hrir_binding_identity(&data)?,
        length,
        quality,
    );
    let cache = CACHE.get_or_init(Default::default);
    let existing = {
        let mut entries = cache
            .lock()
            .map_err(|_| Error::RenderFailed("Room preparation cache failed"))?;
        entries.retain(|_, entry| entry.strong_count() != 0);
        entries.get(&key).and_then(std::sync::Weak::upgrade)
    };
    if let Some(prepared) = existing {
        return Ok(prepared);
    }
    let bank = match reuse {
        Some(reuse) if reuse.room.hrir.fft_length == length && reuse.room.quality == quality => {
            reuse.room.hrir.clone()
        }
        _ => Arc::new(HrirBank::with_policy(data, length, quality.fft())?),
    };
    let room = Arc::new(PreparedRoom::with_quality(profile.clone(), bank, quality)?);
    let prepared = Arc::new(PreparedSet::new(room)?);
    let mut entries = cache
        .lock()
        .map_err(|_| Error::RenderFailed("Room preparation cache failed"))?;
    if let Some(existing) = entries.get(&key).and_then(std::sync::Weak::upgrade) {
        Ok(existing)
    } else {
        entries.insert(key, Arc::downgrade(&prepared));
        Ok(prepared)
    }
}

/// Polar metadata in degrees to the room's listener-relative Cartesian metres (`f64` libm).
pub fn polar(azimuth: f32, elevation: f32, distance: f32) -> Result<[f32; 3]> {
    if !azimuth.is_finite() || !elevation.is_finite() || !distance.is_finite() || distance < 0. {
        return Err(invalid());
    }
    let az = f64::from(azimuth) * (std::f64::consts::PI / 180.);
    let el = f64::from(elevation) * (std::f64::consts::PI / 180.);
    let radius = f64::from(distance);
    Ok([
        ((-libm::sin(az) * libm::cos(el)) * radius) as f32,
        ((libm::cos(az) * libm::cos(el)) * radius) as f32,
        (libm::sin(el) * radius) as f32,
    ])
}

#[cfg(test)]
mod tests {
    struct CollectionFixture(std::path::PathBuf);
    impl CollectionFixture {
        fn new() -> Self {
            static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
            let id = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let stamp = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let root = std::env::temp_dir().join(format!(
                "mradm-collection-{}-{stamp}-{id}",
                std::process::id()
            ));
            std::fs::create_dir(&root).unwrap();
            let source = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../../tests/fixtures/sofa/distance-96");
            for name in ["distance_index.json", "near.sofa", "far.sofa"] {
                std::fs::copy(source.join(name), root.join(name)).unwrap();
            }
            Self(root)
        }
    }
    impl Drop for CollectionFixture {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    #[test]
    fn collection_hits_reauthenticate_files_and_keep_source_reports_current() {
        let fixture = CollectionFixture::new();
        let first = super::load_hrir(&fixture.0, 96000).unwrap();
        let hit = super::load_hrir(&fixture.0.join("distance_index.json"), 96000).unwrap();
        assert!(std::sync::Arc::ptr_eq(&first, &hit));
        let file = fixture.0.join("near.sofa");
        let bytes = std::fs::read(&file).unwrap();
        let mut broken = bytes.clone();
        broken[100] ^= 1;
        std::fs::write(&file, &broken).unwrap();
        assert!(super::load_hrir(&fixture.0, 96000).is_err());
        std::fs::remove_file(&file).unwrap();
        assert!(super::load_hrir(&fixture.0, 96000).is_err());
        std::fs::write(&file, &bytes).unwrap();
        let recovered = super::load_hrir(&fixture.0, 96000).unwrap();
        assert!(std::sync::Arc::ptr_eq(&first, &recovered));
        let review = fixture.0.join("quality_report.json");
        std::fs::write(&review, b"{\"revision\":1}").unwrap();
        let updated = super::load_hrir(&fixture.0, 96000).unwrap();
        assert_eq!(updated.identity, first.identity);
        assert_eq!(updated.collection.as_ref().unwrap().reviews["revision"], 1);
        assert_ne!(
            super::hrir_binding_identity(&updated).unwrap(),
            super::hrir_binding_identity(&first).unwrap()
        );
        std::fs::write(&review, b"not json").unwrap();
        assert!(super::load_hrir(&fixture.0, 96000).is_err());
    }
    #[test]
    fn collection_cache_separates_rates_and_names_and_reclaims_inactive_data() {
        let fixture = CollectionFixture::new();
        let other = CollectionFixture::new();
        let first = super::load_hrir(&fixture.0, 96000).unwrap();
        let converted = super::load_hrir(&fixture.0, 48000).unwrap();
        let renamed = super::load_hrir(&other.0, 96000).unwrap();
        assert_ne!(first.identity, converted.identity);
        assert_eq!((first.sample_rate, converted.sample_rate), (96000, 48000));
        assert_eq!(first.identity, renamed.identity);
        assert_ne!(
            first.collection.as_ref().unwrap().name,
            renamed.collection.as_ref().unwrap().name
        );
        assert_ne!(
            super::hrir_binding_identity(&first).unwrap(),
            super::hrir_binding_identity(&renamed).unwrap()
        );
        let weak = std::sync::Arc::downgrade(&first);
        drop(first);
        assert!(weak.upgrade().is_none());
    }
    #[test]
    fn concurrent_collection_requests_publish_one_live_bank() {
        let fixture = CollectionFixture::new();
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(4));
        let workers: Vec<_> = (0..4)
            .map(|_| {
                let path = fixture.0.clone();
                let barrier = barrier.clone();
                std::thread::spawn(move || {
                    barrier.wait();
                    super::load_hrir(&path, 96000).unwrap()
                })
            })
            .collect();
        let banks: Vec<_> = workers.into_iter().map(|w| w.join().unwrap()).collect();
        assert!(
            banks
                .iter()
                .all(|bank| std::sync::Arc::ptr_eq(bank, &banks[0]))
        );
        let weak = std::sync::Arc::downgrade(&banks[0]);
        drop(banks);
        assert!(weak.upgrade().is_none());
    }
    #[test]
    fn live_banks_are_shared_and_inactive_banks_are_reclaimed() {
        let bytes = include_bytes!("../../../../../tests/fixtures/sofa/simple.sofa");
        let first = super::native_hrir(bytes, 48000).unwrap();
        let second = super::native_hrir(bytes, 48000).unwrap();
        assert!(std::sync::Arc::ptr_eq(&first, &second));
        // The FFI boundary catches the unwind some HDF5 errors raise in debug builds; none may
        // disable later SOFA imports.
        let truncated = std::panic::catch_unwind(|| {
            super::native_hrir(&bytes[..bytes.len() - 1], 48000).map(|_| ())
        });
        assert!(!matches!(truncated, Ok(Ok(()))));
        let after_error = super::native_hrir(bytes, 48000).unwrap();
        assert!(std::sync::Arc::ptr_eq(&first, &after_error));
        drop(after_error);
        let weak = std::sync::Arc::downgrade(&first);
        drop(first);
        drop(second);
        assert!(weak.upgrade().is_none());
        assert!(super::native_hrir(b"not a SOFA", 48000).is_err());
    }
    #[test]
    fn concurrent_preparations_share_the_published_bank() {
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(4));
        let workers: Vec<_> = (0..4)
            .map(|_| {
                let barrier = barrier.clone();
                std::thread::spawn(move || {
                    barrier.wait();
                    super::native_hrir(
                        include_bytes!("../../../../../tests/fixtures/sofa/room-delayed.sofa"),
                        48000,
                    )
                    .unwrap()
                })
            })
            .collect();
        let data: Vec<_> = workers.into_iter().map(|w| w.join().unwrap()).collect();
        assert!(data.iter().all(|d| std::sync::Arc::ptr_eq(d, &data[0])));
    }
}
