//! Offline renders and their results: `adm_render_file(_ex, _ex2)`, the `adm_render_result_t`
//! accessors, the preview session (`adm_create_preview_session`, `adm_preview_render_window(_v2)`),
//! `adm_export_file` and the discovery documents (`adm_capabilities_json` and the others).
//!
//! The render drives the same engine step machine the C++ `RenderService` drove
//! (`mradm_engine_render_begin` / `_next` / `_resume` / `_result`), with the C++ islands
//! (`mradm_audio_island`, the Apple islands) run inside each step, the caller's cancellation polled
//! before every step and by the islands, and the progress and log lines forwarded as before.
use crate::cancel::Flag;
use crate::context::adm_context_t;
use crate::error::{
    Error, Failure, c_str, caught, clear, concat, report, set, try_box, until_nul, utf8_path,
};
use crate::guard;
use crate::options::adm_render_options_t;
use crate::query::{Scene, axml, check_view, import_with};
use crate::string;
use mradm_ffi::adm::Text;
use mradm_ffi::engine::{
    EngineResult, EngineStatus, PlatformIslands, mradm_engine_document,
    mradm_engine_result_destroy, mradm_engine_text_destroy,
};
use mradm_ffi::engine_render::mradm_render_prepared_destroy;
use mradm_ffi::engine_service::{
    EngineIslandReply, EnginePath, EngineProgress, EngineRender, EngineRenderEvent,
    EngineRenderResult, IslandContext, IslandRunner, mradm_engine_apply_policy,
    mradm_engine_render_begin, mradm_engine_render_destroy, mradm_engine_render_next,
    mradm_engine_render_result, mradm_engine_render_resume, mradm_engine_render_set_islands,
    mradm_engine_render_take_prepared,
};
use mradm_ffi::render::RenderPrepared;
use mradm_ffi::render::apple_island::AppleIslands;
use mradm_ffi::scene::mradm_scene_export_axml;
use mradm_ffi::wav::mradm_wav_replace_axml;
use std::ffi::{c_char, c_void};
use std::mem::MaybeUninit;
use std::ptr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};

// ── The C++ islands ──────────────────────────────────────────────────────────────

#[cfg(not(test))]
mod islands {
    use super::*;
    unsafe extern "C" {
        pub(super) fn mradm_platform_islands() -> *const PlatformIslands;
        pub(super) fn mradm_audio_island(
            request: *const EngineRenderEvent,
            context: *const IslandContext,
            reply: *mut EngineIslandReply,
        ) -> i32;
    }
    #[cfg(target_os = "macos")]
    unsafe extern "C" {
        fn mradm_apple_islands() -> *const AppleIslands;
    }
    pub(super) fn apple() -> *const AppleIslands {
        #[cfg(target_os = "macos")]
        return unsafe { mradm_apple_islands() };
        #[cfg(not(target_os = "macos"))]
        ptr::null()
    }
    // The caller's progress callbacks, called from C++ (`adm_c_api.cpp`) so that an exception a C++
    // caller throws from one stays on the C++ side: 1 when it threw.
    unsafe extern "C" {
        pub(super) fn mradm_capi_progress_v1(
            callback: ProgressV1,
            fraction: f64,
            stage: *const c_char,
            message: *const c_char,
            user: *mut c_void,
        ) -> u32;
        pub(super) fn mradm_capi_progress_v2(
            callback: ProgressV2,
            event: *const adm_progress_event_v2_t,
            user: *mut c_void,
        ) -> u32;
    }
}

// Unit tests link without the C++ islands.
#[cfg(test)]
mod islands {
    use super::*;
    pub(super) unsafe fn mradm_platform_islands() -> *const PlatformIslands {
        ptr::null()
    }
    pub(super) unsafe extern "C" fn mradm_audio_island(
        _: *const EngineRenderEvent,
        _: *const IslandContext,
        _: *mut EngineIslandReply,
    ) -> i32 {
        1
    }
    pub(super) fn apple() -> *const AppleIslands {
        ptr::null()
    }
    pub(super) unsafe fn mradm_capi_progress_v1(
        callback: ProgressV1,
        fraction: f64,
        stage: *const c_char,
        message: *const c_char,
        user: *mut c_void,
    ) -> u32 {
        unsafe { callback(fraction, stage, message, user) };
        0
    }
    pub(super) unsafe fn mradm_capi_progress_v2(
        callback: ProgressV2,
        event: *const adm_progress_event_v2_t,
        user: *mut c_void,
    ) -> u32 {
        unsafe { callback(event, user) };
        0
    }
}

// ── Progress and logs ────────────────────────────────────────────────────────────

type ProgressV1 = unsafe extern "C" fn(f64, *const c_char, *const c_char, *mut c_void);
/// `adm_progress_cb`.
#[allow(non_camel_case_types)]
pub type adm_progress_cb =
    Option<unsafe extern "C" fn(f64, *const c_char, *const c_char, *mut c_void)>;

