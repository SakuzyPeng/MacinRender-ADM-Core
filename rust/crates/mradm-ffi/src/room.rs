//! Private room boundary. Loading/preparation may do I/O; session processing
//! only consumes checked PCM/control views. Handles own their data independently.
use crate::checked::*;
use mradm_dsp::{
    Error, Result,
    room::{
        control::{RoomCommand, RoomDescription, RoomOverride},
        geometry::Position,
        loading::{self, Package},
        quality::Quality,
        screen::{Mapping, RoomScreens},
        session::{Instance, PreparedSet, RoomInfo, Session},
    },
};
use std::{ffi::CString, ptr, sync::Arc};

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
    package: Package,
    json: CString,
}
pub struct RoomPreparedHandle {
    prepared: Arc<PreparedSet>,
    json: CString,
    cuda: bool,
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
        let package = loading::load_package(input(name, length)?)?;
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
        let quality = Quality::try_from(quality)?;
        if !mradm_dsp::room::geometry::valid_scale(metres) {
            return Err(Error::InvalidArgument("Invalid room distance scale"));
        }
        let profile = &*profile;
        if !reuse.is_null() {
            separate(reuse, 1, out, 1)?;
        }
        let cuda = crate::room_cuda_selection::selected(
            quality,
            (!reuse.is_null()).then(|| (&*reuse).cuda),
        )?;
        if !reuse.is_null() {
            separate(reuse, 1, out, 1)?;
        }
        let prepared = loading::prepare(
            &profile.package,
            input(hrir_name, hrir_length)?,
            input(sofa_name, sofa_length)?,
            (!reuse.is_null()).then(|| &*(&*reuse).prepared),
            quality,
        )?;
        let metadata = None;
        #[cfg(all(windows, feature = "room-cuda"))]
        let (prepared, metadata) = if cuda {
            let (value, metadata) = mradm_room_cuda::bind_realtime(prepared, None)?;
            (value, Some(metadata))
        } else {
            (prepared, metadata)
        };
        let json = description(&prepared, metres, metadata)?;
        *out = Box::into_raw(Box::new(RoomPreparedHandle {
            prepared,
            json,
            cuda,
        }));
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
