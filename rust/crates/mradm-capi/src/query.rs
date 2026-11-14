//! Queries of an input file: `adm_probe_file` and its `adm_scene_info_t`, `adm_inspect_file_json`,
//! `adm_inspect_file_xml` and `adm_policy_template_json`, plus `adm_render_stage_from_string`.
//!
//! Each export calls `mradm-scene`, `mradm-wav` and `mradm-engine` directly (R2); messages are the
//! Rust errors' full text, no longer cut to the former FFI buffers.
use crate::context::adm_context_t;
use crate::error::{Error, Failure, c_str, caught, clear, concat, report, set, try_box, utf8_path};
use crate::guard;
use crate::string;
use mradm_ffi::scene::SceneHandle;
use mradm_scene::{Context, RenderScene};
use mradm_wav::{Chna, ChunkReader, Reader};
use std::ffi::c_char;
use std::fs::File;
use std::io::BufReader;
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

/// An imported scene. The pointer is the boxed `SceneHandle` the render and Monitor requests still
/// take; it is released with the scene.
pub(crate) struct Scene(pub(crate) *mut SceneHandle);

impl Scene {
    pub(crate) fn render_scene(&self) -> &RenderScene {
        // The pointer comes from `Box::into_raw` in `import_with` and lives until drop.
        unsafe { (*self.0).scene() }
    }
}

impl Drop for Scene {
    fn drop(&mut self) {
        if !self.0.is_null() {
            drop(unsafe { Box::from_raw(self.0) });
        }
    }
}

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
    let layout = layout.map(|l| String::from_utf8_lossy(l).into_owned());
    let labels: Vec<String> = labels
        .iter()
        .map(|l| String::from_utf8_lossy(l).into_owned())
        .collect();
    let handle = SceneHandle::import(&utf8, routed, layout.as_deref(), &labels)
        .map_err(|error| scene_failure(&error, path))?;
    Ok(Scene(Box::into_raw(handle)))
}

