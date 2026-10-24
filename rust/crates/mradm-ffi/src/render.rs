//! Private boundary of `mradm-render` (PCM consistency 4.5): the shared DirectSpeakers, label and
//! LFE routing rules, the offline loudspeaker driver the caller steps block by block, and the VBAP
//! backend with its file stream. Inputs are borrowed for the call; a resolved matrix is owned by its handle
//! and its view lives until `mradm_render_matrix_destroy`. Errors fill the caller's message and
//! context buffers (capacity in, length written out, NUL-terminated) with the C++ bytes.
use crate::adm::Text;
use crate::scene::SceneHandle;
use mradm_render::routing::{self, Error, LfeTarget, MatrixRoute, Override, ResolvedRoute, Target};
use mradm_render::{
    ear, hoa, live_vbap, offline, stream, triple_balance, triple_balance_stream, vbap,
};
use std::ffi::c_void;
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
    sync::Arc,
};

#[repr(C)]
pub struct RenderTarget {
    label: Text,
    is_lfe: u32,
}

#[repr(C)]
pub struct RenderMatrixTarget {
    label: Text,
    gain: f32,
}

#[repr(C)]
pub struct RenderMatrixRoute {
    source_label: Text,
    mute: u32,
    targets: *const RenderMatrixTarget,
    targets_len: usize,
}

#[repr(C)]
pub struct RenderResolvedTarget {
    output: usize,
    gain: f32,
}

#[repr(C)]
pub struct RenderResolvedRoute {
    source_label: Text,
    source_key: Text,
    mute: u32,
    targets: *const RenderResolvedTarget,
    targets_len: usize,
}

#[repr(C)]
pub struct RenderMatrixView {
    routes: *const RenderResolvedRoute,
    routes_len: usize,
}

#[repr(C)]
pub struct RenderOverride {
    object_id: Text,
    speaker_label: Text,
    mute: u32,
    gain_db: f32,
    has_head_locked: u32,
    head_locked: u32,
}

/// level: 1 info, 2 warning (`adm::LogLevel`); the message is static text.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RenderLogLine {
    level: u32,
    message: Text,
}

#[repr(C)]
pub struct RenderLfeRouting {
    applies_to_22_2: u32,
    split_power: u32,
    has_lfe1: u32,
    has_lfe2: u32,
    logs: [RenderLogLine; 2],
    logs_len: usize,
}

/// A resolved DirectSpeakers matrix with the records its view points into.
pub struct RenderMatrix {
    routes: Vec<ResolvedRoute>,
    _targets: Vec<Vec<RenderResolvedTarget>>,
    records: Vec<RenderResolvedRoute>,
}

fn bad(message: &str) -> Error {
    Error {
        code: 1,
        message: message.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

unsafe fn input<'a, T>(data: *const T, len: usize) -> Result<&'a [T], Error> {
    if len == 0 {
        return Ok(&[]);
    }
    if data.is_null()
        || !(data as usize).is_multiple_of(align_of::<T>())
        || len > isize::MAX as usize / size_of::<T>()
    {
        return Err(bad("无效渲染缓冲区"));
    }
    Ok(unsafe { slice::from_raw_parts(data, len) })
}

unsafe fn bytes<'a>(text: &Text) -> Result<&'a [u8], Error> {
    unsafe { input(text.data, text.len) }
}

unsafe fn texts<'a>(data: *const Text, len: usize) -> Result<Vec<&'a [u8]>, Error> {
    unsafe { input(data, len)? }
        .iter()
        .map(|t| unsafe { bytes(t) })
        .collect()
}

unsafe fn targets<'a>(data: *const RenderTarget, len: usize) -> Result<Vec<Target<'a>>, Error> {
    unsafe { input(data, len)? }
        .iter()
        .map(|t| {
            Ok(Target {
                label: unsafe { bytes(&t.label)? },
                is_lfe: t.is_lfe != 0,
            })
        })
        .collect()
}

/// Copies `detail` into `out` (capacity `*len`), NUL-terminated, and stores the length written.
unsafe fn write(detail: &[u8], out: *mut u8, len: *mut usize) {
    if len.is_null() {
        return;
    }
    let capacity = unsafe { *len };
    let written = if out.is_null() || capacity == 0 {
        0
    } else {
        let n = detail.len().min(capacity - 1);
        unsafe {
            ptr::copy_nonoverlapping(detail.as_ptr(), out, n);
            *out.add(n) = 0;
        }
        n
    };
    unsafe {
        *len = written;
    }
}

fn boundary(
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
    f: impl FnOnce() -> Result<(), Error>,
) -> i32 {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some(e),
        Err(_) => Some(Error {
            code: 6,
            message: b"Rust render panic".to_vec(),
            context: Vec::new(),
        }),
    };
    let (detail, location) = error
        .as_ref()
        .map_or((&[][..], &[][..]), |e| (&e.message[..], &e.context[..]));
    unsafe {
        write(detail, message, message_len);
        write(location, context, context_len);
    }
    error.map_or(0, |e| e.code)
}

/// Writes a label rule's result (kind 0 key, 1 canonical, 2 direct) into `output`, whose capacity
/// `*output_len` must be at least `raw_len + 8`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_label(
    kind: u32,
    raw: *const u8,
    raw_len: usize,
    output: *mut u8,
    output_len: *mut usize,
) -> i32 {
    boundary(
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let raw = unsafe { input(raw, raw_len)? };
            let result = match kind {
                0 => routing::label_key(raw),
                1 => routing::canonical_label(raw),
                2 => routing::direct_label(raw),
                _ => return Err(bad("无效标签规则")),
            };
            if output.is_null() || output_len.is_null() || unsafe { *output_len } <= result.len() {
                return Err(bad("标签输出缓冲区不足"));
            }
            unsafe { write(&result, output, output_len) };
            Ok(())
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_is_lfe(raw: *const u8, raw_len: usize) -> u32 {
    catch_unwind(|| unsafe { input(raw, raw_len) }.is_ok_and(routing::is_lfe))
        .is_ok_and(|v| v)
        .into()
}

/// 0 none, 1 LFE1, 2 LFE2; invalid input is none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_lfe_target(
    labels: *const Text,
    labels_len: usize,
    low_pass: u32,
) -> u32 {
    catch_unwind(|| {
        unsafe { texts(labels, labels_len) }
            .map_or(LfeTarget::None, |l| routing::lfe_target(&l, low_pass != 0))
    })
    .unwrap_or(LfeTarget::None) as u32
}

/// 1 and the output index when a target matches the labels, else 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_target_index(
    targets_ptr: *const RenderTarget,
    targets_len: usize,
    labels: *const Text,
    labels_len: usize,
    index: *mut usize,
) -> u32 {
    let found = catch_unwind(|| {
        let t = unsafe { targets(targets_ptr, targets_len) }.ok()?;
        let l = unsafe { texts(labels, labels_len) }.ok()?;
        routing::target_index(&t, &l)
    })
    .ok()
    .flatten();
    match found {
        Some(i) if !index.is_null() => {
            unsafe { *index = i };
            1
        }
        _ => 0,
    }
}

