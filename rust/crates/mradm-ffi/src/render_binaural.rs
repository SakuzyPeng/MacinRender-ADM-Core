//! The binaural HRTF state (PCM consistency 4.7 A): loading, preparation and the Scene stream
//! cache run in Rust `mradm-render::binaural_hrtf`; the C++ renderers borrow its view.
use super::*;
use mradm_dsp::hrtf_filters::Filters;
use mradm_dsp::scene_math::Rotation;
use mradm_render::binaural::{Scales, Spread};
use mradm_render::binaural_hrtf::{self, Sofa, State};
use mradm_render::binaural_render::{self as binaural, Bank, Offline, Pending, Settings};
use std::collections::BTreeMap;
use std::path::Path;
use std::sync::Arc;

/// A prepared HRTF state.
pub struct BinauralHrtfHandle {
    state: Arc<State>,
}

#[repr(C)]
pub struct RenderBinauralHrtfView {
    name: Text,
    sample_rate: u32,
    num_dirs: usize,
    hrir_len: usize,
    fft_size: usize,
    overlap: usize,
    /// An owned filter-bank handle for `mradm_dsp_hrtf_*` (destroy with
    /// `mradm_dsp_hrtf_filters_destroy`).
    filters: *mut Arc<Filters>,
    hrirs: *const f32,
    hrirs_len: usize,
    directions: *const f32,
    directions_len: usize,
}

#[cfg(unix)]
fn native(bytes: &[u8]) -> Option<&Path> {
    use std::os::unix::ffi::OsStrExt;
    Some(Path::new(std::ffi::OsStr::from_bytes(bytes)))
}
#[cfg(not(unix))]
fn native(bytes: &[u8]) -> Option<&Path> {
    std::str::from_utf8(bytes).ok().map(Path::new)
}

unsafe fn prepare(
    sofa: u32,
    path: Text,
    display: Text,
    file_name: Text,
    output: *mut *mut BinauralHrtfHandle,
    view: *mut RenderBinauralHrtfView,
    make: impl FnOnce(Option<&Sofa>) -> Result<Arc<State>, Error>,
) -> Result<(), Error> {
    let (Some(out), Some(view)) = (unsafe { output.as_mut() }, unsafe { view.as_mut() }) else {
        return Err(bad("无效 HRTF 参数"));
    };
    *out = ptr::null_mut();
    let state = if sofa != 0 {
        let (path, display, file_name) =
            unsafe { (bytes(&path)?, bytes(&display)?, bytes(&file_name)?) };
        let path = native(path).ok_or_else(|| Error {
            code: 3,
            message: b"SOFA file could not be opened".to_vec(),
            context: [b"path=".as_slice(), display].concat(),
        })?;
        make(Some(&Sofa {
            path,
            display,
            file_name,
        }))?
    } else {
        make(None)?
    };
    *view = RenderBinauralHrtfView {
        name: Text::new(&state.name),
        sample_rate: state.sample_rate,
        num_dirs: state.num_dirs,
        hrir_len: state.hrir_len,
        fft_size: state.fft_size,
        overlap: state.overlap,
        filters: Box::into_raw(Box::new(Arc::clone(&state.filters))),
        hrirs: state.hrirs.as_ptr(),
        hrirs_len: state.hrirs.len(),
        directions: state.directions.as_ptr(),
        directions_len: state.directions.len(),
    };
    *out = Box::into_raw(Box::new(BinauralHrtfHandle { state }));
    Ok(())
}

/// The file renderer's HRTF state: the built-in set (`sofa` 0) or a SOFA file at `input_rate`,
/// prepared for `block`-frame OLA blocks. The view lives as long as the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_hrtf_prepare(
    sofa: u32,
    path: Text,
    display: Text,
    file_name: Text,
    input_rate: u32,
    block: u64,
    output: *mut *mut BinauralHrtfHandle,
    view: *mut RenderBinauralHrtfView,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || unsafe {
        prepare(sofa, path, display, file_name, output, view, |sofa| {
            let dataset = binaural_hrtf::load(sofa, input_rate)?;
            binaural_hrtf::build(dataset, block, false).map(Arc::new)
        })
    })
}