/// `adm_progress_event_v2_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_progress_event_v2_t {
    struct_size: u32,
    stage: i32,
    operation: i32,
    overall_fraction: f64,
    stage_fraction: f64,
    current_frame: u64,
    total_frames: u64,
    message: *const c_char,
}

type ProgressV2 = unsafe extern "C" fn(*const adm_progress_event_v2_t, *mut c_void);
/// `adm_progress_v2_cb`.
#[allow(non_camel_case_types)]
pub type adm_progress_v2_cb =
    Option<unsafe extern "C" fn(*const adm_progress_event_v2_t, *mut c_void)>;

/// The caller's progress callback (`CallbackProgressSink` / `CallbackProgressSinkV2`).
#[derive(Clone, Copy)]
enum Progress {
    V1(Option<ProgressV1>, *mut c_void),
    V2(Option<ProgressV2>, *mut c_void),
}

/// `RenderStage` names, the progress contract `adm_render_stage_from_string` reads.
const STAGES: [&[u8]; 7] = [
    b"validating\0",
    b"probing\0",
    b"importing_scene\0",
    b"planning\0",
    b"rendering\0",
    b"post_processing\0",
    b"finished\0",
];

fn clamp_fraction(value: f64) -> f64 {
    if value.is_finite() {
        value.clamp(0.0, 1.0)
    } else {
        0.0
    }
}

fn bytes<'a>(t: &Text) -> &'a [u8] {
    if t.len == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(t.data, t.len) }
    }
}

impl Progress {
    /// Delivers one event; `Err` where the C++ message copy or the caller's callback threw.
    fn deliver(&self, p: &EngineProgress) -> Result<(), Failure> {
        let message = || concat(&[bytes(&p.message), b"\0"]);
        match *self {
            Progress::V1(None, _) | Progress::V2(None, _) => Ok(()),
            Progress::V1(Some(callback), user) => {
                let message = message()?;
                let stage = STAGES
                    .get(p.stage as usize)
                    .copied()
                    .unwrap_or(b"unknown\0");
                let threw = unsafe {
                    islands::mradm_capi_progress_v1(
                        callback,
                        clamp_fraction(p.fraction),
                        stage.as_ptr().cast(),
                        message.as_ptr().cast(),
                        user,
                    )
                };
                if threw != 0 {
                    Err(Failure::Exception)
                } else {
                    Ok(())
                }
            }
            Progress::V2(Some(callback), user) => {
                let message = message()?;
                let event = adm_progress_event_v2_t {
                    struct_size: size_of::<adm_progress_event_v2_t>() as u32,
                    stage: if p.stage <= 6 { p.stage as i32 + 1 } else { 0 },
                    operation: if p.operation <= 17 {
                        p.operation as i32
                    } else {
                        0
                    },
                    overall_fraction: clamp_fraction(p.fraction),
                    stage_fraction: clamp_fraction(p.stage_fraction),
                    current_frame: p.current,
                    total_frames: p.total,
                    message: message.as_ptr().cast(),
                };
                let threw = unsafe { islands::mradm_capi_progress_v2(callback, &event, user) };
                if threw != 0 {
                    Err(Failure::Exception)
                } else {
                    Ok(())
                }
            }
        }
    }
}

/// A captured log line (`CLogEntry`), NUL-terminated for the accessors.
struct Log {
    level: i32,
    module: Vec<u8>,
    message: Vec<u8>,
}

/// `CollectingLogSink` when the caller asked for a result, else `NullLogSink`.
struct Logs {
    lines: Option<Mutex<Vec<Log>>>,
}

impl Logs {
    fn log(&self, level: u32, module: &[u8], message: &[u8]) -> Result<(), Failure> {
        let Some(lines) = &self.lines else {
            return Ok(());
        };
        let line = Log {
            level: if level <= 3 { level as i32 } else { 1 },
            module: concat(&[module, b"\0"])?,
            message: concat(&[message, b"\0"])?,
        };
        let mut lines = lines.lock().unwrap_or_else(|e| e.into_inner());
        lines.try_reserve(1).map_err(|_| Failure::Exception)?;
        lines.push(line);
        Ok(())
    }

    fn take(self) -> Vec<Log> {
        self.lines.map_or_else(Vec::new, |l| {
            l.into_inner().unwrap_or_else(|e| e.into_inner())
        })
    }
}

// ── The render ───────────────────────────────────────────────────────────────────

/// `RenderResult`: the error (code 0 on success), output path (UTF-8), metrics and report.
struct Outcome {
    code: i32,
    message: Vec<u8>,
    output_path: Option<Vec<u8>>,
    loudness_lufs: Option<f64>,
    peak_dbtp: Option<f64>,
    report: Option<Vec<u8>>,
}

