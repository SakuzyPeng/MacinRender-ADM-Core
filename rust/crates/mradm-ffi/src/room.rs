//! Private room boundary. Loading/preparation may do I/O; session processing
//! only consumes checked PCM/control views. Handles own their data independently.
use crate::checked::*;
use mradm_dsp::{
    Error, Result,
    room::{
        collection,
        control::{RoomCommand, RoomDescription, RoomOverride},
        geometry::Position,
        hrir::HrirBank,
        prepared::PreparedRoom,
        quality::Quality,
        resources::{HrirData, RoomProfile, sha256},
        screen::{Mapping, RoomScreens},
        session::{Instance, PreparedSet, RoomInfo, Session},
    },
};
use std::{
    ffi::CString,
    fs,
    io::Read,
    path::{Path, PathBuf},
    ptr,
    sync::Arc,
};

fn boundary(message: *mut u8, capacity: usize, call: impl FnOnce() -> Result<()>) -> i32 {
    #[cfg(windows)]
    mradm_dsp::room::set_worker_start_hook(crate::room_environment::start_worker);
    let _environment = crate::room_environment::Environment::enter();
    crate::boundary(message, capacity, call)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_validate_screens(
    screens: *const RoomScreens,
    count: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        for value in input(screens, count)? {
            Mapping::new(*value)?;
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_validate_overrides(
    values: *const RoomOverride,
    count: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        for value in input(values, count)? {
            value.state()?;
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_overrides(
    handle: *mut Session,
    values: *const RoomOverride,
    count: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(handle, 1, values, count)?;
        (*handle).set_overrides(input(values, count)?)
    })
}

pub struct RoomProfileHandle {
    profile: Arc<RoomProfile>,
    root: PathBuf,
    json: CString,
}
pub struct RoomPreparedHandle {
    prepared: Arc<PreparedSet>,
    json: CString,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct RoomPlane {
    samples: *const f32,
    length: usize,
    has_signal: u32,
    reserved: u32,
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct RoomSourceInfo {
    id: u64,
    position: [f64; 3],
    direction: [f64; 3],
    distance: f64,
    constrained: u32,
    present: u32,
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct RoomPreparedInfo {
    latency_frames: u64,
    tail_frames: u64,
    preroll_frames: u64,
    material_guard_frames: u64,
    sample_rate: u32,
    reflection_update_frames: u32,
}

// Polar metadata conversion is part of the room consistency boundary.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_polar(
    azimuth: f32,
    elevation: f32,
    distance: f32,
    out: *mut f32,
) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        if !azimuth.is_finite() || !elevation.is_finite() || !distance.is_finite() || distance < 0.
        {
            return Err(invalid());
        }
        let az = f64::from(azimuth) * (std::f64::consts::PI / 180.);
        let el = f64::from(elevation) * (std::f64::consts::PI / 180.);
        let radius = f64::from(distance);
        output(out, 3)?.copy_from_slice(&[
            ((-libm::sin(az) * libm::cos(el)) * radius) as f32,
            ((libm::cos(az) * libm::cos(el)) * radius) as f32,
            (libm::sin(el) * radius) as f32,
        ]);
        Ok(())
    })
}

fn bounded_read(path: &Path, expected: Option<usize>, maximum: usize) -> Result<Vec<u8>> {
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
unsafe fn path(bytes: *const u8, length: usize) -> Result<PathBuf> {
    let bytes = unsafe { input(bytes, length)? };
    if bytes.is_empty() || bytes.len() > 32768 || bytes.contains(&0) {
        return Err(invalid());
    }
    Ok(PathBuf::from(std::str::from_utf8(bytes).map_err(|_| {
        Error::InvalidArgument("Room paths must be UTF-8")
    })?))
}
fn description(prepared: &PreparedSet, metres: f64) -> Result<CString> {
    let room = &prepared.room;
    let mut value = serde_json::json!({"schema":"mradm.room-prepared","version":1,
        "id":room.profile.id,"name":room.profile.name,"room_hash":room.profile.identity,
        "quality":room.quality.name(),"processing_policy":room.quality.policy(),
        "reflection_update_frames":room.quality.reflection_frames_at(room.profile.sample_rate),
        "reflection_pose_limit_degrees":room.quality.reflection_pose_degrees(),
        "reflection_pose_update_frames":room.quality.reflection_pose_frames_at(room.profile.sample_rate),
        "fft_policy":if room.quality == Quality::Exact { "scalar" } else { "native-auto" },
        "hrir_hash":room.hrir.data.identity,"source_sofa_sha256":room.hrir.data.source_sofa_sha256,
        "coordinate_mapping": {"mode": if metres == 0. { "room" } else { "uniform" },
            "metres_per_unit": if metres == 0. { None } else { Some(metres) },
            "origin": "listener", "dimensions_metres": room.profile.dimensions,
            "listener_metres": room.profile.listener, "boundary_clearance_metres": room.profile.clearance,
            "minimum_distance_metres": room.profile.minimum_distance},
        "sample_rate":room.profile.sample_rate,
        "source_sample_rate":room.hrir.data.source_sample_rate,
        "hrir_resampled":room.hrir.data.source_sample_rate.map(|rate|rate != room.hrir.data.sample_rate),
        "hrir_taps":room.hrir.data.taps,"latency_frames":mradm_dsp::room::BLOCK,
        "material_filter_guard_frames":room.profile.band_offset,
        "hrir_interpolation_guard_frames":room.hrir.data.guard,
        "hrir_preprocessing_guard_frames":room.hrir.data.preprocessing_guard,
        "propagation_guard_frames":40,"alignment_guard_frames":room.profile.alignment_guard,
        "tail_frames":prepared.tail_frames(),"preroll_frames":prepared.preroll_frames(),
        "hrir_directions":room.hrir.data.directions.len(),"late_directions":room.profile.late_directions.len(),
        "reflection_paths":room.images.len(),"hrir_bank_bytes":room.hrir.storage_bytes(),
        "windows_mmcss_workers":crate::room_environment::mmcss_workers(),
        "scope":"Validated resource integrity; acoustic and realtime acceptance are separate."});
    if let Some(collection) = &room.hrir.data.collection {
        value["hrtf_collection"] = collection.summary();
    }
    CString::new(value.to_string()).map_err(|_| invalid())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_profile_load(
    name: *const u8,
    length: usize,
    out: *mut *mut RoomProfileHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(name, length, out, 1)?;
        *out = ptr::null_mut();
        let path = path(name, length)?;
        let root = path.parent().unwrap_or(Path::new(".")).to_owned();
        let bytes = bounded_read(&path, None, 1024 * 1024)?;
        let profile = Arc::new(RoomProfile::parse(&bytes, |n, s| array(&root, n, s))?);
        let json = CString::new(bytes).map_err(|_| invalid())?;
        *out = Box::into_raw(Box::new(RoomProfileHandle {
            profile,
            root,
            json,
        }));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_profile_destroy(handle: *mut RoomProfileHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_profile_json(handle: *const RoomProfileHandle) -> *const u8 {
    if handle.is_null() {
        ptr::null()
    } else {
        unsafe { (*handle).json.as_ptr().cast() }
    }
}

// Preparation-only cache: live sessions own the data. No disk cache or Python
// process is needed, and an inactive SOFA bank can be reclaimed immediately.
fn native_hrir(bytes: &[u8], rate: u32) -> Result<Arc<HrirData>> {
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
    let data = Arc::new(mradm_dsp::room::sofa::prepare_at_rate(bytes, rate)?);
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
fn load_hrir(file: &Path, rate: u32) -> Result<Arc<HrirData>> {
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

fn hrir_binding_identity(data: &HrirData) -> Result<Option<String>> {
    data.collection
        .as_ref()
        .map(|collection| {
            serde_json::to_vec(&collection.summary())
                .map(|bytes| sha256(&bytes))
                .map_err(|_| invalid())
        })
        .transpose()
}

/// `reuse` pins the existing listener HRIR across a room change. Optional sofa
/// selects native preparation when no explicit HRIR bank is supplied. If both
/// inputs are supplied, authenticate their binding as before.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_prepare(
    profile: *const RoomProfileHandle,
    hrir_name: *const u8,
    hrir_length: usize,
    sofa_name: *const u8,
    sofa_length: usize,
    reuse: *const RoomPreparedHandle,
    out: *mut *mut RoomPreparedHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    unsafe {
        mradm_room_prepare_quality(
            profile,
            hrir_name,
            hrir_length,
            sofa_name,
            sofa_length,
            reuse,
            0,
            0.,
            out,
            message,
            capacity,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_prepare_quality(
    profile: *const RoomProfileHandle,
    hrir_name: *const u8,
    hrir_length: usize,
    sofa_name: *const u8,
    sofa_length: usize,
    reuse: *const RoomPreparedHandle,
    quality: u32,
    metres: f64,
    out: *mut *mut RoomPreparedHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(profile, 1, out, 1)?;
        separate(hrir_name, hrir_length, out, 1)?;
        separate(sofa_name, sofa_length, out, 1)?;
        *out = ptr::null_mut();
        let quality = Quality::try_from(quality)?;
        if !mradm_dsp::room::geometry::valid_scale(metres) {
            return Err(Error::InvalidArgument("Invalid room distance scale"));
        }
        let profile = &*profile;
        let data = if !reuse.is_null() {
            separate(reuse, 1, out, 1)?;
            if hrir_length != 0 {
                return Err(Error::InvalidArgument(
                    "Room switch cannot replace its HRIR binding",
                ));
            }
            (&*reuse).prepared.room.hrir.data.clone()
        } else if hrir_length == 0 && sofa_length != 0 {
            let bytes = bounded_read(&path(sofa_name, sofa_length)?, None, 256 * 1024 * 1024)?;
            native_hrir(&bytes, profile.profile.sample_rate)?
        } else {
            let hrir_path = if hrir_length == 0 {
                profile.root.join(&profile.profile.default_hrir)
            } else {
                path(hrir_name, hrir_length)?
            };
            load_hrir(&hrir_path, profile.profile.sample_rate)?
        };
        if sofa_length != 0 {
            let sofa_path = path(sofa_name, sofa_length)?;
            let bytes = bounded_read(&sofa_path, None, 256 * 1024 * 1024)?;
            if data.source_sofa_sha256.as_deref() != Some(sha256(&bytes).as_str()) {
                return Err(Error::InvalidArgument(
                    "所选 HRIR 包与 SOFA 不匹配；请清除 HRIR 覆盖以直接使用 SOFA",
                ));
            }
        }
        let (length, _, _) = PreparedRoom::required_fft(&profile.profile, &data)?;
        type Cache = std::collections::BTreeMap<
            (String, String, Option<String>, usize, Quality),
            std::sync::Weak<PreparedSet>,
        >;
        static CACHE: std::sync::OnceLock<std::sync::Mutex<Cache>> = std::sync::OnceLock::new();
        // Preparation-only weak cache. It shares immutable data between Scene
        // and file sessions without keeping inactive rooms alive or touching DSP locks.
        let key = (
            profile.profile.identity.clone(),
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
        let prepared = if let Some(prepared) = existing {
            prepared
        } else {
            let bank = if !reuse.is_null()
                && (&*reuse).prepared.room.hrir.fft_length == length
                && (&*reuse).prepared.room.quality == quality
            {
                (&*reuse).prepared.room.hrir.clone()
            } else {
                Arc::new(HrirBank::with_policy(data, length, quality.fft())?)
            };
            let room = Arc::new(PreparedRoom::with_quality(
                profile.profile.clone(),
                bank,
                quality,
            )?);
            let prepared = Arc::new(PreparedSet::new(room)?);
            let mut entries = cache
                .lock()
                .map_err(|_| Error::RenderFailed("Room preparation cache failed"))?;
            if let Some(existing) = entries.get(&key).and_then(std::sync::Weak::upgrade) {
                existing
            } else {
                entries.insert(key, Arc::downgrade(&prepared));
                prepared
            }
        };
        let json = description(&prepared, metres)?;
        *out = Box::into_raw(Box::new(RoomPreparedHandle { prepared, json }));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_prepared_destroy(handle: *mut RoomPreparedHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_prepared_json(handle: *const RoomPreparedHandle) -> *const u8 {
    if handle.is_null() {
        ptr::null()
    } else {
        unsafe { (*handle).json.as_ptr().cast() }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_prepared_info(
    handle: *const RoomPreparedHandle,
    out: *mut RoomPreparedInfo,
) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        separate(handle, 1, out, 1)?;
        let p = &(*handle).prepared;
        *out = RoomPreparedInfo {
            latency_frames: mradm_dsp::room::BLOCK as u64,
            tail_frames: p.tail_frames() as u64,
            preroll_frames: p.preroll_frames() as u64,
            material_guard_frames: p.room.profile.band_offset as u64,
            sample_rate: p.room.profile.sample_rate,
            reflection_update_frames: p
                .room
                .quality
                .reflection_frames_at(p.room.profile.sample_rate)
                as u32,
        };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_create(
    prepared: *const RoomPreparedHandle,
    metres: f64,
    scene: u32,
    spread: u32,
    out: *mut *mut Session,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(prepared, 1, out, 1)?;
        *out = ptr::null_mut();
        let session = Session::new(
            (*prepared).prepared.clone(),
            metres,
            flag(scene)?,
            flag(spread)?,
        )?;
        *out = Box::into_raw(Box::new(session));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_destroy(handle: *mut Session) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_configure(
    handle: *mut Session,
    descriptions: *const RoomDescription,
    count: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(handle, 1, descriptions, count)?;
        (*handle).configure(input(descriptions, count)?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_reset(
    handle: *mut Session,
    sample: u64,
    clear: u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        range(handle, 1)?;
        let clear = flag(clear)?;
        (*handle).reset(sample)?;
        if clear {
            (*handle).clear_generation();
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_prime(
    handle: *mut Session,
    command: *const RoomCommand,
    sample: u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(handle, 1, command, 1)?;
        (*handle).prime(&*command, sample)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_pose(
    handle: *mut Session,
    pose: *const f32,
    length: usize,
) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        separate(handle, 1, pose, length)?;
        if length != 3 {
            return Err(invalid());
        }
        let pose = input(pose, length)?;
        (*handle).set_pose([pose[0], pose[1], pose[2]])
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_end(handle: *mut Session) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        range(handle, 1)?;
        (*handle).end_input();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_info(
    handle: *const Session,
    out: *mut RoomInfo,
) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        separate(handle, 1, out, 1)?;
        *out = (*handle).info();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_source(
    handle: *const Session,
    id: u64,
    out: *mut RoomSourceInfo,
) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        separate(handle, 1, out, 1)?;
        *out = RoomSourceInfo {
            id,
            ..RoomSourceInfo::default()
        };
        if let Some(Position {
            point,
            direction,
            distance,
            constrained,
        }) = (*handle).source_position(id)
        {
            *out = RoomSourceInfo {
                id,
                position: point,
                direction,
                distance,
                constrained: u32::from(constrained),
                present: 1,
            };
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_process(
    handle: *mut Session,
    frames: usize,
    planes: *const RoomPlane,
    count: usize,
    initial: *const RoomCommand,
    initial_count: usize,
    events: *const RoomCommand,
    event_count: usize,
    pcm: *mut f32,
    pcm_length: usize,
    info: *mut RoomInfo,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(handle, 1, planes, count)?;
        separate(handle, 1, initial, initial_count)?;
        separate(handle, 1, events, event_count)?;
        separate(handle, 1, pcm, pcm_length)?;
        separate(handle, 1, info, 1)?;
        separate(pcm, pcm_length, planes, count)?;
        separate(pcm, pcm_length, initial, initial_count)?;
        separate(pcm, pcm_length, events, event_count)?;
        separate(pcm, pcm_length, info, 1)?;
        separate(info, 1, planes, count)?;
        separate(info, 1, initial, initial_count)?;
        separate(info, 1, events, event_count)?;
        let planes = input(planes, count)?;
        for plane in planes {
            flag(plane.has_signal)?;
            if plane.reserved != 0 {
                return Err(invalid());
            }
            if plane.has_signal != 0 {
                separate(plane.samples, plane.length, handle, 1)?;
                separate(plane.samples, plane.length, pcm, pcm_length)?;
                separate(plane.samples, plane.length, info, 1)?;
            }
        }
        let views = planes.iter().map(|p| {
            if p.has_signal == 0 {
                None
            } else {
                Some(if p.length == 0 {
                    &[][..]
                } else {
                    std::slice::from_raw_parts(p.samples, p.length)
                })
            }
        });
        *info = (*handle).process(
            frames,
            views,
            input(initial, initial_count)?,
            input(events, event_count)?,
            output(pcm, pcm_length)?,
        )?;
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_instance_create(
    prepared: *const RoomPreparedHandle,
    source_capacity: usize,
    out: *mut *mut Instance,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(prepared, 1, out, 1)?;
        *out = ptr::null_mut();
        *out = Box::into_raw(Box::new(Instance::new(
            (*prepared).prepared.clone(),
            source_capacity,
        )?));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_instance_destroy(handle: *mut Instance) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_instance_reserve(
    handle: *mut Instance,
    source_capacity: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        range(handle, 1)?;
        (*handle).reserve(source_capacity)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_request(handle: *mut Session, request: u64) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        range(handle, 1)?;
        (*handle).note_room_request(request)
    })
}
/// Consumes `next` after argument validation, including when queueing fails.
/// Returns up to two displaced instances; caller destroys them off the worker.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_queue(
    handle: *mut Session,
    next: *mut Instance,
    request: u64,
    displaced: *mut *mut Instance,
    count: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(handle, 1, next, 1)?;
        separate(handle, 1, displaced, count)?;
        separate(next, 1, displaced, count)?;
        if count != 2 {
            return Err(invalid());
        }
        let out = output(displaced, count)?;
        out.fill(ptr::null_mut());
        if let Err(error) = (*handle).validate_room(&*next, request) {
            out[0] = next; // return ownership for disposal off the render worker
            return Err(error);
        }
        let result = (*handle).queue_room(Box::from_raw(next), request)?;
        for (out, item) in out.iter_mut().zip(result) {
            *out = item.map_or(ptr::null_mut(), Box::into_raw);
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_room_session_retired(
    handle: *mut Session,
    out: *mut *mut Instance,
) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        separate(handle, 1, out, 1)?;
        *out = (*handle)
            .take_retired()
            .map_or(ptr::null_mut(), Box::into_raw);
        Ok(())
    })
}

#[cfg(test)]
mod native_sofa_tests {
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
        let bytes = include_bytes!("../../../../tests/fixtures/sofa/simple.sofa");
        let first = super::native_hrir(bytes, 48000).unwrap();
        let second = super::native_hrir(bytes, 48000).unwrap();
        assert!(std::sync::Arc::ptr_eq(&first, &second));
        // Exercise the same panic boundary used by room_prepare. Some HDF5
        // errors unwind in debug builds; none may disable later SOFA imports.
        let mut message = [0; 128];
        let status = super::boundary(message.as_mut_ptr(), message.len(), || {
            super::native_hrir(&bytes[..bytes.len() - 1], 48000).map(|_| ())
        });
        assert_ne!(status, 0);
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
                        include_bytes!("../../../../tests/fixtures/sofa/room-delayed.sofa"),
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