/// The Scene stream renderer's cached HRTF state at `sample_rate` (without impulses).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_hrtf_live(
    sofa: u32,
    path: Text,
    display: Text,
    file_name: Text,
    sample_rate: u32,
    output: *mut *mut BinauralHrtfHandle,
    view: *mut RenderBinauralHrtfView,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || unsafe {
        prepare(sofa, path, display, file_name, output, view, |sofa| {
            binaural_hrtf::prepare_live(sofa, sample_rate)
        })
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_hrtf_destroy(handle: *mut BinauralHrtfHandle) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

// ── Source plan and offline rendering (4.7 B) ─────────────────────────────────

#[repr(C)]
pub struct RenderBinauralRequest {
    /// 0 cloud (automatic), 1 none, 2 SAF spreader.
    spread: u32,
    smoothing_frames: u32,
}

/// The plan's sizes beside the common preparation view.
#[repr(C)]
pub struct RenderBinauralInfo {
    sources: usize,
    groups: usize,
    block: u64,
}

fn spread_of(value: u32) -> Spread {
    match value {
        1 => Spread::None,
        2 => Spread::SafSpreader,
        _ => Spread::Cloud,
    }
}

/// Plans the sources (and SAF spreader groups) of the scene over a prepared HRTF state; the view
/// carries the plan's log lines (after the caller's HRTF source line) and its error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_prepare(
    scene: *const SceneHandle,
    hrtf: *const BinauralHrtfHandle,
    request: *const RenderBinauralRequest,
    output: *mut *mut RenderPrepared,
    view: *mut RenderPreparedView,
    info: *mut RenderBinauralInfo,
) -> i32 {
    boundary(
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let (Some(out), Some(view), Some(scene), Some(hrtf), Some(r), Some(info)) = (
                unsafe { output.as_mut() },
                unsafe { view.as_mut() },
                unsafe { scene.as_ref() },
                unsafe { hrtf.as_ref() },
                unsafe { request.as_ref() },
                unsafe { info.as_mut() },
            ) else {
                return Err(bad("无效双耳准备参数"));
            };
            *out = ptr::null_mut();
            let scene = scene.scene();
            let mut logs = Vec::new();
            let result = binaural::prepare(
                scene,
                Arc::clone(&hrtf.state),
                spread_of(r.spread),
                r.smoothing_frames,
                &mut |level, text| logs.push((level, text)),
            );
            let settings = (
                usize::from(scene.info.channels),
                2,
                scene.info.frames,
                scene.info.sample_rate,
            );
            let (result, data) = match result {
                Ok(p) => (Ok(None), Some(p)),
                Err(e) => (Err(e), None),
            };
            let status = prepared(logs, result, settings, out, view);
            if let Some(p) = data {
                *info = RenderBinauralInfo {
                    sources: p.sources.len(),
                    groups: p.groups.len(),
                    block: p.block,
                };
                unsafe { &mut **out }.binaural = Some(Arc::new(p));
            }
            status
        },
    )
}

unsafe fn binaural_of<'a>(prepared: *const RenderPrepared) -> Option<&'a Arc<binaural::Prepared>> {
    unsafe { prepared.as_ref() }.and_then(|p| p.binaural.as_ref())
}

#[repr(C)]
pub struct RenderBinauralOfflineSettings {
    inputs: usize,
    frames: u64,
    sample_rate: u32,
    has_window: u32,
    window_start: u64,
    window_count: u64,
    smoothing_frames: u32,
    has_pose: u32,
    pose: [f32; 3],
}

/// An offline binaural render in progress.
pub struct RenderBinauralOffline {
    driver: Option<Offline>,
    pools: Vec<u8>,
}

/// One step: `pending` 1 for an input block, 2 for a spreader tail block; `emit` interleaved
/// frames at `output`, valid until the next call.
#[repr(C)]
pub struct RenderBinauralStep {
    pending: u32,
    emit: usize,
    output: *const f32,
    has_progress: u32,
    progress: RenderOfflineProgress,
}

fn offline_error(message: &str) -> offline::Error {
    offline::Error {
        code: 1,
        message: message.to_owned(),
        location: offline::Location::None,
    }
}

/// Opens the input (UTF-8 path) for a binaural preparation; `pools` receives the diagnostic
/// build's worker log line (empty otherwise), valid while the handle lives.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_offline_open(
    prepared: *const RenderPrepared,
    path: *const u8,
    path_len: usize,
    settings: *const RenderBinauralOfflineSettings,
    output: *mut *mut RenderBinauralOffline,
    pools: *mut Text,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let (Some(out), Some(s), Some(plan), Some(pools)) = (
            unsafe { output.as_mut() },
            unsafe { settings.as_ref() },
            unsafe { binaural_of(prepared) },
            unsafe { pools.as_mut() },
        ) else {
            return Err(offline_error("无效双耳离线参数"));
        };
        *out = ptr::null_mut();
        let path = unsafe { crate::wav::path(path, path_len) }.map_err(|e| offline::Error {
            code: e.code,
            message: e.message,
            location: offline::Location::Path,
        })?;
        let reader = Offline::open_reader(path)?;
        let driver = Offline::new(
            Arc::clone(plan),
            reader,
            Settings {
                inputs: s.inputs,
                frames: s.frames,
                sample_rate: s.sample_rate,
                window: (s.has_window != 0).then_some(offline::Window {
                    start: s.window_start,
                    count: s.window_count,
                }),
                smoothing_frames: s.smoothing_frames,
                pose: (s.has_pose != 0).then_some(s.pose),
            },
        )?;
        let handle = Box::new(RenderBinauralOffline {
            pools: driver.diagnostic_pools().unwrap_or("").as_bytes().to_vec(),
            driver: Some(driver),
        });
        *pools = Text::bytes(&handle.pools);
        *out = Box::into_raw(handle);
        Ok(())
    })
}

