//! Queries of an input file: `adm_probe_file` and its `adm_scene_info_t`, `adm_inspect_file_json`,
//! `adm_inspect_file_xml` and `adm_policy_template_json`, plus `adm_render_stage_from_string`.
//!
//! Each export makes the calls the C++ service made (the scene import, the WAVE readers), with the
//! same message buffers, so messages, locators and truncation are unchanged.
use crate::context::adm_context_t;
use crate::error::{
    Error, Failure, c_str, caught, clear, concat, report, set, try_box, until_nul, utf8_path,
};
use crate::guard;
use crate::string;
use mradm_ffi::adm::{Text, View};
use mradm_ffi::scene::{
    SceneHandle, SceneImportInfo, mradm_scene_destroy, mradm_scene_import,
    mradm_scene_inspect_json, mradm_scene_template, mradm_scene_view,
};
use mradm_ffi::wav::{
    ChunkReadHandle, Info, ReadHandle, mradm_wav_chunks_chna, mradm_wav_chunks_destroy,
    mradm_wav_chunks_open, mradm_wav_chunks_read, mradm_wav_reader_destroy, mradm_wav_reader_open,
};
use std::ffi::c_char;
use std::mem::MaybeUninit;
use std::ptr;

/// `adm_render_stage_t` of a progress stage name; `ADM_STAGE_UNKNOWN` (0) otherwise.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_render_stage_from_string(stage: *const c_char) -> i32 {
    if stage.is_null() {
        return 0;
    }
    match unsafe { c_str(stage) } {
        b"validating" => 1,
        b"probing" => 2,
        b"importing_scene" => 3,
        b"planning" => 4,
        b"rendering" => 5,
        b"post_processing" => 6,
        b"finished" => 7,
        _ => 0,
    }
}

/// An imported scene, destroyed with its handle.
pub(crate) struct Scene(pub(crate) *mut SceneHandle);

impl Drop for Scene {
    fn drop(&mut self) {
        unsafe { mradm_scene_destroy(self.0) };
    }
}

const EMPTY: Text = Text {
    data: ptr::null(),
    len: 0,
};

/// `RenderScene::import` without a layout or labels; `routed` is whether the caller passed
/// import options (the template does not).
fn import(path: &[u8], routed: bool) -> Result<Scene, Failure> {
    import_with(path, routed, None, &[])
}

/// `RenderScene::import` with the caller's input layout and channel labels (a preview session).
pub(crate) fn import_with(
    path: &[u8],
    routed: bool,
    layout: Option<&[u8]>,
    labels: &[Vec<u8>],
) -> Result<Scene, Failure> {
    let utf8 = utf8_path(path).ok_or(Failure::Exception)?;
    let mut views = Vec::new();
    views
        .try_reserve_exact(labels.len())
        .map_err(|_| Failure::Exception)?;
    views.extend(labels.iter().map(|l| Text::bytes(l)));
    let mut message = [0u8; 1024];
    let mut context = 0u32;
    let mut raw = ptr::null_mut();
    let code = unsafe {
        mradm_scene_import(
            utf8.as_ptr(),
            utf8.len(),
            u32::from(routed),
            u32::from(layout.is_some()),
            layout.map_or(EMPTY, Text::bytes),
            if views.is_empty() {
                ptr::null()
            } else {
                views.as_ptr()
            },
            views.len(),
            &mut raw,
            &mut context,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    let scene = Scene(raw);
    if code != 0 {
        return Err(scene_error(code, &message, context, path)?.into());
    }
    Ok(scene)
}

/// `scene_error`: the locator by the import's context (1 the input, 2 a referenced path).
pub(crate) fn scene_error(
    code: i32,
    message: &[u8],
    context: u32,
    path: &[u8],
) -> Result<Error, Failure> {
    let locator = match context {
        1 => concat(&[b"input=", path])?,
        2 => concat(&[b"path=", path])?,
        _ => Vec::new(),
    };
    Error::clamped(code, until_nul(message), &locator)
}

fn text(t: Text) -> &'static [u8] {
    if t.len == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(t.data, t.len) }
    }
}