impl Outcome {
    fn error(code: i32, message: &[u8]) -> Result<Self, Failure> {
        Ok(Self {
            code,
            message: concat(&[message])?,
            output_path: None,
            loudness_lufs: None,
            peak_dbtp: None,
            report: None,
        })
    }
}

/// `error_of`: codes outside 0–6 are `internal_error`.
fn error_code(code: i32) -> i32 {
    if (0..=6).contains(&code) {
        code
    } else {
        guard::INTERNAL
    }
}

/// The caller's side of the islands (`IslandDriver`): cancellation, progress and logs; a failed
/// callback copy stops the islands and ends the render as the C++ rethrow did.
struct Driver<'a> {
    cancel: Option<&'a Flag>,
    progress: Progress,
    logs: &'a Logs,
    failed: AtomicBool,
}

impl Driver<'_> {
    fn cancelled(&self) -> bool {
        self.cancel.is_some_and(Flag::stop_requested)
    }

    fn fail(&self, result: Result<(), Failure>) {
        if result.is_err() {
            self.failed.store(true, Ordering::Release);
        }
    }
}

unsafe extern "C" fn island_cancelled(user: *mut c_void) -> u32 {
    let driver = unsafe { &*user.cast::<Driver<'_>>() };
    u32::from(driver.failed.load(Ordering::Acquire) || driver.cancelled())
}

unsafe extern "C" fn island_progress(user: *mut c_void, event: *const EngineProgress) {
    let driver = unsafe { &*user.cast::<Driver<'_>>() };
    if !driver.failed.load(Ordering::Acquire) {
        driver.fail(driver.progress.deliver(unsafe { &*event }));
    }
}

unsafe extern "C" fn island_log(user: *mut c_void, level: u32, module: Text, message: Text) {
    let driver = unsafe { &*user.cast::<Driver<'_>>() };
    if !driver.failed.load(Ordering::Acquire) {
        driver.fail(driver.logs.log(level, bytes(&module), bytes(&message)));
    }
}

struct RenderHandle(*mut EngineRender);

impl Drop for RenderHandle {
    fn drop(&mut self) {
        unsafe { mradm_engine_render_destroy(self.0) };
    }
}

/// A preview session's cached preparation.
struct Prepared(*mut RenderPrepared);

impl Drop for Prepared {
    fn drop(&mut self) {
        unsafe { mradm_render_prepared_destroy(self.0) };
    }
}

/// `RenderService::render` of `input` (UTF-8) to `output` with `options`.
#[allow(clippy::too_many_arguments)] // The C++ service's arguments.
fn render(
    options: &adm_render_options_t,
    input: &[u8],
    output: Option<&[u8]>,
    cancel: Option<&Flag>,
    progress: Progress,
    logs: &Logs,
    preimported: Option<&Scene>,
    cache: Option<&mut Option<Prepared>>,
) -> Result<Outcome, Failure> {
    let request = options.request(input, output)?;
    let cached = cache
        .as_ref()
        .and_then(|c| c.as_ref())
        .map_or(ptr::null(), |p| p.0.cast_const());
    let mut raw = ptr::null_mut();
    let code = unsafe {
        mradm_engine_render_begin(
            &request.record,
            islands::mradm_platform_islands(),
            preimported.map_or(ptr::null(), |s| s.0.cast_const()),
            cached,
            &mut raw,
        )
    };
    if code != 0 {
        return Outcome::error(
            error_code(code),
            b"invalid render request (UTF-8 paths without NUL required)",
        );
    }
    let handle = RenderHandle(raw);
    let driver = Driver {
        cancel,
        progress,
        logs,
        failed: AtomicBool::new(false),
    };
    let runner = IslandRunner {
        run: Some(islands::mradm_audio_island),
        driver: IslandContext {
            user: ptr::from_ref(&driver).cast_mut().cast(),
            cancelled: Some(island_cancelled),
            progress: Some(island_progress),
            log: Some(island_log),
        },
        apple: islands::apple(),
    };
    if unsafe { mradm_engine_render_set_islands(handle.0, &runner) } != 0 {
        return Outcome::error(guard::INTERNAL, b"render islands unavailable");
    }
    loop {
        let mut event = unsafe { MaybeUninit::<EngineRenderEvent>::zeroed().assume_init() };
        let status = unsafe {
            mradm_engine_render_next(handle.0, u32::from(driver.cancelled()), &mut event)
        };
        if driver.failed.load(Ordering::Acquire) {
            return Err(Failure::Exception);
        }
        if status != 0 {
            return Outcome::error(guard::INTERNAL, b"render step failed");
        }
        match event.kind {
            1 => progress.deliver(&event.progress)?,
            2 => logs.log(event.level, bytes(&event.module), bytes(&event.message))?,
            3 => {
                // Every island runs inside the step; one returned here has no runner in this build.
                let mut reply = unsafe { MaybeUninit::<EngineIslandReply>::zeroed().assume_init() };
                reply.code = guard::INTERNAL;
                reply.message = Text::new("unknown render island");
                reply.ffmpeg_major = -1;
                if unsafe { mradm_engine_render_resume(handle.0, &reply) } != 0 {
                    return Outcome::error(guard::INTERNAL, b"render island reply failed");
                }
            }
            4 => break,
            _ => {}
        }
    }
    if let Some(slot) = cache {
        let made = unsafe { mradm_engine_render_take_prepared(handle.0) };
        if !made.is_null() {
            *slot = Some(Prepared(made));
        }
    }
    let mut r = unsafe { MaybeUninit::<EngineRenderResult>::zeroed().assume_init() };
    if unsafe { mradm_engine_render_result(handle.0, &mut r) } != 0 {
        return Outcome::error(guard::INTERNAL, b"render result is unavailable");
    }
    let has_metrics = r.has_metrics != 0;
    Ok(Outcome {
        code: error_code(r.code),
        message: concat(&[bytes(&r.message)])?,
        output_path: if r.has_output != 0 {
            Some(concat(&[bytes(&r.output_path)])?)
        } else {
            None
        },
        loudness_lufs: (has_metrics && r.has_lufs != 0).then_some(r.lufs),
        peak_dbtp: (has_metrics && r.has_peak != 0).then_some(r.peak_dbtp),
        report: if r.has_report != 0 {
            Some(concat(&[bytes(&r.report), b"\0"])?)
        } else {
            None
        },
    })
}

// ── Results ──────────────────────────────────────────────────────────────────────

/// `adm_render_result_t`: NUL-terminated texts for the borrowed accessors.
#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_render_result_t {
    code: i32,
    message: Vec<u8>,
    output_path: Vec<u8>,
    loudness_lufs: Option<f64>,
    peak_dbtp: Option<f64>,
    logs: Vec<Log>,
    report: Option<Vec<u8>>,
}

/// `make_c_result`; `Err` where the C++ conversion threw (the native output path).
fn result_of(outcome: Outcome, logs: Vec<Log>) -> Result<adm_render_result_t, Failure> {
    let output_path = match &outcome.output_path {
        Some(utf8) => concat(&[&native_path(utf8).ok_or(Failure::Exception)?, b"\0"])?,
        None => concat(&[b"\0"])?,
    };
    Ok(adm_render_result_t {
        code: outcome.code,
        message: concat(&[&outcome.message, b"\0"])?,
        output_path,
        loudness_lufs: outcome.loudness_lufs,
        peak_dbtp: outcome.peak_dbtp,
        logs,
        report: outcome.report,
    })
}

/// `path.string()` of a UTF-8 path: Windows converts to the code page `std::filesystem` uses
/// (failing on unmappable characters as it did); elsewhere the bytes are unchanged.
#[cfg(not(windows))]
fn native_path(utf8: &[u8]) -> Option<Vec<u8>> {
    concat(&[utf8]).ok()
}

#[cfg(windows)]
fn native_path(utf8: &[u8]) -> Option<Vec<u8>> {
    unsafe extern "system" {
        fn AreFileApisANSI() -> i32;
        fn MultiByteToWideChar(
            code_page: u32,
            flags: u32,
            text: *const u8,
            len: i32,
            wide: *mut u16,
            wide_len: i32,
        ) -> i32;
        fn WideCharToMultiByte(
            code_page: u32,
            flags: u32,
            wide: *const u16,
            wide_len: i32,
            text: *mut u8,
            len: i32,
            default_char: *const u8,
            used_default: *mut i32,
        ) -> i32;
    }
    unsafe extern "C" {
        fn ___lc_codepage_func() -> u32;
    }
    const CP_UTF8: u32 = 65001;
    const MB_ERR_INVALID_CHARS: u32 = 8;
    const WC_NO_BEST_FIT_CHARS: u32 = 0x400;
    if utf8.is_empty() {
        return Some(Vec::new());
    }
    let len = i32::try_from(utf8.len()).ok()?;
    let count = unsafe {
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8.as_ptr(),
            len,
            ptr::null_mut(),
            0,
        )
    };
    let mut wide = Vec::new();
    wide.try_reserve_exact(usize::try_from(count).ok().filter(|&n| n != 0)?)
        .ok()?;
    wide.resize(count as usize, 0u16);
    if unsafe {
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8.as_ptr(),
            len,
            wide.as_mut_ptr(),
            count,
        )
    } != count
    {
        return None;
    }
    let code_page = unsafe {
        if ___lc_codepage_func() == CP_UTF8 {
            return concat(&[utf8]).ok();
        } else if AreFileApisANSI() == 0 {
            1
        } else {
            0
        }
    };
    let mut used_default = 0i32;
    let size = unsafe {
        WideCharToMultiByte(
            code_page,
            WC_NO_BEST_FIT_CHARS,
            wide.as_ptr(),
            count,
            ptr::null_mut(),
            0,
            ptr::null(),
            &mut used_default,
        )
    };
    if size <= 0 || used_default != 0 {
        return None;
    }
    let mut narrow = Vec::new();
    narrow.try_reserve_exact(size as usize).ok()?;
    narrow.resize(size as usize, 0u8);
    let written = unsafe {
        WideCharToMultiByte(
            code_page,
            WC_NO_BEST_FIT_CHARS,
            wide.as_ptr(),
            count,
            narrow.as_mut_ptr(),
            size,
            ptr::null(),
            &mut used_default,
        )
    };
    (written == size && used_default == 0).then_some(narrow)
}

