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
    sync::Arc,
};

/// One room call outside the C boundary: the room floating-point environment (and on Windows the
/// worker start hook) around `call`. Panics are the caller's to catch.
pub fn in_environment<T>(call: impl FnOnce() -> Result<T>) -> Result<T> {
    #[cfg(windows)]
    mradm_dsp::room::set_worker_start_hook(crate::room_environment::start_worker);
    let _environment = crate::room_environment::Environment::enter();
    call()
}

/// The status code and message the room boundary reports for `error`.
pub fn error_code(error: &Error) -> (i32, &'static str) {
    match *error {
        Error::InvalidArgument(s) => (1, s),
        Error::Unsupported(s) => (2, s),
        Error::Io(s) => (3, s),
        Error::RenderFailed(s) => (4, s),
    }
}

/// Every screen mapping is valid (`mradm_room_validate_screens`).
pub fn validate_screens(screens: &[RoomScreens]) -> Result<()> {
    in_environment(|| {
        for value in screens {
            Mapping::new(*value)?;
        }
        Ok(())
    })
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

pub struct RoomProfileHandle {
    package: Arc<Package>,
    json: CString,
}
impl RoomProfileHandle {
    /// Loads the room package at `path` (`mradm_room_profile_load`).
    pub fn load(path: &[u8]) -> Result<Box<Self>> {
        in_environment(|| {
            let package = Arc::new(loading::load_package(path)?);
            let json = CString::new(package.manifest.clone()).map_err(|_| invalid())?;
            Ok(Box::new(Self { package, json }))
        })
    }

    /// The package manifest JSON.
    pub fn manifest(&self) -> &std::ffi::CStr {
        &self.json
    }

    /// The loaded package, shared with the handle.
    pub fn package(&self) -> Arc<Package> {
        Arc::clone(&self.package)
    }

    /// A second handle on the same loaded package (destroyed with `mradm_room_profile_destroy`);
    /// `None` when out of memory.
    pub fn share(&self) -> Option<*mut RoomProfileHandle> {
        let manifest = self.json.as_bytes_with_nul();
        let mut json = Vec::new();
        json.try_reserve_exact(manifest.len()).ok()?;
        json.extend_from_slice(manifest);
        let json = CString::from_vec_with_nul(json).ok()?;
        let layout = std::alloc::Layout::new::<RoomProfileHandle>();
        let handle = unsafe { std::alloc::alloc(layout) }.cast::<RoomProfileHandle>();
        if handle.is_null() {
            return None;
        }
        unsafe {
            handle.write(RoomProfileHandle {
                package: Arc::clone(&self.package),
                json,
            })
        };
        Some(handle)
    }
}
pub struct RoomPreparedHandle {
    prepared: Arc<PreparedSet>,
    json: CString,
    cuda: bool,
}
impl RoomPreparedHandle {
    /// Prepares the room for `profile` (`mradm_room_prepare_quality`): the HRIR manifest and SOFA
    /// paths (empty for none), an earlier preparation to reuse, the quality and the distance scale.
    pub fn prepare(
        profile: &RoomProfileHandle,
        hrir: &[u8],
        sofa: &[u8],
        reuse: Option<&RoomPreparedHandle>,
        quality: u32,
        metres: f64,
    ) -> Result<Box<Self>> {
        in_environment(|| {
            let (prepared, cuda, metadata) = prepare_set(
                &profile.package,
                hrir,
                sofa,
                reuse.map(|r| (&*r.prepared, r.cuda)),
                quality,
                metres,
            )?;
            let json = description(&prepared, metres, metadata)?;
            Ok(Box::new(Self {
                prepared,
                json,
                cuda,
            }))
        })
    }

    /// The prepared room's description JSON.
    pub fn description(&self) -> &std::ffi::CStr {
        &self.json
    }
}

pub(crate) fn description(
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

/// `mradm_room_prepare_quality` after argument checks: the quality and scale, the CUDA selection,
/// the room set and its CUDA binding. A room switch prepares through the same path.
pub(crate) fn prepare_set(
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