/// A scene error with its locator (the input or a referenced path).
pub(crate) fn scene_failure(error: &mradm_scene::Error, path: &[u8]) -> Failure {
    let locator = match error.context {
        Context::Input => concat(&[b"input=", path]),
        Context::Path => concat(&[b"path=", path]),
        Context::None | Context::Label => Ok(Vec::new()),
    };
    match locator.and_then(|l| Error::clamped(error.code, error.message.as_bytes(), &l)) {
        Ok(error) => error.into(),
        Err(failure) => failure,
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
    let render = scene.render_scene();
    Ok(adm_scene_info_t {
        sample_rate: render.info.sample_rate,
        channels: u32::from(render.info.channels),
        frames: render.info.frames,
        programme_count: render.snapshot.programmes.len() as u32,
        object_count: render.snapshot.objects.len() as u32,
    })
}

/// The import is complete once `import_with` returns: there is no separate view step that can fail.
pub(crate) fn check_view(_scene: &Scene, _path: &[u8]) -> Result<(), Failure> {
    Ok(())
}

/// `log_import_warnings`: the scene's import warnings.
pub(crate) fn import_warnings(scene: &Scene, _path: &[u8]) -> Result<Vec<Vec<u8>>, Failure> {
    let snapshot = &scene.render_scene().snapshot;
    let mut warnings = Vec::new();
    warnings
        .try_reserve_exact(snapshot.warnings.len())
        .map_err(|_| Failure::Exception)?;
    for &id in &snapshot.warnings {
        let text = snapshot
            .strings
            .get(id as usize)
            .ok_or(Failure::Exception)?;
        warnings.push(concat(&[text.as_bytes()])?);
    }
    Ok(warnings)
}

/// A UTF-8 path as `mradm-wav` opens it.
fn wav_path(utf8: &[u8]) -> Option<&str> {
    std::str::from_utf8(utf8)
        .ok()
        .filter(|p| !p.is_empty() && !p.contains('\0'))
}

fn wave_facts(path: &[u8]) -> Option<adm_scene_info_t> {
    let utf8 = utf8_path(path)?;
    let file = File::open(wav_path(&utf8)?).ok()?;
    let info = Reader::new(BufReader::new(file)).ok()?.info();
    let channels = u32::from(info.format.channels);
    (channels != 0).then_some(adm_scene_info_t {
        sample_rate: info.format.sample_rate,
        channels,
        frames: info.frames,
        programme_count: 0,
        object_count: 0,
    })
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

/// An owned copy of a text output.
pub(crate) fn owned(bytes: &[u8]) -> Result<Vec<u8>, Failure> {
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
                let display = String::from_utf8_lossy(&utf8);
                owned(
                    mradm_engine::scene_json::inspect_json(scene.render_scene(), &display)
                        .as_bytes(),
                )
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
                owned(mradm_scene::report::template(scene.render_scene()).as_bytes())
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
pub(crate) const BAD_ALLOC: &[u8] = b"bad allocation";
#[cfg(not(windows))]
pub(crate) const BAD_ALLOC: &[u8] = b"std::bad_alloc";

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

fn alloc_zeroed<T: Clone + Default>(len: usize) -> Result<Vec<T>, Chunks> {
    let mut v = Vec::new();
    v.try_reserve_exact(len).map_err(|_| Chunks::OutOfMemory)?;
    v.resize(len, T::default());
    Ok(v)
}

/// `read_wav_adm_chunks`: the axml payload (empty when absent) and whether a CHNA record has
/// track index 0.
fn chunks(path: &[u8]) -> Result<(Vec<u8>, bool), Chunks> {
    let wav_error = |error: mradm_wav::Error| {
        concat(&[b"path=", path])
            .and_then(|locator| Error::clamped(error.code, error.message.as_bytes(), &locator))
            .map_or(Chunks::OutOfMemory, Chunks::Error)
    };
    let utf8 = utf8_path(path).ok_or(Chunks::Path)?;
    let name = std::str::from_utf8(&utf8)
        .map_err(|_| wav_error(mradm_wav::Error::invalid("WAVE 路径必须使用 UTF-8")))?;
    if name.is_empty() {
        return Err(wav_error(mradm_wav::Error::invalid("WAVE 路径为空")));
    }
    if name.contains('\0') {
        return Err(wav_error(mradm_wav::Error::invalid("WAVE 路径包含 NUL")));
    }
    let file = File::open(name).map_err(|e| wav_error(e.into()))?;
    let mut reader = ChunkReader::new(BufReader::new(file)).map_err(wav_error)?;
    let id = *b"axml";
    let mut axml = Vec::new();
    if let Some(chunk) = reader.chunks().iter().find(|c| c.id == id).copied() {
        let Ok(len) = usize::try_from(chunk.size) else {
            return Err(wav_error(mradm_wav::Error {
                code: 2,
                message: "WAVE chunk is too large".into(),
            }));
        };
        axml = alloc_zeroed::<u8>(len)?;
        if len != 0 {
            reader.read_chunk(id, &mut axml).map_err(wav_error)?;
        }
    }
    let Some(data) = reader.metadata(*b"chna", u64::MAX).map_err(wav_error)? else {
        return Ok((axml, false));
    };
    let chna = Chna::decode_import(&data).map_err(wav_error)?;
    Ok((axml, chna.entries.iter().any(|e| e.track_index == 0)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::context::{adm_context_last_error_message, adm_create_context, adm_destroy_context};
    use crate::test_alloc::{with_allocation_failure, with_allocation_failure_after};

    fn scene(code: i32, message: &str, context: Context) -> mradm_scene::Error {
        mradm_scene::Error::new(code, message, context)
    }

    #[test]
    fn query_error_allocations_return_internal_and_allow_retry() {
        let context = adm_create_context();
        let cannot_read = scene(3, "cannot read", Context::Input);
        for successful in 0..3 {
            let mut out = ptr::null_mut();
            let (code, failed) = with_allocation_failure_after(successful, || unsafe {
                text_query(
                    context,
                    c"input.wav".as_ptr(),
                    &mut out,
                    |path| Err(scene_failure(&cannot_read, path)),
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
        let failure = scene_failure(&cannot_read, b"input.wav");
        assert_eq!(unsafe { report(context, failure, None) }, guard::IO);
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
        let error = |e: Failure| match e {
            Failure::Error(e) => e,
            Failure::Exception => panic!("allocation"),
        };
        let e = error(scene_failure(&scene(3, "m", Context::Input), b"a.wav"));
        assert_eq!(
            (e.code, &e.message[..], &e.context[..]),
            (3, &b"m"[..], &b"input=a.wav"[..])
        );
        let e = error(scene_failure(&scene(9, "m", Context::Path), b"a.wav"));
        assert_eq!(
            (e.code, &e.context[..]),
            (guard::INTERNAL, &b"path=a.wav"[..])
        );
        assert!(
            error(scene_failure(&scene(1, "", Context::None), b"a.wav"))
                .context
                .is_empty()
        );
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
