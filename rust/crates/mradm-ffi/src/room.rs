//! Private room boundary. Loading/preparation may do I/O; session processing
//! only consumes checked PCM/control views. Handles own their data independently.
use crate::checked::*;
use mradm_dsp::{
    Error, Result,
    room::{
        loading::{self, Package},
        quality::Quality,
        screen::{Mapping, RoomScreens},
        session::PreparedSet,
    },
};
use mradm_render::room_runtime::{Config, Fault, Resources};
use std::{
    ffi::CString,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr,
    sync::Arc,
};

fn boundary(message: *mut u8, capacity: usize, call: impl FnOnce() -> Result<()>) -> i32 {
    #[cfg(windows)]
    mradm_dsp::room::set_worker_start_hook(crate::room_environment::start_worker);
    let _environment = crate::room_environment::Environment::enter();
    crate::boundary(message, capacity, call)
}

/// The room boundary of one call the Rust room runtime makes (formerly one C++ FFI call): the
/// room floating-point environment, and panics as `(6, "Rust DSP panic")`.
pub(crate) fn scope(call: &mut dyn FnMut() -> Result<()>) -> std::result::Result<(), Fault> {
    #[cfg(windows)]
    mradm_dsp::room::set_worker_start_hook(crate::room_environment::start_worker);
    let _environment = crate::room_environment::Environment::enter();
    match catch_unwind(AssertUnwindSafe(call)) {
        Ok(Ok(())) => Ok(()),
        Ok(Err(Error::InvalidArgument(s))) => Err((1, s)),
        Ok(Err(Error::Unsupported(s))) => Err((2, s)),
        Ok(Err(Error::Io(s))) => Err((3, s)),
        Ok(Err(Error::RenderFailed(s))) => Err((4, s)),
        Err(_) => Err((6, "Rust DSP panic")),
    }
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

pub struct RoomProfileHandle {
    package: Arc<Package>,
    json: CString,
}
pub struct RoomPreparedHandle {
    prepared: Arc<PreparedSet>,
    json: CString,
    cuda: bool,
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct RoomSourceInfo {
    pub(crate) id: u64,
    pub(crate) position: [f64; 3],
    pub(crate) direction: [f64; 3],
    pub(crate) distance: f64,
    pub(crate) constrained: u32,
    pub(crate) present: u32,
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
        output(out, 3)?.copy_from_slice(&loading::polar(azimuth, elevation, distance)?);
        Ok(())
    })
}

fn description(
    prepared: &PreparedSet,
    metres: f64,
    cuda: Option<serde_json::Value>,
) -> Result<CString> {
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
    if let Some(cuda) = cuda {
        value["execution_backend"] = serde_json::json!("cuda");
        value["cuda"] = cuda;
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
        let package = Arc::new(loading::load_package(input(name, length)?)?);
        let json = CString::new(package.manifest.clone()).map_err(|_| invalid())?;
        *out = Box::into_raw(Box::new(RoomProfileHandle { package, json }));
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
        if !reuse.is_null() {
            separate(reuse, 1, out, 1)?;
        }
        let (prepared, cuda, metadata) = prepare_set(
            &(*profile).package,
            input(hrir_name, hrir_length)?,
            input(sofa_name, sofa_length)?,
            (!reuse.is_null()).then(|| (&*(&*reuse).prepared, (&*reuse).cuda)),
            quality,
            metres,
        )?;
        let json = description(&prepared, metres, metadata)?;
        *out = Box::into_raw(Box::new(RoomPreparedHandle {
            prepared,
            json,
            cuda,
        }));
        Ok(())
    })
}
/// `mradm_room_prepare_quality` after argument checks: the quality and scale, the CUDA selection,
/// the room set and its CUDA binding. A room switch prepares through the same path.
fn prepare_set(
    package: &Package,
    hrir: &[u8],
    sofa: &[u8],
    reuse: Option<(&PreparedSet, bool)>,
    quality: u32,
    metres: f64,
) -> Result<(Arc<PreparedSet>, bool, Option<serde_json::Value>)> {
    let quality = Quality::try_from(quality)?;
    if !mradm_dsp::room::geometry::valid_scale(metres) {
        return Err(Error::InvalidArgument("Invalid room distance scale"));
    }
    let cuda = crate::room_cuda_selection::selected(quality, reuse.map(|r| r.1))?;
    let prepared = loading::prepare(package, hrir, sofa, reuse.map(|r| r.0), quality)?;
    let metadata = None;
    #[cfg(all(windows, feature = "room-cuda"))]
    let (prepared, metadata) = if cuda {
        let (value, metadata) = mradm_room_cuda::bind_realtime(prepared, None)?;
        (value, Some(metadata))
    } else {
        (prepared, metadata)
    };
    Ok((prepared, cuda, metadata))
}

/// A room switch's candidate (`room_internal::prepare` with the pinned HRIR and no SOFA).
pub(crate) fn prepare_switch(
    config: &Config,
    reuse: &Resources,
) -> std::result::Result<Resources, Fault> {
    let mut out = None;
    scope(&mut || {
        let (set, cuda, _) = prepare_set(
            &config.package,
            config.hrir.as_bytes(),
            b"",
            Some((&reuse.set, reuse.cuda)),
            config.quality as u32,
            config.metres,
        )?;
        out = Some(Resources { set, cuda });
        Ok(())
    })?;
    Ok(out.expect("prepared in scope"))
}

/// The prepared set behind a handle, shared with a runtime.
pub(crate) unsafe fn resources(handle: *const RoomPreparedHandle) -> Option<Arc<Resources>> {
    unsafe { handle.as_ref() }.map(|h| {
        Arc::new(Resources {
            set: Arc::clone(&h.prepared),
            cuda: h.cuda,
        })
    })
}

/// The package behind a profile handle.
pub(crate) unsafe fn package(handle: *const RoomProfileHandle) -> Option<Arc<Package>> {
    unsafe { handle.as_ref() }.map(|h| Arc::clone(&h.package))
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