/// 1 and the nominal direction when one of the labels is, or aliases, a BS.2051 label, else 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_label_position(
    labels: *const Text,
    labels_len: usize,
    azimuth: *mut f32,
    elevation: *mut f32,
) -> u32 {
    let found =
        catch_unwind(|| routing::label_position(&unsafe { texts(labels, labels_len) }.ok()?))
            .ok()
            .flatten();
    match found {
        Some([a, e]) if !azimuth.is_null() && !elevation.is_null() => {
            unsafe {
                *azimuth = a;
                *elevation = e;
            }
            1
        }
        _ => 0,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_matrix_resolve(
    routes: *const RenderMatrixRoute,
    routes_len: usize,
    targets_ptr: *const RenderTarget,
    targets_len: usize,
    layout_id: Text,
    output: *mut *mut RenderMatrix,
    view: *mut RenderMatrixView,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        if output.is_null() || view.is_null() {
            return Err(bad("无效 DirectSpeakers 矩阵输出"));
        }
        unsafe { *output = ptr::null_mut() };
        let authored = unsafe { input(routes, routes_len)? }
            .iter()
            .map(|r| {
                Ok(MatrixRoute {
                    source_label: unsafe { bytes(&r.source_label)? },
                    mute: r.mute != 0,
                    targets: unsafe { input(r.targets, r.targets_len)? }
                        .iter()
                        .map(|t| Ok((unsafe { bytes(&t.label)? }, t.gain)))
                        .collect::<Result<_, Error>>()?,
                })
            })
            .collect::<Result<Vec<_>, Error>>()?;
        let t = unsafe { targets(targets_ptr, targets_len)? };
        let resolved = routing::resolve_matrix(&authored, &t, unsafe { bytes(&layout_id)? })?;
        let resolved_targets: Vec<Vec<RenderResolvedTarget>> = resolved
            .iter()
            .map(|r| {
                r.targets
                    .iter()
                    .map(|t| RenderResolvedTarget {
                        output: t.output,
                        gain: t.gain,
                    })
                    .collect()
            })
            .collect();
        let records = resolved
            .iter()
            .zip(&resolved_targets)
            .map(|(r, t)| RenderResolvedRoute {
                source_label: Text::bytes(&r.source_label),
                source_key: Text::bytes(&r.source_key),
                mute: u32::from(r.mute),
                targets: t.as_ptr(),
                targets_len: t.len(),
            })
            .collect();
        let handle = Box::new(RenderMatrix {
            routes: resolved,
            _targets: resolved_targets,
            records,
        });
        unsafe {
            *view = RenderMatrixView {
                routes: handle.records.as_ptr(),
                routes_len: handle.records.len(),
            };
            *output = Box::into_raw(handle);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_matrix_destroy(handle: *mut RenderMatrix) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The index of the resolved row a DirectSpeakers block's labels select.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_matrix_route(
    matrix: *const RenderMatrix,
    labels: *const Text,
    labels_len: usize,
    index: *mut usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        if matrix.is_null() || index.is_null() {
            return Err(bad("无效 DirectSpeakers 矩阵"));
        }
        let route = routing::matrix_route(unsafe { &(*matrix).routes }, &unsafe {
            texts(labels, labels_len)?
        })?;
        unsafe { *index = route };
        Ok(())
    })
}

unsafe fn override_value<'a>(o: &RenderOverride) -> Result<Override<'a>, Error> {
    Ok(Override {
        object_id: unsafe { bytes(&o.object_id)? },
        speaker_label: unsafe { bytes(&o.speaker_label)? },
        mute: o.mute != 0,
        gain_db: o.gain_db,
        head_locked: (o.has_head_locked != 0).then_some(o.head_locked != 0),
    })
}

/// The live overrides of one input channel; `key` is its canonical speaker label (empty for
/// Objects and HOA channels). Bit 1: a gain multiplier was written; bit 2: an explicit head lock
/// (0 or 1) was written; otherwise the channel inherits. Null output pointers skip that field.
/// Valid queries borrow the records and do not allocate, even for noncanonical labels.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_override(
    values: *const RenderOverride,
    values_len: usize,
    object_id: Text,
    key: Text,
    gain: *mut f32,
    head_locked: *mut u32,
) -> u32 {
    catch_unwind(|| {
        let (Ok(o), Ok(id), Ok(key)) = (
            unsafe { input(values, values_len) },
            unsafe { bytes(&object_id) },
            unsafe { bytes(&key) },
        ) else {
            return 0;
        };
        let Ok((g, h)) = routing::try_live_override(
            o.iter().map(|o| unsafe { override_value(o) }),
            id,
            key,
            !gain.is_null(),
            !head_locked.is_null(),
        ) else {
            return 0;
        };
        let mut flags = 0;
        if let (Some(g), Some(out)) = (g, unsafe { gain.as_mut() }) {
            *out = g;
            flags |= 1;
        }
        if let (Some(h), Some(out)) = (h, unsafe { head_locked.as_mut() }) {
            *out = u32::from(h);
            flags |= 2;
        }
        flags
    })
    .unwrap_or(0)
}

/// The 22.2 LFE routing for the LFE targets (0, 1, 2) of the in-effect DirectSpeakers blocks.
/// `output` is filled even on failure: its log lines precede the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_lfe_routing(
    split_power: u32,
    output_layout: Text,
    targets_ptr: *const u32,
    targets_len: usize,
    output: *mut RenderLfeRouting,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    boundary(
        message,
        message_len,
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            if output.is_null() {
                return Err(bad("无效 LFE 路由输出"));
            }
            let blocks = unsafe { input(targets_ptr, targets_len)? }
                .iter()
                .map(|&t| match t {
                    1 => LfeTarget::Lfe1,
                    2 => LfeTarget::Lfe2,
                    _ => LfeTarget::None,
                });
            let empty = RenderLogLine {
                level: 0,
                message: Text::bytes(&[]),
            };
            let mut logs = [empty; 2];
            let mut logs_len = 0;
            let result = routing::lfe_routing(
                split_power != 0,
                unsafe { bytes(&output_layout)? },
                blocks,
                &mut |level, text: &'static str| {
                    logs[logs_len] = RenderLogLine {
                        level: match level {
                            routing::Level::Info => 1,
                            routing::Level::Warning => 2,
                        },
                        message: Text::new(text),
                    };
                    logs_len += 1;
                },
            );
            let routing = result.as_ref().copied().unwrap_or_default();
            unsafe {
                *output = RenderLfeRouting {
                    applies_to_22_2: u32::from(routing.applies_to_22_2),
                    split_power: u32::from(routing.split_power),
                    has_lfe1: u32::from(routing.has_lfe1),
                    has_lfe2: u32::from(routing.has_lfe2),
                    logs,
                    logs_len,
                };
            }
            result.map(|_| ())
        },
    )
}

/// The gain from one LFE input target to one 22.2 LFE output (0, 1, 2) under `routing`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_lfe_gain(
    routing: *const RenderLfeRouting,
    input_target: u32,
    output_target: u32,
) -> f32 {
    let Some(r) = (unsafe { routing.as_ref() }) else {
        return 0.;
    };
    let target = |t| match t {
        1 => LfeTarget::Lfe1,
        2 => LfeTarget::Lfe2,
        _ => LfeTarget::None,
    };
    routing::LfeRouting {
        applies_to_22_2: r.applies_to_22_2 != 0,
        split_power: r.split_power != 0,
        has_lfe1: r.has_lfe1 != 0,
        has_lfe2: r.has_lfe2 != 0,
    }
    .gain(target(input_target), target(output_target))
}

#[repr(C)]
pub struct RenderOfflineSettings {
    inputs: usize,
    outputs: usize,
    frames: u64,
    sample_rate: u32,
    default_interp_ms: u32,
    smoothing_frames: u64,
    has_window: u32,
    window_start: u64,
    window_count: u64,
    has_meter_window: u32,
    meter_start: u64,
    meter_count: u64,
}