/// The result handle the caller asked for, and the code to return (`render_file_impl`'s tail).
unsafe fn finish(outcome: Outcome, logs: Logs, result: *mut *mut adm_render_result_t) -> i32 {
    let code = outcome.code;
    if result.is_null() {
        return code;
    }
    let Ok(converted) = result_of(outcome, logs.take()) else {
        return guard::INTERNAL;
    };
    let handle = try_box(converted);
    if handle.is_null() {
        return guard::INTERNAL;
    }
    unsafe { *result = handle };
    code
}

fn logs_for(result: *mut *mut adm_render_result_t) -> Logs {
    Logs {
        lines: (!result.is_null()).then(|| Mutex::new(Vec::new())),
    }
}

/// The options of a render: the caller's, or `RenderOptions{}` (on the stack, as the C++).
#[allow(clippy::large_enum_variant)]
enum Options<'a> {
    Caller(&'a adm_render_options_t),
    Default(adm_render_options_t),
}

impl Options<'_> {
    fn get(&self) -> &adm_render_options_t {
        match self {
            Options::Caller(o) => o,
            Options::Default(o) => o,
        }
    }
}

unsafe fn options_of<'a>(opts: *const adm_render_options_t) -> Result<Options<'a>, Failure> {
    match unsafe { opts.as_ref() } {
        Some(o) => Ok(Options::Caller(o)),
        None => adm_render_options_t::defaults()
            .map(Options::Default)
            .ok_or(Failure::Exception),
    }
}