unsafe fn offline_driver<'a>(
    handle: *mut RenderBinauralOffline,
) -> offline::Result<&'a mut Offline> {
    unsafe { handle.as_mut() }
        .and_then(|h| h.driver.as_mut())
        .ok_or_else(|| offline_error("无效双耳离线句柄"))
}

/// What the next step renders: 0 nothing (finished), 1 an input block, 2 a spreader tail block.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_offline_pending(
    handle: *const RenderBinauralOffline,
) -> u32 {
    match unsafe { handle.as_ref() }
        .and_then(|h| h.driver.as_ref())
        .map(Offline::pending)
    {
        Some(Pending::Block) => 1,
        Some(Pending::Tail) => 2,
        _ => 0,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_offline_next(
    handle: *mut RenderBinauralOffline,
    step: *mut RenderBinauralStep,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let step = unsafe { step.as_mut() }.ok_or_else(|| offline_error("无效双耳离线参数"))?;
        let driver = unsafe { offline_driver(handle) }?;
        *step = RenderBinauralStep {
            pending: 0,
            emit: 0,
            output: ptr::null(),
            has_progress: 0,
            progress: RenderOfflineProgress {
                fraction: 0.,
                stage_fraction: 0.,
                done: 0,
                total: 0,
            },
        };
        if let Some(s) = driver.next_step()? {
            step.pending = if s.tail { 2 } else { 1 };
            step.emit = s.emit;
            step.output = driver.output(s.emit).as_ptr();
            if let Some(p) = s.progress {
                step.has_progress = 1;
                step.progress = RenderOfflineProgress {
                    fraction: p.fraction,
                    stage_fraction: p.stage_fraction,
                    done: p.done,
                    total: p.total,
                };
            }
        }
        Ok(())
    })
}