/// One rendered block; `frames` is 0 after the last one. The spans live until the next call.
#[repr(C)]
pub struct RenderOfflineBlock {
    start: u64,
    frames: usize,
    last: u32,
    emit_offset: usize,
    emit_frames: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
}

#[repr(C)]
pub struct RenderOfflineProgress {
    fraction: f64,
    stage_fraction: f64,
    done: u64,
    total: u64,
}

#[repr(C)]
pub struct RenderOfflineMetrics {
    has_lufs: u32,
    has_peak: u32,
    lufs: f64,
    peak_dbtp: f64,
}

/// An offline loudspeaker render in progress.
pub struct RenderOffline(Option<offline::Driver>);

/// Like `boundary` for the driver: the locator kind (0 none, 1 `path=<input>`, 2 `input=<input>`,
/// 3 `meter`, 4 `HOA DSP`, 5 `Triple Balance DSP`) and the message, cut at a character boundary
/// as the WAVE reader did.
fn driver_boundary(
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
    f: impl FnOnce() -> offline::Result<()>,
) -> i32 {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some(e),
        Err(_) => Some(offline::Error {
            code: 6,
            message: "Rust render panic".to_owned(),
            location: offline::Location::None,
        }),
    };
    if let Some(out) = unsafe { location.as_mut() } {
        *out = match error.as_ref().map(|e| e.location) {
            None | Some(offline::Location::None) => 0,
            Some(offline::Location::Path) => 1,
            Some(offline::Location::Input) => 2,
            Some(offline::Location::Meter) => 3,
            Some(offline::Location::HoaDsp) => 4,
            Some(offline::Location::TripleBalanceDsp) => 5,
        };
    }
    let detail = error.as_ref().map_or("", |e| e.message.as_str());
    if let Some(capacity) = unsafe { message_len.as_ref() }.copied() {
        let mut n = detail.len().min(capacity.saturating_sub(1));
        while !detail.is_char_boundary(n) {
            n -= 1;
        }
        unsafe { write(&detail.as_bytes()[..n], message, message_len) };
    }
    error.map_or(0, |e| e.code)
}

fn driver_error(message: &str) -> offline::Error {
    offline::Error {
        code: 1,
        message: message.to_owned(),
        location: offline::Location::None,
    }
}

unsafe fn driver<'a>(handle: *mut RenderOffline) -> offline::Result<&'a mut offline::Driver> {
    unsafe { handle.as_mut() }
        .and_then(|h| h.0.as_mut())
        .ok_or_else(|| driver_error("无效离线渲染句柄"))
}

/// Opens the input (UTF-8 path) and prepares the renderer and meter for `plan`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_offline_open(
    path: *const u8,
    path_len: usize,
    plan: *const c_void,
    settings: *const RenderOfflineSettings,
    backend: Text,
    output: *mut *mut RenderOffline,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let (Some(out), Some(s)) = (unsafe { output.as_mut() }, unsafe { settings.as_ref() })
        else {
            return Err(driver_error("无效离线渲染参数"));
        };
        // The kernel of a Rust preparation (`RenderPreparedView::plan`).
        let kernel = unsafe { plan.cast::<offline::Kernel>().as_ref() }
            .cloned()
            .ok_or_else(|| driver_error("无效离线渲染参数"))?;
        *out = ptr::null_mut();
        let path = unsafe { crate::wav::path(path, path_len) }.map_err(|e| offline::Error {
            code: e.code,
            message: e.message,
            location: offline::Location::Path,
        })?;
        let backend = String::from_utf8_lossy(
            unsafe { bytes(&backend) }
                .map_err(|e| driver_error(&String::from_utf8_lossy(&e.message)))?,
        )
        .into_owned();
        let window =
            |has: u32, start, count| (has != 0).then_some(offline::Window { start, count });
        let reader = offline::Driver::open_reader(path)?;
        let driver = offline::Driver::new(
            reader,
            offline::Settings {
                kernel,
                inputs: s.inputs,
                outputs: s.outputs,
                frames: s.frames,
                sample_rate: s.sample_rate,
                default_interp_ms: s.default_interp_ms,
                smoothing_frames: s.smoothing_frames,
                window: window(s.has_window, s.window_start, s.window_count),
                meter_window: window(s.has_meter_window, s.meter_start, s.meter_count),
                backend,
            },
        )?;
        *out = Box::into_raw(Box::new(RenderOffline(Some(driver))));
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_offline_next(
    handle: *mut RenderOffline,
    block: *mut RenderOfflineBlock,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let out = unsafe { block.as_mut() }.ok_or_else(|| driver_error("无效离线渲染块"))?;
        let d = unsafe { driver(handle)? };
        *out = RenderOfflineBlock {
            start: 0,
            frames: 0,
            last: 0,
            emit_offset: 0,
            emit_frames: 0,
            input: ptr::null(),
            input_len: 0,
            output: ptr::null_mut(),
            output_len: 0,
        };
        let Some(b) = d.render()? else {
            return Ok(());
        };
        let (input, input_len) = (d.input().as_ptr(), d.input().len());
        let output = d.output();
        *out = RenderOfflineBlock {
            start: b.start,
            frames: b.frames,
            last: u32::from(b.last),
            emit_offset: b.emit_offset,
            emit_frames: b.emit_frames,
            input,
            input_len,
            output: output.as_mut_ptr(),
            output_len: output.len(),
        };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_offline_commit(
    handle: *mut RenderOffline,
    progress: *mut RenderOfflineProgress,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let out = unsafe { progress.as_mut() }.ok_or_else(|| driver_error("无效离线渲染进度"))?;
        let p = unsafe { driver(handle)? }.commit()?;
        *out = RenderOfflineProgress {
            fraction: p.fraction,
            stage_fraction: p.stage_fraction,
            done: p.done,
            total: p.total,
        };
        Ok(())
    })
}

/// Waits for the meter and reports the global loudness and true peak; the handle is spent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_offline_finish(
    handle: *mut RenderOffline,
    metrics: *mut RenderOfflineMetrics,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let out = unsafe { metrics.as_mut() }.ok_or_else(|| driver_error("无效离线渲染结果"))?;
        let d = unsafe { handle.as_mut() }
            .and_then(|h| h.0.take())
            .ok_or_else(|| driver_error("无效离线渲染句柄"))?;
        let m = d.finish()?;
        *out = RenderOfflineMetrics {
            has_lufs: u32::from(m.lufs.is_some()),
            has_peak: u32::from(m.peak_dbtp.is_some()),
            lufs: m.lufs.unwrap_or(0.),
            peak_dbtp: m.peak_dbtp.unwrap_or(0.),
        };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_offline_destroy(handle: *mut RenderOffline) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

#[repr(C)]
pub struct RenderSpeaker {
    label: Text,
    azimuth: f32,
    elevation: f32,
    is_lfe: u32,
}

/// `routing`: 0 label, 1 position, 2 matrix; `matrix` is read when `has_matrix != 0`.
#[repr(C)]
pub struct RenderVbapRequest {
    speakers: *const RenderSpeaker,
    speakers_len: usize,
    layout_id: Text,
    input_path: Text,
    spread: u32,
    routing: u32,
    has_matrix: u32,
    matrix: *const RenderMatrixRoute,
    matrix_len: usize,
    split_power: u32,
}