unsafe fn render_file_impl(
    context: *const adm_context_t,
    input_path: *const c_char,
    output_path: *const c_char,
    opts: *const adm_render_options_t,
    progress: Progress,
    result: *mut *mut adm_render_result_t,
) -> i32 {
    if !result.is_null() {
        unsafe { *result = ptr::null_mut() };
    }
    if context.is_null() || input_path.is_null() || unsafe { *input_path } == 0 {
        return guard::INVALID_ARGUMENT;
    }
    guard::guard(guard::INTERNAL, || unsafe {
        let run = || -> Result<i32, Failure> {
            let input = utf8_path(c_str(input_path)).ok_or(Failure::Exception)?;
            let output = if output_path.is_null() || *output_path == 0 {
                None
            } else {
                Some(utf8_path(c_str(output_path)).ok_or(Failure::Exception)?)
            };
            let options = options_of(opts)?;
            let options = options.get();
            let cancel = options.cancel_token.as_ref().map(|t| t.take());
            let logs = logs_for(result);
            let outcome = render(
                options,
                &input,
                output.as_deref(),
                cancel.as_ref(),
                progress,
                &logs,
                None,
                None,
            )?;
            Ok(finish(outcome, logs, result))
        };
        run().unwrap_or(guard::INTERNAL)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_file_ex(
    context: *mut adm_context_t,
    input_path: *const c_char,
    output_path: *const c_char,
    opts: *const adm_render_options_t,
    progress: adm_progress_cb,
    user_data: *mut c_void,
    result: *mut *mut adm_render_result_t,
) -> i32 {
    unsafe {
        render_file_impl(
            context,
            input_path,
            output_path,
            opts,
            Progress::V1(progress, user_data),
            result,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_file_ex2(
    context: *mut adm_context_t,
    input_path: *const c_char,
    output_path: *const c_char,
    opts: *const adm_render_options_t,
    progress: adm_progress_v2_cb,
    user_data: *mut c_void,
    result: *mut *mut adm_render_result_t,
) -> i32 {
    unsafe {
        render_file_impl(
            context,
            input_path,
            output_path,
            opts,
            Progress::V2(progress, user_data),
            result,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_file(
    context: *mut adm_context_t,
    input_path: *const c_char,
    output_path: *const c_char,
    progress: adm_progress_cb,
    user_data: *mut c_void,
    result: *mut *mut adm_render_result_t,
) -> i32 {
    unsafe {
        adm_render_file_ex(
            context,
            input_path,
            output_path,
            ptr::null(),
            progress,
            user_data,
            result,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_render_result(result: *mut adm_render_result_t) {
    if !result.is_null() {
        drop(unsafe { Box::from_raw(result) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_result_error_code(result: *const adm_render_result_t) -> i32 {
    unsafe { result.as_ref() }.map_or(guard::INVALID_ARGUMENT, |r| r.code)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_result_message(
    result: *const adm_render_result_t,
) -> *const c_char {
    match unsafe { result.as_ref() } {
        Some(r) => r.message.as_ptr().cast(),
        None => c"result is null".as_ptr(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_result_output_path(
    result: *const adm_render_result_t,
) -> *const c_char {
    unsafe { result.as_ref() }.map_or(ptr::null(), |r| r.output_path.as_ptr().cast())
}

unsafe fn metric(value: Option<f64>, out_value: *mut f64) -> i32 {
    let Some(v) = value else {
        return 0;
    };
    if let Some(out) = unsafe { out_value.as_mut() } {
        *out = v;
    }
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_result_loudness_lufs(
    result: *const adm_render_result_t,
    out_value: *mut f64,
) -> i32 {
    unsafe { metric(result.as_ref().and_then(|r| r.loudness_lufs), out_value) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_result_peak_dbtp(
    result: *const adm_render_result_t,
    out_value: *mut f64,
) -> i32 {
    unsafe { metric(result.as_ref().and_then(|r| r.peak_dbtp), out_value) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_result_semantic_report_json(
    result: *const adm_render_result_t,
) -> *const c_char {
    unsafe { result.as_ref() }
        .and_then(|r| r.report.as_ref())
        .map_or(ptr::null(), |r| r.as_ptr().cast())
}

/// Saturates rather than truncates.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_result_log_count(result: *const adm_render_result_t) -> u32 {
    unsafe { result.as_ref() }.map_or(0, |r| u32::try_from(r.logs.len()).unwrap_or(u32::MAX))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_result_log_entry(
    result: *const adm_render_result_t,
    index: u32,
    out_level: *mut i32,
    out_module: *mut *const c_char,
    out_message: *mut *const c_char,
) -> i32 {
    let Some(entry) = unsafe { result.as_ref() }.and_then(|r| r.logs.get(index as usize)) else {
        return 0;
    };
    unsafe {
        if let Some(l) = out_level.as_mut() {
            *l = entry.level;
        }
        if let Some(m) = out_module.as_mut() {
            *m = entry.module.as_ptr().cast();
        }
        if let Some(m) = out_message.as_mut() {
            *m = entry.message.as_ptr().cast();
        }
    }
    1
}

// ── Policy, preview and export ───────────────────────────────────────────────────

struct PolicyResult(*mut EngineResult);

impl Drop for PolicyResult {
    fn drop(&mut self) {
        unsafe { mradm_engine_result_destroy(self.0) };
    }
}

/// `apply_policy`: the options' semantic policy on an imported scene (its log lines discarded).
fn apply_policy(
    scene: &Scene,
    input: &[u8],
    options: &adm_render_options_t,
) -> Result<(), Failure> {
    let json = options.semantic_policy_json.as_deref();
    let path = options
        .semantic_policy_path
        .as_ref()
        .map_or(&[][..], |p| &p.utf8[..]);
    let mut raw = ptr::null_mut();
    let mut status = unsafe { MaybeUninit::<EngineStatus>::zeroed().assume_init() };
    let code = unsafe {
        mradm_engine_apply_policy(
            scene.0,
            u32::from(json.is_some()),
            Text::bytes(json.unwrap_or(&[])),
            u32::from(options.semantic_policy_path.is_some()),
            EnginePath {
                utf8: Text::bytes(path),
                display: Text::bytes(path),
            },
            Text::bytes(input),
            &mut raw,
            &mut status,
        )
    };
    let result = PolicyResult(raw);
    if code != 0 || result.0.is_null() {
        return Err(Failure::Exception);
    }
    if status.code != 0 {
        return Err(
            Error::new(status.code, bytes(&status.message), bytes(&status.context))?.into(),
        );
    }
    Ok(())
}

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_preview_session_t {
    input: Vec<u8>,
    options: adm_render_options_t,
    cancel: Option<Flag>,
    scene: Scene,
    prepared: Option<Prepared>,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_preview_session(
    context: *mut adm_context_t,
    input_path: *const c_char,
    opts: *const adm_render_options_t,
    out: *mut *mut adm_preview_session_t,
) -> i32 {
    if !out.is_null() {
        unsafe { *out = ptr::null_mut() };
    }
    if context.is_null() || input_path.is_null() || unsafe { *input_path } == 0 || out.is_null() {
        return guard::INVALID_ARGUMENT;
    }
    const UNEXPECTED: Option<&str> = Some("unexpected exception while creating preview session");
    caught(context, UNEXPECTED, || unsafe {
        clear(context);
        let created = (|| -> Result<adm_preview_session_t, Failure> {
            let native = c_str(input_path);
            let options = match opts.as_ref() {
                Some(o) => o.duplicate(),
                None => adm_render_options_t::defaults(),
            }
            .ok_or(Failure::Exception)?;
            let cancel = opts
                .as_ref()
                .and_then(|o| o.cancel_token.as_ref())
                .map(|t| t.take());
            let scene = import_with(
                native,
                true,
                options.input_layout(),
                options.input_channel_labels(),
            )?;
            check_view(&scene, native)?;
            let input = utf8_path(native).ok_or(Failure::Exception)?;
            apply_policy(&scene, &input, &options)?;
            Ok(adm_preview_session_t {
                input,
                options,
                cancel,
                scene,
                prepared: None,
            })
        })();
        let session = match created {
            Ok(session) => session,
            Err(failure) => return report(context, failure, UNEXPECTED),
        };
        let handle = try_box(session);
        if handle.is_null() {
            set(context, b"failed to allocate preview session handle");
            return guard::INTERNAL;
        }
        *out = handle;
        guard::OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_preview_session(session: *mut adm_preview_session_t) {
    if !session.is_null() {
        drop(unsafe { Box::from_raw(session) });
    }
}

unsafe fn preview_render_window_impl(
    session: *mut adm_preview_session_t,
    start_sec: f64,
    end_sec: f64,
    output_path: *const c_char,
    progress: Progress,
    result: *mut *mut adm_render_result_t,
) -> i32 {
    if !result.is_null() {
        unsafe { *result = ptr::null_mut() };
    }
    let Some(session) = (unsafe { session.as_mut() }) else {
        return guard::INVALID_ARGUMENT;
    };
    if !start_sec.is_finite() || start_sec < 0.0 || !end_sec.is_finite() {
        return guard::INVALID_ARGUMENT;
    }
    guard::guard(guard::INTERNAL, || unsafe {
        let mut run = || -> Result<i32, Failure> {
            let output = if output_path.is_null() || *output_path == 0 {
                None
            } else {
                Some(utf8_path(c_str(output_path)).ok_or(Failure::Exception)?)
            };
            let saved = (
                session.options.render_start_sec,
                session.options.render_end_sec,
            );
            session.options.render_start_sec = start_sec;
            session.options.render_end_sec = (end_sec > 0.0).then_some(end_sec);
            let logs = logs_for(result);
            let outcome = render(
                &session.options,
                &session.input,
                output.as_deref(),
                session.cancel.as_ref(),
                progress,
                &logs,
                Some(&session.scene),
                Some(&mut session.prepared),
            );
            (
                session.options.render_start_sec,
                session.options.render_end_sec,
            ) = saved;
            Ok(finish(outcome?, logs, result))
        };
        run().unwrap_or(guard::INTERNAL)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_preview_render_window(
    session: *mut adm_preview_session_t,
    start_sec: f64,
    end_sec: f64,
    output_path: *const c_char,
    progress: adm_progress_cb,
    user_data: *mut c_void,
    result: *mut *mut adm_render_result_t,
) -> i32 {
    unsafe {
        preview_render_window_impl(
            session,
            start_sec,
            end_sec,
            output_path,
            Progress::V1(progress, user_data),
            result,
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_preview_render_window_v2(
    session: *mut adm_preview_session_t,
    start_sec: f64,
    end_sec: f64,
    output_path: *const c_char,
    progress: adm_progress_v2_cb,
    user_data: *mut c_void,
    result: *mut *mut adm_render_result_t,
) -> i32 {
    unsafe {
        preview_render_window_impl(
            session,
            start_sec,
            end_sec,
            output_path,
            Progress::V2(progress, user_data),
            result,
        )
    }
}

/// `RenderService::export_file`: the input's AXML rewritten with the options' policy.
fn export(input: &[u8], output: &[u8], options: &adm_render_options_t) -> Result<(), Failure> {
    let scene = import_with(input, false, None, &[])?;
    check_view(&scene, input)?;
    let display = utf8_path(input).ok_or(Failure::Exception)?;
    apply_policy(&scene, &display, options)?;
    let xml = axml(input)?;
    let locator = concat(&[b"input=", input, b" output=", output])?;
    let mut message = [0u8; 1024];
    let mut text = Text::bytes(&[]);
    let code = unsafe {
        mradm_scene_export_axml(
            scene.0,
            xml.as_ptr(),
            xml.len(),
            &mut text,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    if code != 0 {
        return Err(Error::new(code, until_nul(&message), &locator)?.into());
    }
    let rewritten = concat(&[bytes(&text)])?;
    let source = utf8_path(input).ok_or(Failure::Exception)?;
    let target = utf8_path(output).ok_or(Failure::Exception)?;
    let mut message = [0u8; 512];
    let mut context = [0u8; 1024];
    let code = unsafe {
        mradm_wav_replace_axml(
            source.as_ptr(),
            source.len(),
            target.as_ptr(),
            target.len(),
            rewritten.as_ptr(),
            rewritten.len(),
            message.as_mut_ptr(),
            message.len(),
            context.as_mut_ptr(),
            context.len(),
        )
    };
    if code != 0 {
        return Err(Error::clamped(code, until_nul(&message), &locator)?.into());
    }
    Ok(())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_export_file(
    context: *mut adm_context_t,
    input_path: *const c_char,
    output_path: *const c_char,
    opts: *const adm_render_options_t,
) -> i32 {
    let empty = |p: *const c_char| p.is_null() || unsafe { *p } == 0;
    if context.is_null() || empty(input_path) || empty(output_path) {
        return guard::INVALID_ARGUMENT;
    }
    const UNEXPECTED: Option<&str> = Some("unexpected exception while exporting ADM file");
    caught(context, UNEXPECTED, || unsafe {
        clear(context);
        let exported =
            options_of(opts).and_then(|o| export(c_str(input_path), c_str(output_path), o.get()));
        match exported {
            Ok(()) => guard::OK,
            Err(failure) => report(context, failure, UNEXPECTED),
        }
    })
}

// ── Discovery documents ──────────────────────────────────────────────────────────

/// `engine::document`: the discovery document of `kind` as an owned string.
unsafe fn document(context: *mut adm_context_t, kind: u32, out_json: *mut *mut c_char) -> i32 {
    if !out_json.is_null() {
        unsafe { *out_json = ptr::null_mut() };
    }
    if context.is_null() || out_json.is_null() {
        return guard::INVALID_ARGUMENT;
    }
    guard::code(|| unsafe {
        let mut raw = ptr::null_mut();
        let mut json = Text::bytes(&[]);
        if mradm_engine_document(kind, islands::mradm_platform_islands(), &mut raw, &mut json) != 0
        {
            return guard::INTERNAL;
        }
        let copy = string::copy(json.data, json.len);
        mradm_engine_text_destroy(raw);
        if copy.is_null() {
            return guard::INTERNAL;
        }
        *out_json = copy;
        guard::OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_capabilities_json(
    context: *mut adm_context_t,
    out_json: *mut *mut c_char,
) -> i32 {
    unsafe { document(context, 0, out_json) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_layouts_json(
    context: *mut adm_context_t,
    out_json: *mut *mut c_char,
) -> i32 {
    unsafe { document(context, 1, out_json) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_output_formats_json(
    context: *mut adm_context_t,
    out_json: *mut *mut c_char,
) -> i32 {
    unsafe { document(context, 2, out_json) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_input_layouts_json(
    context: *mut adm_context_t,
    out_json: *mut *mut c_char,
) -> i32 {
    unsafe { document(context, 3, out_json) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_support_matrix_json(
    context: *mut adm_context_t,
    out_json: *mut *mut c_char,
) -> i32 {
    unsafe { document(context, 4, out_json) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fractions_clamp_as_the_cpp_did() {
        assert_eq!(clamp_fraction(f64::NAN), 0.0);
        assert_eq!(clamp_fraction(-0.5), 0.0);
        assert_eq!(clamp_fraction(1.5), 1.0);
        assert_eq!(clamp_fraction(0.25), 0.25);
    }

    #[test]
    fn null_results_answer_as_the_cpp_did() {
        unsafe {
            assert_eq!(
                adm_render_result_error_code(ptr::null()),
                guard::INVALID_ARGUMENT
            );
            assert_eq!(
                std::ffi::CStr::from_ptr(adm_render_result_message(ptr::null())),
                c"result is null"
            );
            assert!(adm_render_result_output_path(ptr::null()).is_null());
            assert_eq!(adm_render_result_log_count(ptr::null()), 0);
            let mut v = 0.0;
            assert_eq!(adm_render_result_loudness_lufs(ptr::null(), &mut v), 0);
            assert_eq!(
                adm_render_result_log_entry(
                    ptr::null(),
                    0,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    ptr::null_mut()
                ),
                0
            );
            assert_eq!(
                adm_render_file(
                    ptr::null_mut(),
                    c"a.wav".as_ptr(),
                    ptr::null(),
                    None,
                    ptr::null_mut(),
                    ptr::null_mut()
                ),
                guard::INVALID_ARGUMENT
            );
            assert_eq!(
                adm_preview_render_window(
                    ptr::null_mut(),
                    0.0,
                    0.0,
                    ptr::null(),
                    None,
                    ptr::null_mut(),
                    ptr::null_mut()
                ),
                guard::INVALID_ARGUMENT
            );
        }
    }

    #[test]
    fn log_levels_outside_the_enum_are_info() {
        let logs = Logs {
            lines: Some(Mutex::new(Vec::new())),
        };
        logs.log(3, b"m", b"x").unwrap();
        logs.log(9, b"m", b"y").unwrap();
        let lines = logs.take();
        assert_eq!((lines[0].level, lines[1].level), (3, 1));
        assert_eq!(lines[1].message, b"y\0");
    }
}