/// The scene facts `adm_scene_info_t` holds (`SceneProbe`).
#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_scene_info_t {
    sample_rate: u32,
    channels: u32,
    frames: u64,
    programme_count: u32,
    object_count: u32,
}

/// `RenderService::probe`: the scene's facts, or the raw WAVE facts when the import fails but the
/// file is a readable WAVE with 1–65535 channels.
fn probe(path: &[u8]) -> Result<adm_scene_info_t, Failure> {
    let scene = match import(path, true) {
        Ok(scene) => scene,
        Err(Failure::Error(error)) => return wave_facts(path).ok_or(Failure::Error(error)),
        Err(Failure::Exception) => return Err(Failure::Exception),
    };
    let mut message = [0u8; 1024];
    let mut info = unsafe { MaybeUninit::<SceneImportInfo>::zeroed().assume_init() };
    let mut view = unsafe { MaybeUninit::<View>::zeroed().assume_init() };
    let code = unsafe {
        mradm_scene_view(
            scene.0,
            &mut info,
            &mut view,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    if code != 0 {
        return Err(scene_error(code, &message, 1, path)?.into());
    }
    Ok(adm_scene_info_t {
        sample_rate: info.sample_rate,
        channels: u32::from(info.channels as u16),
        frames: info.frames,
        programme_count: view.programmes_len as u32,
        object_count: view.objects_len as u32,
    })
}

/// `log_import_warnings`: the scene view the warnings come from, for its error only (the preview and
/// export log sinks discard the lines).
pub(crate) fn check_view(scene: &Scene, path: &[u8]) -> Result<(), Failure> {
    let mut message = [0u8; 1024];
    let mut info = unsafe { MaybeUninit::<SceneImportInfo>::zeroed().assume_init() };
    let mut view = unsafe { MaybeUninit::<View>::zeroed().assume_init() };
    let code = unsafe {
        mradm_scene_view(
            scene.0,
            &mut info,
            &mut view,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    if code != 0 {
        return Err(scene_error(code, &message, 1, path)?.into());
    }
    Ok(())
}

fn wave_facts(path: &[u8]) -> Option<adm_scene_info_t> {
    let utf8 = utf8_path(path)?;
    let mut message = [0u8; 512];
    let mut handle: *mut ReadHandle = ptr::null_mut();
    let mut info = Info::default();
    let code = unsafe {
        mradm_wav_reader_open(
            utf8.as_ptr(),
            utf8.len(),
            &mut handle,
            &mut info,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    unsafe { mradm_wav_reader_destroy(handle) };
    (code == 0 && info.channels != 0 && info.channels <= u32::from(u16::MAX)).then_some(
        adm_scene_info_t {
            sample_rate: info.rate,
            channels: info.channels,
            frames: info.frames,
            programme_count: 0,
            object_count: 0,
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_probe_file(
    context: *mut adm_context_t,
    input_path: *const c_char,
    out: *mut *mut adm_scene_info_t,
) -> i32 {
    if !out.is_null() {
        unsafe { *out = ptr::null_mut() };
    }
    if context.is_null() || input_path.is_null() || unsafe { *input_path } == 0 {
        return guard::INVALID_ARGUMENT;
    }
    const UNEXPECTED: Option<&str> = Some("unexpected exception while probing ADM file");
    caught(context, UNEXPECTED, || unsafe {
        clear(context);
        let info = match probe(c_str(input_path)) {
            Ok(info) => info,
            Err(failure) => return report(context, failure, UNEXPECTED),
        };
        if out.is_null() {
            return guard::OK;
        }
        let handle = try_box(info);
        if handle.is_null() {
            set(context, b"failed to allocate scene info handle");
            return guard::INTERNAL;
        }
        *out = handle;
        guard::OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_scene_info(info: *mut adm_scene_info_t) {
    if !info.is_null() {
        drop(unsafe { Box::from_raw(info) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_info_sample_rate(info: *const adm_scene_info_t) -> u32 {
    unsafe { info.as_ref() }.map_or(0, |i| i.sample_rate)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_info_channels(info: *const adm_scene_info_t) -> u32 {
    unsafe { info.as_ref() }.map_or(0, |i| i.channels)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_info_frames(info: *const adm_scene_info_t) -> u64 {
    unsafe { info.as_ref() }.map_or(0, |i| i.frames)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_info_duration_seconds(info: *const adm_scene_info_t) -> f64 {
    match unsafe { info.as_ref() } {
        Some(i) if i.sample_rate != 0 => i.frames as f64 / f64::from(i.sample_rate),
        _ => 0.0,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_info_programme_count(info: *const adm_scene_info_t) -> u32 {
    unsafe { info.as_ref() }.map_or(0, |i| i.programme_count)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_scene_info_object_count(info: *const adm_scene_info_t) -> u32 {
    unsafe { info.as_ref() }.map_or(0, |i| i.object_count)
}

/// The shared shape of the three text queries: argument checks, the query, the validate-only
/// call (NULL output) and the owned copy.
unsafe fn text_query(
    context: *mut adm_context_t,
    input_path: *const c_char,
    out: *mut *mut c_char,
    query: impl FnOnce(&[u8]) -> Result<Vec<u8>, Failure>,
    allocation: &str,
    unexpected: &str,
) -> i32 {
    if !out.is_null() {
        unsafe { *out = ptr::null_mut() };
    }
    if context.is_null() || input_path.is_null() || unsafe { *input_path } == 0 {
        return guard::INVALID_ARGUMENT;
    }
    caught(context, Some(unexpected), || unsafe {
        clear(context);
        let text = match query(c_str(input_path)) {
            Ok(text) => text,
            Err(failure) => return report(context, failure, Some(unexpected)),
        };
        if out.is_null() {
            return guard::OK;
        }
        let copy = string::copy(text.as_ptr(), text.len());
        if copy.is_null() {
            set(context, allocation.as_bytes());
            return guard::INTERNAL;
        }
        *out = copy;
        guard::OK
    })
}

/// An owned copy of a Rust entry's text output.
fn owned(t: Text) -> Result<Vec<u8>, Failure> {
    let bytes = text(t);
    let mut copy = Vec::new();
    copy.try_reserve_exact(bytes.len())
        .map_err(|_| Failure::Exception)?;
    copy.extend_from_slice(bytes);
    Ok(copy)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_inspect_file_json(
    context: *mut adm_context_t,
    input_path: *const c_char,
    out_json: *mut *mut c_char,
) -> i32 {
    unsafe {
        text_query(
            context,
            input_path,
            out_json,
            |path| {
                let scene = import(path, true)?;
                let utf8 = utf8_path(path).ok_or(Failure::Exception)?;
                let mut message = [0u8; 1024];
                let mut output = EMPTY;
                let code = mradm_scene_inspect_json(
                    scene.0,
                    utf8.as_ptr(),
                    utf8.len(),
                    &mut output,
                    message.as_mut_ptr(),
                    message.len(),
                );
                if code != 0 {
                    return Err(Error::new(code, until_nul(&message), b"")?.into());
                }
                owned(output)
            },
            "failed to allocate ADM inspect JSON",
            "unexpected exception while inspecting ADM file",
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_policy_template_json(
    context: *mut adm_context_t,
    input_path: *const c_char,
    out_json: *mut *mut c_char,
) -> i32 {
    unsafe {
        text_query(
            context,
            input_path,
            out_json,
            |path| {
                let scene = import(path, false)?;
                let mut message = [0u8; 1024];
                let mut output = EMPTY;
                let code =
                    mradm_scene_template(scene.0, &mut output, message.as_mut_ptr(), message.len());
                if code != 0 {
                    return Err(Error::new(code, until_nul(&message), b"")?.into());
                }
                owned(output)
            },
            "failed to allocate semantic-policy template JSON",
            "unexpected exception while building semantic-policy template",
        )
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_inspect_file_xml(
    context: *mut adm_context_t,
    input_path: *const c_char,
    out_xml: *mut *mut c_char,
) -> i32 {
    unsafe {
        text_query(
            context,
            input_path,
            out_xml,
            axml,
            "failed to allocate ADM XML",
            "unexpected exception while reading ADM XML",
        )
    }
}

/// The C++ `std::bad_alloc::what()` the AXML reader appended to its message.
#[cfg(windows)]
const BAD_ALLOC: &[u8] = b"bad allocation";
#[cfg(not(windows))]
const BAD_ALLOC: &[u8] = b"std::bad_alloc";

/// `io::get_axml`: the axml payload, rejecting a CHNA track index 0 and a missing or empty axml.
pub(crate) fn axml(path: &[u8]) -> Result<Vec<u8>, Failure> {
    let input = concat(&[b"input=", path])?;
    let (axml, chna_zero) = match chunks(path) {
        Ok(chunks) => chunks,
        Err(Chunks::Error(mut error)) => {
            error.context = input;
            return Err(error.into());
        }
        Err(Chunks::OutOfMemory) => {
            return Err(Error::new(
                3,
                &concat(&["读取 AXML 失败：".as_bytes(), BAD_ALLOC])?,
                &input,
            )?
            .into());
        }
        Err(Chunks::Path) => {
            return Err(
                Error::new(3, "读取 AXML 失败：路径无法转换为 UTF-8".as_bytes(), &input)?.into(),
            );
        }
    };
    if chna_zero {
        return Err(Error::new(3, "CHNA trackIndex 必须从 1 开始".as_bytes(), &input)?.into());
    }
    if axml.is_empty() {
        return Err(Error::new(3, "axml chunk 缺失或为空".as_bytes(), &input)?.into());
    }
    Ok(axml)
}

enum Chunks {
    Error(Error),
    OutOfMemory,
    /// The native path could not be converted (a C++ exception in the reader).
    Path,
}

struct ChunkReader(*mut ChunkReadHandle);

impl Drop for ChunkReader {
    fn drop(&mut self) {
        unsafe { mradm_wav_chunks_destroy(self.0) };
    }
}

fn alloc_zeroed<T: Clone + Default>(len: usize) -> Result<Vec<T>, Chunks> {
    let mut v = Vec::new();
    v.try_reserve_exact(len).map_err(|_| Chunks::OutOfMemory)?;
    v.resize(len, T::default());
    Ok(v)
}

/// `read_wav_adm_chunks`: the axml payload (empty when absent) and whether a CHNA record has
/// track index 0.
fn chunks(path: &[u8]) -> Result<(Vec<u8>, bool), Chunks> {
    let wav_error = |code: i32, message: &[u8]| {
        concat(&[b"path=", path])
            .and_then(|locator| Error::clamped(code, until_nul(message), &locator))
            .map_or(Chunks::OutOfMemory, Chunks::Error)
    };
    let utf8 = utf8_path(path).ok_or(Chunks::Path)?;
    let mut message = [0u8; 512];
    let mut raw = ptr::null_mut();
    let code = unsafe {
        mradm_wav_chunks_open(
            utf8.as_ptr(),
            utf8.len(),
            &mut raw,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    let reader = ChunkReader(raw);
    if code != 0 {
        return Err(wav_error(code, &message));
    }
    let id = b"axml";
    let mut size = 0u64;
    let mut found = 0u8;
    let code = unsafe {
        mradm_wav_chunks_read(
            reader.0,
            id.as_ptr(),
            ptr::null_mut(),
            0,
            &mut size,
            &mut found,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    if code != 0 {
        return Err(wav_error(code, &message));
    }
    let mut axml = Vec::new();
    if found != 0 {
        let Ok(len) = usize::try_from(size) else {
            return Err(wav_error(2, b"WAVE chunk is too large"));
        };
        axml = alloc_zeroed::<u8>(len)?;
        if len != 0 {
            let code = unsafe {
                mradm_wav_chunks_read(
                    reader.0,
                    id.as_ptr(),
                    axml.as_mut_ptr(),
                    axml.len(),
                    &mut size,
                    &mut found,
                    message.as_mut_ptr(),
                    message.len(),
                )
            };
            if code != 0 {
                return Err(wav_error(code, &message));
            }
        }
    }
    let mut count = 0usize;
    let code = unsafe {
        mradm_wav_chunks_chna(
            reader.0,
            ptr::null_mut(),
            ptr::null_mut(),
            0,
            &mut count,
            &mut found,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    if code != 0 {
        return Err(wav_error(code, &message));
    }
    if found == 0 || count == 0 {
        return Ok((axml, false));
    }
    let mut indices = alloc_zeroed::<u16>(count)?;
    let mut uids = alloc_zeroed::<u8>(count.wrapping_mul(12))?;
    let code = unsafe {
        mradm_wav_chunks_chna(
            reader.0,
            indices.as_mut_ptr(),
            uids.as_mut_ptr(),
            count,
            &mut count,
            &mut found,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    if code != 0 {
        return Err(wav_error(code, &message));
    }
    Ok((axml, indices[..count].contains(&0)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::context::{adm_context_last_error_message, adm_create_context, adm_destroy_context};
    use crate::test_alloc::{with_allocation_failure, with_allocation_failure_after};

    #[test]
    fn query_error_allocations_return_internal_and_allow_retry() {
        let context = adm_create_context();
        for successful in 0..3 {
            let mut out = ptr::null_mut();
            let (code, failed) = with_allocation_failure_after(successful, || unsafe {
                text_query(
                    context,
                    c"input.wav".as_ptr(),
                    &mut out,
                    |path| Err(scene_error(3, b"cannot read", 1, path)?.into()),
                    "allocation failed",
                    "unexpected query failure",
                )
            });
            assert!(failed);
            assert_eq!(code, guard::INTERNAL);
            assert!(out.is_null());
            assert_eq!(
                unsafe { c_str(adm_context_last_error_message(context)) },
                b"unexpected query failure"
            );
        }
        let failure = scene_error(3, b"cannot read", 1, b"input.wav").unwrap();
        assert_eq!(unsafe { report(context, failure.into(), None) }, guard::IO);
        assert_eq!(
            unsafe { c_str(adm_context_last_error_message(context)) },
            b"cannot read: input=input.wav"
        );
        unsafe { adm_destroy_context(context) };
    }

    #[test]
    fn xml_locator_allocation_failure_returns_internal() {
        let context = adm_create_context();
        let mut out = ptr::null_mut();
        let (code, failed) = with_allocation_failure(|| unsafe {
            adm_inspect_file_xml(context, c"unused.wav".as_ptr(), &mut out)
        });
        assert!(failed);
        assert_eq!(code, guard::INTERNAL);
        assert!(out.is_null());
        assert_eq!(
            unsafe { c_str(adm_context_last_error_message(context)) },
            b"unexpected exception while reading ADM XML"
        );
        unsafe { adm_destroy_context(context) };
    }

    #[test]
    fn stage_names_map_to_their_values() {
        let stage = |s: &std::ffi::CStr| unsafe { adm_render_stage_from_string(s.as_ptr()) };
        assert_eq!(stage(c"validating"), 1);
        assert_eq!(stage(c"finished"), 7);
        assert_eq!(stage(c"Rendering"), 0);
        assert_eq!(unsafe { adm_render_stage_from_string(ptr::null()) }, 0);
    }

    #[test]
    fn scene_errors_take_the_locator_of_their_context() {
        let e = scene_error(3, b"m\0junk", 1, b"a.wav").unwrap();
        assert_eq!(
            (e.code, &e.message[..], &e.context[..]),
            (3, &b"m"[..], &b"input=a.wav"[..])
        );
        let e = scene_error(9, b"m", 2, b"a.wav").unwrap();
        assert_eq!(
            (e.code, &e.context[..]),
            (guard::INTERNAL, &b"path=a.wav"[..])
        );
        assert!(scene_error(1, b"", 0, b"a.wav").unwrap().context.is_empty());
    }

    #[test]
    fn duration_is_frames_over_rate() {
        let info = adm_scene_info_t {
            sample_rate: 48_000,
            channels: 2,
            frames: 72_000,
            programme_count: 1,
            object_count: 3,
        };
        assert_eq!(unsafe { adm_scene_info_duration_seconds(&info) }, 1.5);
        assert_eq!(unsafe { adm_scene_info_duration_seconds(ptr::null()) }, 0.0);
        assert_eq!(unsafe { adm_scene_info_channels(ptr::null()) }, 0);
    }
}