/// The log lines (also of a failed preparation) and the error, or the offline driver's plan,
/// its input-channel count and its output channels once prepared.
#[repr(C)]
pub struct RenderPreparedView {
    logs: *const RenderLogLine,
    logs_len: usize,
    message: Text,
    context: Text,
    plan: *const c_void,
    tracks: usize,
    outputs: usize,
}

/// A prepared VBAP, HOA, EAR or Triple Balance render, or the log lines and error of a failed one.
pub struct RenderPrepared {
    texts: Vec<Vec<u8>>,
    lines: Vec<RenderLogLine>,
    error: Error,
    kernel: Option<offline::Kernel>,
    bindings: Arc<[vbap::Binding]>,
    settings: (usize, usize, u64, u32),
    triple_balance: Option<Box<TripleBalanceData>>,
}

/// Boxes a preparation's logs and result, fills `view` and returns the status.
fn prepared(
    logs: Vec<(routing::Level, Vec<u8>)>,
    result: Result<(offline::Kernel, Vec<vbap::Binding>), Error>,
    settings: (usize, usize, u64, u32),
    output: &mut *mut RenderPrepared,
    view: &mut RenderPreparedView,
) -> Result<(), Error> {
    let mut handle = Box::new(RenderPrepared {
        texts: Vec::with_capacity(logs.len()),
        lines: Vec::with_capacity(logs.len()),
        error: Error {
            code: 0,
            message: Vec::new(),
            context: Vec::new(),
        },
        kernel: None,
        bindings: Arc::from([]),
        settings,
        triple_balance: None,
    });
    for (level, text) in logs {
        handle.lines.push(RenderLogLine {
            level: match level {
                routing::Level::Info => 1,
                routing::Level::Warning => 2,
            },
            message: Text::bytes(&text),
        });
        handle.texts.push(text);
    }
    let (code, tracks) = match result {
        Ok((kernel, bindings)) => {
            handle.kernel = Some(kernel);
            let tracks = bindings.len();
            handle.bindings = Arc::from(bindings);
            (0, tracks)
        }
        Err(e) => {
            let code = e.code;
            handle.error = e;
            (code, 0)
        }
    };
    *view = RenderPreparedView {
        logs: handle.lines.as_ptr(),
        logs_len: handle.lines.len(),
        message: Text::bytes(&handle.error.message),
        context: Text::bytes(&handle.error.context),
        plan: handle
            .kernel
            .as_ref()
            .map_or(ptr::null(), |k| ptr::from_ref(k).cast()),
        tracks,
        outputs: handle.settings.1,
    };
    *output = Box::into_raw(handle);
    if code == 0 {
        Ok(())
    } else {
        Err(Error {
            code,
            message: Vec::new(),
            context: Vec::new(),
        })
    }
}

/// A borrowed DirectSpeakers matrix.
unsafe fn matrix_routes<'a>(
    data: *const RenderMatrixRoute,
    len: usize,
) -> Result<Vec<MatrixRoute<'a>>, Error> {
    unsafe { input(data, len)? }
        .iter()
        .map(|m| {
            Ok(MatrixRoute {
                source_label: unsafe { bytes(&m.source_label)? },
                mute: m.mute != 0,
                targets: unsafe { input(m.targets, m.targets_len)? }
                    .iter()
                    .map(|t| Ok((unsafe { bytes(&t.label)? }, t.gain)))
                    .collect::<Result<_, Error>>()?,
            })
        })
        .collect()
}

/// Prepares the VBAP gain plan of the policy-applied scene. `*output` and `*view` are set unless
/// the arguments are invalid: the log lines precede the error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_vbap_prepare(
    scene: *const SceneHandle,
    request: *const RenderVbapRequest,
    output: *mut *mut RenderPrepared,
    view: *mut RenderPreparedView,
) -> i32 {
    boundary(
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let (Some(out), Some(view), Some(scene), Some(r)) = (
                unsafe { output.as_mut() },
                unsafe { view.as_mut() },
                unsafe { scene.as_ref() },
                unsafe { request.as_ref() },
            ) else {
                return Err(bad("无效 VBAP 准备参数"));
            };
            *out = ptr::null_mut();
            let speakers = unsafe { input(r.speakers, r.speakers_len)? }
                .iter()
                .map(|s| {
                    Ok(vbap::Speaker {
                        label: unsafe { bytes(&s.label)? },
                        azimuth: s.azimuth,
                        elevation: s.elevation,
                        is_lfe: s.is_lfe != 0,
                    })
                })
                .collect::<Result<Vec<_>, Error>>()?;
            let matrix = unsafe { matrix_routes(r.matrix, r.matrix_len)? };
            let routing = match r.routing {
                0 => vbap::Routing::Label,
                1 => vbap::Routing::Position,
                2 => vbap::Routing::Matrix,
                _ => return Err(bad("无效 DirectSpeakers 路由")),
            };
            let scene = scene.scene();
            let request = vbap::Request {
                scene,
                speakers: &speakers,
                layout_id: unsafe { bytes(&r.layout_id)? },
                input_path: unsafe { bytes(&r.input_path)? },
                spread: r.spread != 0,
                routing,
                matrix: (r.has_matrix != 0).then_some(&matrix[..]),
                split_power: r.split_power != 0,
            };
            let mut logs = Vec::new();
            let result = vbap::prepare(&request, &mut |level, text| logs.push((level, text)));
            let settings = (
                usize::from(scene.info.channels),
                speakers.len(),
                scene.info.frames,
                scene.info.sample_rate,
            );
            let result = result.map(|p| (offline::Kernel::Mix(p.plan), p.bindings));
            prepared(logs, result, settings, out, view)
        },
    )
}

/// The HOA3 encoder request; the output layout must be `hoa3`.
#[repr(C)]
pub struct RenderHoaRequest {
    layout_id: Text,
    split_power: u32,
}

/// Prepares the HOA encoder plan of the policy-applied scene, like `mradm_render_vbap_prepare`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_hoa_prepare(
    scene: *const SceneHandle,
    request: *const RenderHoaRequest,
    output: *mut *mut RenderPrepared,
    view: *mut RenderPreparedView,
) -> i32 {
    boundary(
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let (Some(out), Some(view), Some(scene), Some(r)) = (
                unsafe { output.as_mut() },
                unsafe { view.as_mut() },
                unsafe { scene.as_ref() },
                unsafe { request.as_ref() },
            ) else {
                return Err(bad("无效 HOA 准备参数"));
            };
            *out = ptr::null_mut();
            let scene = scene.scene();
            let request = hoa::Request {
                scene,
                layout_id: unsafe { bytes(&r.layout_id)? },
                split_power: r.split_power != 0,
            };
            let mut logs = Vec::new();
            let result = hoa::prepare(&request, &mut |level, text| logs.push((level, text)));
            let settings = (
                usize::from(scene.info.channels),
                mradm_dsp::hoa::CHANNELS,
                scene.info.frames,
                scene.info.sample_rate,
            );
            let result = result.map(|p| (offline::Kernel::Hoa(p.plan), p.bindings));
            prepared(logs, result, settings, out, view)
        },
    )
}

/// `geometry`: 0 standard, 1 Apple. `matrix_routing` selects matrix DirectSpeakers routing
/// (otherwise automatic); its matrix is read when `has_matrix != 0`.
#[repr(C)]
pub struct RenderEarRequest {
    layout_id: Text,
    geometry: u32,
    input_path: Text,
    matrix_routing: u32,
    has_matrix: u32,
    matrix: *const RenderMatrixRoute,
    matrix_len: usize,
    split_power: u32,
}