/// The written frame count and the loudness and true peak (also of a silent render: -200 dB).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_offline_finish(
    handle: *mut RenderBinauralOffline,
    written: *mut u64,
    metrics: *mut RenderOfflineMetrics,
) -> i32 {
    let (Some(h), Some(written), Some(metrics)) = (
        unsafe { handle.as_mut() },
        unsafe { written.as_mut() },
        unsafe { metrics.as_mut() },
    ) else {
        return 1;
    };
    let Some(driver) = h.driver.take() else {
        return 1;
    };
    let (frames, m) = driver.finish();
    *written = frames;
    *metrics = RenderOfflineMetrics {
        has_lufs: u32::from(m.lufs.is_some()),
        has_peak: u32::from(m.peak_dbtp.is_some()),
        lufs: m.lufs.unwrap_or(0.),
        peak_dbtp: m.peak_dbtp.unwrap_or(0.),
    };
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_offline_destroy(handle: *mut RenderBinauralOffline) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

// ── Transitional source bank for the C++ file stream (removed with 4.7 C) ─────

#[repr(C)]
pub struct RenderBinauralScales {
    object_id: Text,
    scales: [f32; 5],
}

#[repr(C)]
pub struct RenderBinauralSourceKey {
    object_id: Text,
    label_key: Text,
}

#[repr(C)]
pub struct RenderBinauralSourceOutput {
    left: *const f32,
    right: *const f32,
}

/// A file stream's source list, reusable OLA state and persistent worker pool.
pub struct BinauralBank {
    prepared: Arc<binaural::Prepared>,
    sources: Vec<mradm_render::binaural::Source>,
    keys: Vec<RenderBinauralSourceKey>,
    outputs: Vec<RenderBinauralSourceOutput>,
    bank: Bank,
}

impl BinauralBank {
    fn bind_sources(&mut self, sources: Vec<mradm_render::binaural::Source>) {
        self.sources = sources;
        self.keys = self
            .sources
            .iter()
            .map(|s| RenderBinauralSourceKey {
                object_id: Text::bytes(&s.object_id),
                label_key: Text::bytes(&s.label_key),
            })
            .collect();
        self.outputs = self
            .bank
            .states
            .iter()
            .map(|s| RenderBinauralSourceOutput {
                left: s.left.as_ptr(),
                right: s.right.as_ptr(),
            })
            .collect();
    }
}

/// The sources of the preparation's scene with each listed object's blocks scaled (none: the
/// prepared list), with worker threads for the file stream (one per core when there are sources
/// and several cores). `*output` must be null to create a stream, or its existing bank to rebuild
/// it in place. A failed rebuild preserves its state and views; successful rebuilds invalidate
/// the previous keys/outputs. The preparation and worker pool stay fixed for the stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_bank_create(
    prepared: *const RenderPrepared,
    scales: *const RenderBinauralScales,
    scales_len: usize,
    output: *mut *mut BinauralBank,
    keys: *mut *const RenderBinauralSourceKey,
    keys_len: *mut usize,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let (Some(out), Some(plan), Some(keys), Some(keys_len)) = (
            unsafe { output.as_mut() },
            unsafe { binaural_of(prepared) },
            unsafe { keys.as_mut() },
            unsafe { keys_len.as_mut() },
        ) else {
            return Err(offline_error("无效双耳源参数"));
        };
        let existing = unsafe { (*out).as_mut() };
        if existing
            .as_ref()
            .is_some_and(|h| !Arc::ptr_eq(&h.prepared, plan))
        {
            return Err(offline_error("无效双耳源参数"));
        }
        let list = if scales_len == 0 {
            &[][..]
        } else if scales.is_null() {
            return Err(offline_error("无效双耳源参数"));
        } else {
            unsafe { slice::from_raw_parts(scales, scales_len) }
        };
        let mut map: BTreeMap<Vec<u8>, Scales> = BTreeMap::new();
        for s in list {
            let id = unsafe { bytes(&s.object_id) }.map_err(|_| offline_error("无效双耳源参数"))?;
            map.insert(id.to_vec(), s.scales);
        }
        let sources = if map.is_empty() {
            plan.sources.clone()
        } else {
            mradm_render::binaural::sources(&plan.input, plan.spread, &map, &mut |_, _| {})
        };
        if let Some(handle) = existing {
            handle.bank.rebuild(sources.len())?;
            handle.bind_sources(sources);
            *keys = handle.keys.as_ptr();
            *keys_len = handle.keys.len();
        } else {
            let hardware = std::thread::available_parallelism().map_or(1, usize::from);
            let threads = if !plan.sources.is_empty() && hardware > 1 {
                hardware
            } else {
                0
            };
            let bank = Bank::new(&plan.hrtf, sources.len(), plan.block as usize, threads)?;
            let mut handle = Box::new(BinauralBank {
                prepared: Arc::clone(plan),
                sources: Vec::new(),
                keys: Vec::new(),
                outputs: Vec::new(),
                bank,
            });
            handle.bind_sources(sources);
            *keys = handle.keys.as_ptr();
            *keys_len = handle.keys.len();
            *out = Box::into_raw(handle);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_bank_reset(handle: *mut BinauralBank) {
    if let Some(h) = unsafe { handle.as_mut() } {
        h.bank.reset();
    }
}

/// Renders the chunk `[start, start + frames)` of every source; `head_locked` holds each source's
/// live value (-1 none, 0 or 1). The pose (yaw, pitch, roll) applies when `has_pose`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_bank_render(
    handle: *mut BinauralBank,
    input: *const f32,
    input_len: usize,
    channels: usize,
    start: u64,
    frames: usize,
    smoothing_frames: u32,
    has_pose: u32,
    pose: *const f32,
    head_locked: *const i32,
    head_locked_len: usize,
    outputs: *mut *const RenderBinauralSourceOutput,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let (Some(h), Some(outputs)) = (unsafe { handle.as_mut() }, unsafe { outputs.as_mut() })
        else {
            return Err(offline_error("无效双耳源参数"));
        };
        if input.is_null()
            || input_len < channels * frames
            || frames > h.prepared.block as usize
            || head_locked_len != h.sources.len()
            || (head_locked_len != 0 && head_locked.is_null())
            || (has_pose != 0 && pose.is_null())
        {
            return Err(offline_error("无效双耳源参数"));
        }
        let input = unsafe { slice::from_raw_parts(input, input_len) };
        let locked: &[i32] = if head_locked_len == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(head_locked, head_locked_len) }
        };
        let rotation = if has_pose != 0 {
            let p = unsafe { slice::from_raw_parts(pose, 3) };
            Some(
                Rotation::new_compat([p[0], p[1], p[2]], false).map_err(|e| offline::Error {
                    code: 1,
                    message: e.to_string(),
                    location: offline::Location::None,
                })?,
            )
        } else {
            None
        };
        h.bank.render(
            &h.sources,
            &h.prepared.hrtf.filters,
            input,
            channels,
            start,
            frames,
            smoothing_frames,
            rotation.as_ref(),
            &|i| match locked[i] {
                0 => Some(false),
                1 => Some(true),
                _ => None,
            },
        )?;
        *outputs = h.outputs.as_ptr();
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_binaural_bank_destroy(handle: *mut BinauralBank) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

#[cfg(test)]
#[path = "render_binaural_test.rs"]
mod tests;