/// Prepares the EAR gain plan and filters of the policy-applied scene, like
/// `mradm_render_vbap_prepare`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_ear_prepare(
    scene: *const SceneHandle,
    request: *const RenderEarRequest,
    output: *mut *mut RenderPrepared,
    view: *mut RenderPreparedView,
) -> i32 {
    boundary(
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let (Some(out), Some(view), Some(scene), Some(r)) = (
                unsafe { output.as_mut() },
                unsafe { view.as_mut() },
                unsafe { scene.as_ref() },
                unsafe { request.as_ref() },
            ) else {
                return Err(bad("无效 EAR 准备参数"));
            };
            *out = ptr::null_mut();
            let matrix = unsafe { matrix_routes(r.matrix, r.matrix_len)? };
            let layout_id = unsafe { bytes(&r.layout_id)? };
            let scene = scene.scene();
            let request = ear::Request {
                scene,
                layout_id,
                geometry: match r.geometry {
                    0 => mradm_scene::layouts::Geometry::Standard,
                    1 => mradm_scene::layouts::Geometry::Apple,
                    _ => return Err(bad("无效扬声器几何")),
                },
                matrix_routing: r.matrix_routing != 0,
                matrix: (r.has_matrix != 0).then_some(&matrix[..]),
                input_path: unsafe { bytes(&r.input_path)? },
                split_power: r.split_power != 0,
            };
            let mut logs = Vec::new();
            let result = ear::prepare(&request, &mut |level, text| logs.push((level, text)));
            let outputs = result.as_ref().map_or(0, |p| p.outputs);
            let settings = (
                usize::from(scene.info.channels),
                outputs,
                scene.info.frames,
                scene.info.sample_rate,
            );
            let result = result.map(|p| {
                let plan = offline::EarPlan {
                    mix: p.plan,
                    filters: p.filters,
                    wav71: layout_id == b"wav71",
                };
                (offline::Kernel::Ear(Arc::new(plan)), p.bindings)
            });
            prepared(logs, result, settings, out, view)
        },
    )
}

/// The Triple Balance request. `mode`: 0 standard, 1 D. `spread`: 0 automatic, 1 none, 2 MDAP.
/// `routing`: 0 automatic, 1 label, 2 position, 3 matrix.
#[repr(C)]
pub struct RenderTripleBalanceRequest {
    layout_id: Text,
    mode: u32,
    spread: u32,
    split_power: u32,
    standard_geometry: u32,
    smoothing_frames: u32,
    routing: u32,
    has_matrix: u32,
    input_path: Text,
}

/// The backend report, also of a rejected preparation.
#[repr(C)]
pub struct RenderTripleBalanceView {
    report: Text,
}

/// What the file stream reads besides the offline kernel: the size tracks and the spread mode.
struct TripleBalanceData {
    report: Vec<u8>,
    sizes: Arc<[triple_balance::SizeTrack]>,
    spread_none: bool,
}

/// Prepares the Triple Balance plan of the policy-applied scene. `*output`, `*view` and
/// `*triple_balance` are set unless the arguments are invalid; the report is set on rejection too.
/// The handle also opens the standard-mode file stream.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_prepare(
    scene: *const SceneHandle,
    request: *const RenderTripleBalanceRequest,
    output: *mut *mut RenderPrepared,
    view: *mut RenderPreparedView,
    triple_balance: *mut RenderTripleBalanceView,
) -> i32 {
    boundary(
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let (Some(out), Some(view), Some(tb_view), Some(scene), Some(r)) = (
                unsafe { output.as_mut() },
                unsafe { view.as_mut() },
                unsafe { triple_balance.as_mut() },
                unsafe { scene.as_ref() },
                unsafe { request.as_ref() },
            ) else {
                return Err(bad("无效 Triple Balance 准备参数"));
            };
            *out = ptr::null_mut();
            let scene = scene.scene();
            let request = triple_balance::Request {
                scene,
                layout_id: unsafe { bytes(&r.layout_id)? },
                mode: match r.mode {
                    0 => triple_balance::Mode::Standard,
                    1 => triple_balance::Mode::D,
                    _ => return Err(bad("invalid Triple Balance mode")),
                },
                spread: match r.spread {
                    0 => triple_balance::Spread::Automatic,
                    1 => triple_balance::Spread::None,
                    2 => triple_balance::Spread::Mdap,
                    _ => return Err(bad("无效扩展模式")),
                },
                split_power: r.split_power != 0,
                standard_geometry: r.standard_geometry != 0,
                smoothing_frames: r.smoothing_frames,
                routing: match r.routing {
                    0 => triple_balance::Routing::Automatic,
                    1 => triple_balance::Routing::Label,
                    2 => triple_balance::Routing::Position,
                    3 => triple_balance::Routing::Matrix,
                    _ => return Err(bad("无效 DirectSpeakers 路由")),
                },
                has_matrix: r.has_matrix != 0,
                input_path: unsafe { bytes(&r.input_path)? },
            };
            let mut logs = Vec::new();
            let outcome =
                triple_balance::prepare(&request, &mut |level, text| logs.push((level, text)));
            let outputs = outcome.result.as_ref().map_or(0, |p| p.outputs);
            let settings = (
                usize::from(scene.info.channels),
                outputs,
                scene.info.frames,
                scene.info.sample_rate,
            );
            let mut data = TripleBalanceData {
                report: outcome.report.into_bytes(),
                sizes: Arc::from([]),
                spread_none: request.spread == triple_balance::Spread::None,
            };
            let result = outcome.result.map(|p| {
                data.sizes = Arc::from(p.size_tracks);
                (offline::Kernel::TripleBalance(p.plan), p.bindings)
            });
            let status = prepared(logs, result, settings, out, view);
            // `prepared` boxed the handle.
            let handle = unsafe { &mut **out };
            let data = handle.triple_balance.insert(Box::new(data));
            *tb_view = RenderTripleBalanceView {
                report: Text::bytes(&data.report),
            };
            status
        },
    )
}

/// A Triple Balance live edit: the common override and the topology scales it validates.
#[repr(C)]
pub struct RenderTripleBalanceOverride {
    base: RenderOverride,
    extent_scale: f32,
    extent_width_scale: f32,
    extent_height_scale: f32,
    extent_depth_scale: f32,
    diffuse_scale: f32,
    divergence_scale: f32,
}

/// Triple Balance file streaming over a standard-mode preparation.
pub struct RenderTripleBalanceStream(triple_balance_stream::Stream);

unsafe fn tb_stream<'a>(
    handle: *mut RenderTripleBalanceStream,
) -> offline::Result<&'a mut triple_balance_stream::Stream> {
    unsafe { handle.as_mut() }
        .map(|h| &mut h.0)
        .ok_or_else(|| driver_error("无效 Triple Balance 流句柄"))
}

unsafe fn tb_overrides<'a>(
    values: *const RenderTripleBalanceOverride,
    values_len: usize,
) -> offline::Result<Vec<triple_balance_stream::LiveOverride<'a>>> {
    unsafe { input(values, values_len) }
        .and_then(|values| {
            values
                .iter()
                .map(|o| {
                    Ok(triple_balance_stream::LiveOverride {
                        base: unsafe { override_value(&o.base)? },
                        extent_scale: o.extent_scale,
                        extent_width_scale: o.extent_width_scale,
                        extent_height_scale: o.extent_height_scale,
                        extent_depth_scale: o.extent_depth_scale,
                        diffuse_scale: o.diffuse_scale,
                        divergence_scale: o.divergence_scale,
                    })
                })
                .collect::<Result<Vec<_>, Error>>()
        })
        .map_err(|e| driver_error(&String::from_utf8_lossy(&e.message)))
}

/// Opens the input (UTF-8 path) for streaming a standard-mode Triple Balance preparation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_open(
    prepared: *const RenderPrepared,
    path: *const u8,
    path_len: usize,
    default_interp_ms: u32,
    output: *mut *mut RenderTripleBalanceStream,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let (Some(out), Some(p)) = (unsafe { output.as_mut() }, unsafe { prepared.as_ref() })
        else {
            return Err(driver_error("无效流式渲染参数"));
        };
        *out = ptr::null_mut();
        let (Some(offline::Kernel::TripleBalance(plan)), Some(data)) =
            (p.kernel.as_ref(), p.triple_balance.as_ref())
        else {
            return Err(driver_error("无效流式渲染参数"));
        };
        let (mix, session) = triple_balance_stream::Source::of(plan)
            .ok_or_else(|| driver_error("无效流式渲染参数"))?;
        let path = unsafe { crate::wav::path(path, path_len) }.map_err(|e| offline::Error {
            code: e.code,
            message: e.message,
            location: offline::Location::Path,
        })?;
        let (inputs, outputs, frames, sample_rate) = p.settings;
        let source = triple_balance_stream::Source {
            mix,
            session,
            bindings: Arc::clone(&p.bindings),
            size_tracks: Arc::clone(&data.sizes),
            spread_none: data.spread_none,
            inputs,
            outputs,
            frames,
            sample_rate,
        };
        let s = triple_balance_stream::Stream::open(
            path,
            source,
            default_interp_ms,
            triple_balance_stream::DEFAULT_CHECKPOINT_BUDGET,
        )?;
        *out = Box::into_raw(Box::new(RenderTripleBalanceStream(s)));
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_process(
    handle: *mut RenderTripleBalanceStream,
    output: *mut f32,
    output_len: usize,
    frames: usize,
    produced: *mut usize,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let out = unsafe { produced.as_mut() }.ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = 0;
        let s = unsafe { tb_stream(handle)? };
        let buffer = unsafe { checked_output(output, output_len) }
            .ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = s.process(buffer, frames)?;
        Ok(())
    })
}

/// `*pending` (set unless the call fails) is whether a block remains before `seek_finish`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_seek_begin(
    handle: *mut RenderTripleBalanceStream,
    frame: u64,
    pending: *mut u32,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let more = unsafe { tb_stream(handle)? }.seek_begin(frame)?;
        if let Some(p) = unsafe { pending.as_mut() } {
            *p = u32::from(more);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_seek_step(
    handle: *mut RenderTripleBalanceStream,
    pending: *mut u32,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let more = unsafe { tb_stream(handle)? }.seek_step()?;
        if let Some(p) = unsafe { pending.as_mut() } {
            *p = u32::from(more);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_seek_finish(
    handle: *mut RenderTripleBalanceStream,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        unsafe { tb_stream(handle)? }.seek_finish()
    })
}

/// A rejection fills `message` and `context` (the object id) like the other boundaries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_validate(
    handle: *const RenderTripleBalanceStream,
    values: *const RenderTripleBalanceOverride,
    values_len: usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let s = unsafe { handle.as_ref() }
            .map(|h| &h.0)
            .ok_or_else(|| bad("无效 Triple Balance 流句柄"))?;
        let o = unsafe { tb_overrides(values, values_len) }.map_err(|e| bad(&e.message))?;
        s.validate_overrides(&o)
            .map_err(|(code, message, context)| Error {
                code,
                message: message.into_bytes(),
                context,
            })
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_overrides(
    handle: *mut RenderTripleBalanceStream,
    values: *const RenderTripleBalanceOverride,
    values_len: usize,
) -> i32 {
    driver_boundary(ptr::null_mut(), ptr::null_mut(), ptr::null_mut(), || {
        let o = unsafe { tb_overrides(values, values_len)? };
        unsafe { tb_stream(handle)? }.set_overrides(&o)
    })
}

/// 1 when size tracks make a seek render from an earlier block.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_requires_preroll(
    handle: *const RenderTripleBalanceStream,
) -> u32 {
    unsafe { handle.as_ref() }.map_or(0, |h| u32::from(h.0.requires_preroll()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_triple_balance_stream_destroy(
    handle: *mut RenderTripleBalanceStream,
) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_prepared_destroy(handle: *mut RenderPrepared) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// File streaming over a prepared plan.
pub struct RenderStream(stream::FileStream);

/// Opens the input (UTF-8 path) for streaming the prepared plan.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_stream_open(
    prepared: *const RenderPrepared,
    path: *const u8,
    path_len: usize,
    default_interp_ms: u32,
    smoothing_frames: u64,
    output: *mut *mut RenderStream,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let (Some(out), Some(p)) = (unsafe { output.as_mut() }, unsafe { prepared.as_ref() })
        else {
            return Err(driver_error("无效流式渲染参数"));
        };
        *out = ptr::null_mut();
        let kernel = p
            .kernel
            .as_ref()
            .ok_or_else(|| driver_error("无效流式渲染参数"))?;
        let path = unsafe { crate::wav::path(path, path_len) }.map_err(|e| offline::Error {
            code: e.code,
            message: e.message,
            location: offline::Location::Path,
        })?;
        let (inputs, outputs, frames, sample_rate) = p.settings;
        let s = stream::FileStream::open(
            path,
            kernel,
            Arc::clone(&p.bindings),
            stream::Settings {
                inputs,
                outputs,
                frames,
                sample_rate,
                default_interp_ms,
                smoothing_frames,
            },
        )?;
        *out = Box::into_raw(Box::new(RenderStream(s)));
        Ok(())
    })
}

unsafe fn file_stream<'a>(
    handle: *mut RenderStream,
) -> offline::Result<&'a mut stream::FileStream> {
    unsafe { handle.as_mut() }
        .map(|h| &mut h.0)
        .ok_or_else(|| driver_error("无效流式渲染句柄"))
}

/// Fills up to `frames` interleaved frames of `output`; `*produced` is less only at the end.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_stream_process(
    handle: *mut RenderStream,
    output: *mut f32,
    output_len: usize,
    frames: usize,
    produced: *mut usize,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        let out = unsafe { produced.as_mut() }.ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = 0;
        let s = unsafe { file_stream(handle)? };
        let buffer = unsafe { checked_output(output, output_len) }
            .ok_or_else(|| driver_error("无效流式渲染输出"))?;
        *out = s.process(buffer, frames)?;
        Ok(())
    })
}

unsafe fn checked_output<'a>(data: *mut f32, len: usize) -> Option<&'a mut [f32]> {
    if len == 0 {
        return Some(&mut []);
    }
    if data.is_null()
        || !(data as usize).is_multiple_of(align_of::<f32>())
        || len > isize::MAX as usize / size_of::<f32>()
    {
        return None;
    }
    Some(unsafe { slice::from_raw_parts_mut(data, len) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_stream_seek(
    handle: *mut RenderStream,
    frame: u64,
    location: *mut u32,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    driver_boundary(location, message, message_len, || {
        unsafe { file_stream(handle)? }.seek(frame)
    })
}

/// Replaces the live overrides; nonzero for invalid input or a nonfinite gain.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_stream_overrides(
    handle: *mut RenderStream,
    values: *const RenderOverride,
    values_len: usize,
) -> i32 {
    driver_boundary(ptr::null_mut(), ptr::null_mut(), ptr::null_mut(), || {
        let o = unsafe { input(values, values_len) }
            .and_then(|values| {
                values
                    .iter()
                    .map(|o| unsafe { override_value(o) })
                    .collect::<Result<Vec<_>, _>>()
            })
            .map_err(|e| driver_error(&String::from_utf8_lossy(&e.message)))?;
        unsafe { file_stream(handle)? }.set_overrides(&o)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_stream_destroy(handle: *mut RenderStream) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The Scene stream VBAP renderer's configuration (`live_scene::RendererConfig`).
#[repr(C)]
pub struct RenderLiveVbapRequest {
    layout_id: Text,
    /// 1 for the Apple speaker geometry.
    geometry: u32,
    /// `speaker_spread_mode != none`.
    spread: u32,
    split_power: u32,
    smoothing_frames: u32,
}

/// `live_scene::ElementDescriptor`; role 0 object, 1 DirectSpeakers, 2 LFE.
#[repr(C)]
pub struct RenderLiveElement {
    element_id: u64,
    role: u32,
    speaker_label: Text,
    has_position: u32,
    position: [f32; 3],
}

/// `live_scene::ObjectState`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RenderLiveState {
    valid_fields: u64,
    active: u32,
    linear_gain: f32,
    position: [f32; 3],
    width: f32,
    height: f32,
    depth: f32,
    diffuse: f32,
    divergence: f32,
    channel_lock: u32,
    screen_reference: u32,
    head_locked: u32,
    divergence_azimuth_range: f32,
    divergence_position_range: f32,
    has_max_distance: u32,
    max_distance: f32,
}

/// A PCM plane; the samples are read only with a signal.
#[repr(C)]
pub struct RenderLivePlane {
    element_id: u64,
    has_signal: u32,
    samples: *const f32,
    samples_len: usize,
}

#[repr(C)]
pub struct RenderLiveEntry {
    element_id: u64,
    state: RenderLiveState,
}

#[repr(C)]
pub struct RenderLiveUpdate {
    element_id: u64,
    offset: u32,
    ramp: u32,
    jump: u32,
    changed: u64,
    cleared: u64,
    state: RenderLiveState,
}

/// One `live_scene::Frame`; `trace` is the checkpoint prefix, empty for none.
#[repr(C)]
pub struct RenderLiveFrame {
    generation: u64,
    duration: u32,
    planes: *const RenderLivePlane,
    planes_len: usize,
    initial: *const RenderLiveEntry,
    initial_len: usize,
    updates: *const RenderLiveUpdate,
    updates_len: usize,
    trace: Text,
}

/// A warning of the rendered frame (`live_scene::DiagnosticCode`); the message is static text.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RenderLiveDiagnostic {
    code: u32,
    element_id: u64,
    field: u64,
    message: Text,
}

pub struct RenderLiveVbap {
    renderer: live_vbap::Renderer,
    initial: Vec<live_vbap::Entry>,
    updates: Vec<live_vbap::Update>,
    diagnostics: Vec<RenderLiveDiagnostic>,
}

fn live_state(s: &RenderLiveState) -> live_vbap::State {
    live_vbap::State {
        valid_fields: s.valid_fields,
        active: s.active != 0,
        linear_gain: s.linear_gain,
        position: s.position,
        width: s.width,
        height: s.height,
        depth: s.depth,
        diffuse: s.diffuse,
        divergence: s.divergence,
        channel_lock: s.channel_lock != 0,
        screen_reference: s.screen_reference != 0,
        head_locked: s.head_locked != 0,
        divergence_azimuth_range: s.divergence_azimuth_range,
        divergence_position_range: s.divergence_position_range,
        channel_lock_max_distance: (s.has_max_distance != 0).then_some(s.max_distance),
    }
}

unsafe fn live_vbap<'a>(handle: *mut RenderLiveVbap) -> Result<&'a mut RenderLiveVbap, Error> {
    unsafe { handle.as_mut() }.ok_or_else(|| bad("无效 Live VBAP 句柄"))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_vbap_create(
    request: *const RenderLiveVbapRequest,
    output: *mut *mut RenderLiveVbap,
    message: *mut u8,
    message_len: *mut usize,
) -> i32 {
    boundary(
        message,
        message_len,
        ptr::null_mut(),
        ptr::null_mut(),
        || {
            let (Some(out), Some(r)) = (unsafe { output.as_mut() }, unsafe { request.as_ref() })
            else {
                return Err(bad("无效 Live VBAP 参数"));
            };
            *out = ptr::null_mut();
            let renderer = live_vbap::Renderer::new(&live_vbap::Config {
                layout_id: unsafe { bytes(&r.layout_id)? },
                geometry: if r.geometry == 1 {
                    mradm_scene::layouts::Geometry::Apple
                } else {
                    mradm_scene::layouts::Geometry::Standard
                },
                spread: r.spread != 0,
                split_power: r.split_power != 0,
                smoothing_frames: r.smoothing_frames,
            })?;
            *out = Box::into_raw(Box::new(RenderLiveVbap {
                renderer,
                initial: Vec::new(),
                updates: Vec::new(),
                diagnostics: Vec::new(),
            }));
            Ok(())
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_vbap_destroy(handle: *mut RenderLiveVbap) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle)) };
    }
}

/// The output channels; 0 for a null handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_vbap_channels(handle: *const RenderLiveVbap) -> u32 {
    unsafe { handle.as_ref() }.map_or(0, |h| h.renderer.output_channels())
}

/// Replaces the element generation; a failure keeps the previous one.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_vbap_configure(
    handle: *mut RenderLiveVbap,
    generation: u64,
    elements: *const RenderLiveElement,
    elements_len: usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let h = unsafe { live_vbap(handle)? };
        let elements = unsafe { input(elements, elements_len)? }
            .iter()
            .map(|e| {
                Ok(live_vbap::Element {
                    id: e.element_id,
                    role: match e.role {
                        0 => Some(live_vbap::Role::Object),
                        1 => Some(live_vbap::Role::DirectSpeaker),
                        2 => Some(live_vbap::Role::Lfe),
                        _ => None,
                    },
                    speaker_label: unsafe { bytes(&e.speaker_label)? },
                    has_position: e.has_position != 0,
                    position: e.position,
                })
            })
            .collect::<Result<Vec<_>, Error>>()?;
        h.renderer.configure(generation, &elements)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_vbap_reset(handle: *mut RenderLiveVbap) {
    if let Some(h) = unsafe { handle.as_mut() } {
        h.renderer.reset();
    }
}

/// Renders the frame into the start of `output`. On success `*diagnostics` lists the frame's
/// warnings until the next call on the handle; on failure the output is unchanged.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_live_vbap_render(
    handle: *mut RenderLiveVbap,
    frame: *const RenderLiveFrame,
    output: *mut f32,
    output_len: usize,
    diagnostics: *mut *const RenderLiveDiagnostic,
    diagnostics_len: *mut usize,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let (Some(list), Some(count)) = (unsafe { diagnostics.as_mut() }, unsafe {
            diagnostics_len.as_mut()
        }) else {
            return Err(bad("无效 Live VBAP 参数"));
        };
        *list = ptr::null();
        *count = 0;
        // Retain the mixer boundary's rejection of overlapping PCM. Validate the raw
        // ranges before borrowing the handle, samples or mutable output.
        let invalid_buffers = |_| Error {
            code: 1,
            message: b"Invalid Live VBAP FFI arguments".to_vec(),
            context: b"Live VBAP DSP".to_vec(),
        };
        if handle.is_null() {
            return Err(bad("无效 Live VBAP 句柄"));
        }
        if frame.is_null() {
            return Err(bad("无效 Live VBAP 参数"));
        }
        crate::checked::separate(handle, 1, frame, 1).map_err(invalid_buffers)?;
        crate::checked::separate(handle, 1, output, output_len).map_err(invalid_buffers)?;
        crate::checked::separate(frame, 1, output, output_len).map_err(invalid_buffers)?;
        let f = unsafe { frame.as_ref() }.ok_or_else(|| bad("无效 Live VBAP 参数"))?;
        crate::checked::separate(handle, 1, f.planes, f.planes_len).map_err(invalid_buffers)?;
        crate::checked::separate(handle, 1, f.initial, f.initial_len).map_err(invalid_buffers)?;
        crate::checked::separate(handle, 1, f.updates, f.updates_len).map_err(invalid_buffers)?;
        crate::checked::separate(handle, 1, f.trace.data, f.trace.len).map_err(invalid_buffers)?;
        crate::checked::separate(output, output_len, f.planes, f.planes_len)
            .map_err(invalid_buffers)?;
        crate::checked::separate(output, output_len, f.initial, f.initial_len)
            .map_err(invalid_buffers)?;
        crate::checked::separate(output, output_len, f.updates, f.updates_len)
            .map_err(invalid_buffers)?;
        crate::checked::separate(output, output_len, f.trace.data, f.trace.len)
            .map_err(invalid_buffers)?;
        let records = unsafe { input(f.planes, f.planes_len)? };
        for p in records.iter().filter(|p| p.has_signal != 0) {
            crate::checked::separate(handle, 1, p.samples, p.samples_len)
                .map_err(invalid_buffers)?;
            crate::checked::separate(output, output_len, p.samples, p.samples_len)
                .map_err(invalid_buffers)?;
        }
        let h = unsafe { live_vbap(handle)? };
        let planes = records
            .iter()
            .map(|p| {
                Ok(live_vbap::Plane {
                    element_id: p.element_id,
                    has_signal: p.has_signal != 0,
                    samples: if p.has_signal != 0 {
                        unsafe { input(p.samples, p.samples_len)? }
                    } else {
                        &[]
                    },
                })
            })
            .collect::<Result<Vec<_>, Error>>()?;
        h.initial.clear();
        h.initial
            .extend(
                unsafe { input(f.initial, f.initial_len)? }
                    .iter()
                    .map(|e| live_vbap::Entry {
                        element_id: e.element_id,
                        state: live_state(&e.state),
                    }),
            );
        h.updates.clear();
        h.updates
            .extend(unsafe { input(f.updates, f.updates_len)? }.iter().map(|u| {
                live_vbap::Update {
                    element_id: u.element_id,
                    offset: u.offset,
                    ramp: u.ramp,
                    jump: u.jump != 0,
                    changed: u.changed,
                    cleared: u.cleared,
                    state: live_state(&u.state),
                }
            }));
        let output = unsafe { checked_output(output, output_len) }
            .ok_or_else(|| bad("无效 Live VBAP 输出"))?;
        h.renderer.render(
            &live_vbap::Frame {
                generation: f.generation,
                duration: f.duration,
                planes: &planes,
                initial: &h.initial,
                updates: &h.updates,
                trace: unsafe { bytes(&f.trace)? },
            },
            output,
        )?;
        h.diagnostics.clear();
        h.diagnostics.extend(
            h.renderer
                .diagnostics()
                .iter()
                .map(|d| RenderLiveDiagnostic {
                    code: d.code,
                    element_id: d.element_id,
                    field: d.field,
                    message: Text {
                        data: d.message.as_ptr(),
                        len: d.message.len(),
                    },
                }),
        );
        *list = h.diagnostics.as_ptr();
        *count = h.diagnostics.len();
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;

    fn record(id: &[u8], label: &[u8], gain_db: f32, head: Option<bool>) -> RenderOverride {
        RenderOverride {
            object_id: Text::bytes(id),
            speaker_label: Text::bytes(label),
            mute: 0,
            gain_db,
            has_head_locked: u32::from(head.is_some()),
            head_locked: u32::from(head.unwrap_or(false)),
        }
    }

    #[test]
    fn borrowed_live_queries_do_not_allocate_and_select_fields_independently() {
        let long_label = b"_x".repeat(256);
        let long_key = b"X".repeat(256);
        let mut records: Vec<_> = (0..128)
            .map(|_| record(b"other", b"m_+030", -6., Some(false)))
            .collect();
        records.extend([
            record(b"bed", b"", -12., Some(false)),
            record(b"bed", b"", 0., Some(true)),
            record(b"bed", b"M+030", -12., Some(false)),
            record(b"bed", b"m_+030 \xc3\xa9", -6., Some(true)),
            record(b"bed", b"M+030", -6., None),
            record(b"bed", b"M-030", 0., Some(false)),
            record(b"bed", &long_label, 0., Some(false)),
        ]);
        // A gain-only channel mute must not shadow the last explicit head-lock value.
        records[132].mute = 1;
        let cases: &[(&[u8], f32, u32)] = &[
            (b"M+030", 0., 1),
            (b"M-030", 1., 0),
            (b"", 1., 1),
            (b"L", 1., 1), // live labels are canonicalized, not alias-resolved
            (&long_key, 1., 0),
        ];
        let mut correct = true;
        COUNT.with(|c| c.set(Some(0)));
        for _ in 0..100 {
            for &(key, expected_gain, expected_head) in cases {
                for fields in 0..4 {
                    let (mut gain, mut head) = (17., 19);
                    let flags = unsafe {
                        mradm_render_live_override(
                            records.as_ptr(),
                            records.len(),
                            Text::bytes(b"bed"),
                            Text::bytes(key),
                            if fields & 1 != 0 {
                                &mut gain
                            } else {
                                ptr::null_mut()
                            },
                            if fields & 2 != 0 {
                                &mut head
                            } else {
                                ptr::null_mut()
                            },
                        )
                    };
                    correct &= flags == fields
                        && gain == if fields & 1 != 0 { expected_gain } else { 17. }
                        && head == if fields & 2 != 0 { expected_head } else { 19 };
                }
            }
        }
        let allocations = COUNT.with(|c| c.replace(None).unwrap());
        assert!(correct, "live precedence and nullable outputs");
        assert_eq!(
            allocations, 0,
            "including the first lookup and label folding"
        );
    }

    #[test]
    fn live_queries_preserve_outputs_when_a_later_record_is_invalid() {
        let mut records = [
            record(b"bed", b"", 0., Some(true)),
            record(b"other", b"", 0., None),
        ];
        records[1].speaker_label = Text {
            data: ptr::null(),
            len: 1,
        };
        let (mut gain, mut head) = (17., 19);
        let flags = unsafe {
            mradm_render_live_override(
                records.as_ptr(),
                records.len(),
                Text::bytes(b"bed"),
                Text::bytes(b"M+030"),
                &mut gain,
                &mut head,
            )
        };
        assert_eq!((flags, gain, head), (0, 17., 19));
        records[1] = record(b"other", b"", 0., None);
        let flags = unsafe {
            mradm_render_live_override(
                records.as_ptr(),
                records.len(),
                Text::bytes(b"missing"),
                Text::bytes(b"M+030"),
                &mut gain,
                &mut head,
            )
        };
        assert_eq!((flags, gain, head), (0, 17., 19));
    }
}
