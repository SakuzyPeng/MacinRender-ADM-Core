//! The render orchestration boundary (`mradm-engine`): the backend
//! capability records and the discovery JSON documents. Island facts (the Apple backend, the
//! system-spatial layouts, encoder availability) arrive as [`EnginePlatform`].
use crate::adm::Text;
use mradm_engine::{
    catalog::{self, Backend, Layout, Mode, Platform, SpatialLayout},
    request, tables,
};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
    sync::OnceLock,
};

pub const LAYOUT_3D: u32 = 1;
pub const LAYOUT_SPREAD: u32 = 2;
pub const LAYOUT_BINAURAL: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineLayout {
    id: Text,
    display_name: Text,
    channel_count: u16,
    lfe_count: u16,
    /// `LAYOUT_3D | LAYOUT_SPREAD | LAYOUT_BINAURAL`.
    flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineMode {
    id: Text,
    display_name: Text,
    layouts: *const Text,
    layouts_len: usize,
    sample_rates: *const u32,
    sample_rates_len: usize,
    supports_realtime: u32,
}

/// Feature bits: 1 objects, 2 DirectSpeakers, 4 HOA, 8 channelLock, 16 divergence,
/// 32 screenRef, 64 diffuse, 128 render window.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineBackend {
    renderer: Text,
    name: Text,
    version: Text,
    layouts: *const EngineLayout,
    layouts_len: usize,
    modes: *const EngineMode,
    modes_len: usize,
    qualities: *const EngineMode,
    qualities_len: usize,
    hrtf_sources: *const Text,
    hrtf_sources_len: usize,
    flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EngineSpatialLayout {
    id: Text,
    display_name: Text,
    channel_count: u16,
    reserved: u16,
}

/// `apple_lfe_routing_ok` is -1 when absent; `apple` may be null.
#[repr(C)]
pub struct EnginePlatform {
    apac: u32,
    iamf: u32,
    iamf_mp4_packager: u32,
    apple_lfe_routing_ok: i32,
    apple: *const EngineBackend,
    system_spatial: *const EngineSpatialLayout,
    system_spatial_len: usize,
}

/// An owned document; its text stays valid until `mradm_engine_text_destroy`.
pub struct EngineText(String);

/// Owned record storage behind one backend view.
struct Records {
    backend: EngineBackend,
    _layouts: Vec<EngineLayout>,
    _modes: Vec<EngineMode>,
    _qualities: Vec<EngineMode>,
    _mode_layouts: Vec<Vec<Text>>,
    _hrtf: Vec<Text>,
    _source: Box<Backend>,
}

// The records only point into `_source` and the vectors they live beside, never mutated.
unsafe impl Send for Records {}
unsafe impl Sync for Records {}

fn layout_record(layout: &Layout) -> EngineLayout {
    let mut flags = 0;
    if layout.is_3d {
        flags |= LAYOUT_3D;
    }
    if layout.supports_spread {
        flags |= LAYOUT_SPREAD;
    }
    if layout.is_binaural {
        flags |= LAYOUT_BINAURAL;
    }
    EngineLayout {
        id: Text::new(&layout.id),
        display_name: Text::new(&layout.display_name),
        channel_count: layout.channel_count,
        lfe_count: layout.lfe_count,
        flags,
    }
}

fn mode_records(modes: &[Mode], storage: &mut Vec<Vec<Text>>) -> Vec<EngineMode> {
    modes
        .iter()
        .map(|mode| {
            storage.push(mode.layouts.iter().map(|l| Text::new(l)).collect());
            let layouts = storage.last().unwrap();
            EngineMode {
                id: Text::new(&mode.id),
                display_name: Text::new(&mode.display_name),
                layouts: layouts.as_ptr(),
                layouts_len: layouts.len(),
                sample_rates: mode.sample_rates.as_ptr(),
                sample_rates_len: mode.sample_rates.len(),
                supports_realtime: u32::from(mode.supports_realtime),
            }
        })
        .collect()
}

fn records(backend: Backend) -> Records {
    let source = Box::new(backend);
    let b = &*source;
    let layouts: Vec<EngineLayout> = b.layouts.iter().map(layout_record).collect();
    let mut mode_layouts = Vec::new();
    let modes = mode_records(&b.modes, &mut mode_layouts);
    let qualities = mode_records(&b.qualities, &mut mode_layouts);
    let hrtf: Vec<Text> = b.hrtf_sources.iter().map(|s| Text::new(s)).collect();
    let flags = [
        b.supports_objects,
        b.supports_direct_speakers,
        b.supports_hoa,
        b.supports_channel_lock,
        b.supports_object_divergence,
        b.supports_screen_ref,
        b.supports_diffuse,
        b.supports_render_window,
    ]
    .iter()
    .enumerate()
    .fold(0u32, |acc, (bit, &set)| acc | (u32::from(set) << bit));
    let backend = EngineBackend {
        renderer: Text::new(&b.renderer),
        name: Text::new(&b.name),
        version: Text::new(&b.version),
        layouts: layouts.as_ptr(),
        layouts_len: layouts.len(),
        modes: modes.as_ptr(),
        modes_len: modes.len(),
        qualities: qualities.as_ptr(),
        qualities_len: qualities.len(),
        hrtf_sources: hrtf.as_ptr(),
        hrtf_sources_len: hrtf.len(),
        flags,
    };
    Records {
        backend,
        _layouts: layouts,
        _modes: modes,
        _qualities: qualities,
        _mode_layouts: mode_layouts,
        _hrtf: hrtf,
        _source: source,
    }
}

fn base_platform() -> Platform {
    Platform {
        sofa: cfg!(feature = "sofa"),
        ..Platform::default()
    }
}

/// The capability records of the Rust backends in discovery order (0 ear, 1 saf, 2
/// triple-balance, 3 hoa, 4 saf-binaural, 5 room-binaural); process-lifetime data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_backend(index: u32, out: *mut *const EngineBackend) -> i32 {
    static RECORDS: OnceLock<Vec<Records>> = OnceLock::new();
    if out.is_null() {
        return 1;
    }
    let result = catch_unwind(|| {
        let all = RECORDS.get_or_init(|| {
            catalog::backends(&base_platform())
                .into_iter()
                .map(records)
                .collect()
        });
        all.get(index as usize)
            .map(|r| &r.backend as *const EngineBackend)
    });
    match result {
        Ok(Some(view)) => {
            unsafe { *out = view };
            0
        }
        Ok(None) => 1,
        Err(_) => 6,
    }
}

unsafe fn items<'a, T>(data: *const T, len: usize) -> Option<&'a [T]> {
    if len == 0 {
        return Some(&[]);
    }
    if data.is_null() || !(data as usize).is_multiple_of(align_of::<T>()) {
        return None;
    }
    Some(unsafe { slice::from_raw_parts(data, len) })
}

pub(crate) unsafe fn text(value: Text) -> Option<String> {
    let bytes = unsafe { items(value.data, value.len)? };
    Some(String::from_utf8_lossy(bytes).into_owned())
}

pub(crate) unsafe fn texts(data: *const Text, len: usize) -> Option<Vec<String>> {
    unsafe { items(data, len)? }
        .iter()
        .map(|&t| unsafe { text(t) })
        .collect()
}

unsafe fn modes(data: *const EngineMode, len: usize) -> Option<Vec<Mode>> {
    unsafe { items(data, len)? }
        .iter()
        .map(|m| {
            Some(Mode {
                id: unsafe { text(m.id)? },
                display_name: unsafe { text(m.display_name)? },
                layouts: unsafe { texts(m.layouts, m.layouts_len)? },
                sample_rates: unsafe { items(m.sample_rates, m.sample_rates_len)? }.to_vec(),
                supports_realtime: m.supports_realtime != 0,
            })
        })
        .collect()
}

unsafe fn backend(b: &EngineBackend) -> Option<Backend> {
    let flag = |bit: u32| b.flags & (1 << bit) != 0;
    Some(Backend {
        renderer: unsafe { text(b.renderer)? },
        name: unsafe { text(b.name)? },
        version: unsafe { text(b.version)? },
        layouts: unsafe { items(b.layouts, b.layouts_len)? }
            .iter()
            .map(|l| {
                Some(Layout {
                    id: unsafe { text(l.id)? },
                    display_name: unsafe { text(l.display_name)? },
                    channel_count: l.channel_count,
                    is_3d: l.flags & LAYOUT_3D != 0,
                    lfe_count: l.lfe_count,
                    supports_spread: l.flags & LAYOUT_SPREAD != 0,
                    is_binaural: l.flags & LAYOUT_BINAURAL != 0,
                })
            })
            .collect::<Option<_>>()?,
        modes: unsafe { modes(b.modes, b.modes_len)? },
        qualities: unsafe { modes(b.qualities, b.qualities_len)? },
        supports_objects: flag(0),
        supports_direct_speakers: flag(1),
        supports_hoa: flag(2),
        supports_channel_lock: flag(3),
        supports_object_divergence: flag(4),
        supports_screen_ref: flag(5),
        supports_diffuse: flag(6),
        hrtf_sources: unsafe { texts(b.hrtf_sources, b.hrtf_sources_len)? },
        supports_render_window: flag(7),
    })
}

pub(crate) unsafe fn platform(p: &EnginePlatform) -> Option<Platform> {
    let apple = if p.apple.is_null() {
        None
    } else {
        Some(unsafe { backend(&*p.apple)? })
    };
    let system_spatial_layouts = unsafe { items(p.system_spatial, p.system_spatial_len)? }
        .iter()
        .map(|l| {
            Some(SpatialLayout {
                id: unsafe { text(l.id)? },
                display_name: unsafe { text(l.display_name)? },
                channel_count: l.channel_count,
            })
        })
        .collect::<Option<_>>()?;
    Some(Platform {
        apac: p.apac != 0,
        iamf: p.iamf != 0,
        iamf_mp4_packager: p.iamf_mp4_packager != 0,
        apple,
        system_spatial_layouts,
        apple_system_spatial_lfe_routing_ok: (p.apple_lfe_routing_ok >= 0)
            .then_some(p.apple_lfe_routing_ok != 0),
        ..base_platform()
    })
}

/// Discovery document `kind`: 0 capabilities, 1 layouts, 2 output formats, 3 input layouts,
/// 4 render support matrix (nlohmann `dump(2)` bytes, no trailing newline). Returns 1 for an
/// invalid argument, 6 for a panic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_document(
    kind: u32,
    platform_record: *const EnginePlatform,
    out: *mut *mut EngineText,
    document: *mut Text,
) -> i32 {
    if platform_record.is_null() || out.is_null() || document.is_null() {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let platform = unsafe { platform(&*platform_record)? };
        Some(match kind {
            0 => tables::capabilities_json(&platform),
            1 => tables::layouts_json(&platform),
            2 => tables::output_formats_json(&platform),
            3 => tables::input_layouts_json(),
            4 => tables::support_matrix_json(&platform),
            _ => return None,
        })
    }));
    match result {
        Ok(Some(json)) => {
            let owned = Box::new(EngineText(json));
            unsafe {
                *document = Text::new(&owned.0);
                *out = Box::into_raw(owned);
            }
            0
        }
        Ok(None) => 1,
        Err(_) => {
            unsafe { *out = ptr::null_mut() };
            6
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_text_destroy(text: *mut EngineText) {
    if !text.is_null() {
        drop(unsafe { Box::from_raw(text) });
    }
}

// ── Request resolution ─────────────────────────────────────────────

#[repr(C)]
pub struct EngineLog {
    pub(crate) level: u32,
    pub(crate) module: Text,
    pub(crate) message: Text,
}

/// `code` 0 is success; the views stay valid until `mradm_engine_result_destroy`.
#[repr(C)]
pub struct EngineStatus {
    pub(crate) code: i32,
    pub(crate) message: Text,
    pub(crate) context: Text,
    pub(crate) logs: *const EngineLog,
    pub(crate) logs_len: usize,
}

/// The status and log lines of one engine call.
pub struct EngineResult {
    error: Option<request::Error>,
    logs: Vec<request::Log>,
    log_views: Vec<EngineLog>,
}

pub(crate) fn finish<T>(
    outcome: request::Outcome<T>,
    out: *mut *mut EngineResult,
    status: *mut EngineStatus,
) -> Option<T> {
    let (value, error) = match outcome.result {
        Ok(value) => (Some(value), None),
        Err(error) => (None, Some(error)),
    };
    let mut result = Box::new(EngineResult {
        error,
        logs: outcome.logs,
        log_views: Vec::new(),
    });
    result.log_views = result
        .logs
        .iter()
        .map(|l| EngineLog {
            level: l.level as u32,
            module: Text::new(l.module),
            message: Text::new(&l.message),
        })
        .collect();
    unsafe {
        *status = status_view(&result);
        *out = Box::into_raw(result);
    }
    value
}

fn status_view(result: &EngineResult) -> EngineStatus {
    let (code, message, context) = result
        .error
        .as_ref()
        .map_or((0, Text::new(""), Text::new("")), |e| {
            (e.code, Text::new(&e.message), Text::new(&e.context))
        });
    EngineStatus {
        code,
        message,
        context,
        logs: result.log_views.as_ptr(),
        logs_len: result.log_views.len(),
    }
}

pub(crate) fn guarded(
    out: *mut *mut EngineResult,
    status: *mut EngineStatus,
    call: impl FnOnce() -> i32,
) -> i32 {
    if out.is_null() || status.is_null() {
        return 1;
    }
    unsafe { *out = ptr::null_mut() };
    catch_unwind(AssertUnwindSafe(call)).unwrap_or(6)
}

unsafe fn input<'a>(data: *const u8, len: usize) -> Option<&'a [u8]> {
    unsafe { items(data, len) }
}

/// The canonical id of an output layout, or the input itself when it is not an alias.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_normalize_layout(
    layout: *const u8,
    len: usize,
    out: *mut Text,
) -> i32 {
    if out.is_null() {
        return 1;
    }
    let Some(bytes) = (unsafe { input(layout, len) }) else {
        return 1;
    };
    unsafe {
        *out = match request::canonical_layout(bytes) {
            Some(id) => Text::new(id),
            None => Text::bytes(bytes),
        };
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_engine_result_destroy(result: *mut EngineResult) {
    if !result.is_null() {
        drop(unsafe { Box::from_raw(result) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Records built for C++ read back as the same backend, as the Apple island's report does.
    #[test]
    fn backend_records_round_trip() {
        let mut all = catalog::backends(&base_platform());
        let mut apple = catalog::room();
        apple.renderer = "apple".into();
        apple.supports_hoa = true;
        apple.supports_render_window = false;
        all.push(apple);
        for expected in all {
            let r = records(expected.clone());
            assert_eq!(unsafe { backend(&r.backend) }.unwrap(), expected);
        }
    }

    fn text_of(t: Text) -> String {
        String::from_utf8(unsafe { slice::from_raw_parts(t.data, t.len) }.to_vec()).unwrap()
    }

    #[test]
    fn layout_normalization_views() {
        let mut out = Text::new("");
        let raw = b"Bogus";
        assert_eq!(
            unsafe { mradm_engine_normalize_layout(raw.as_ptr(), raw.len(), &mut out) },
            0
        );
        assert_eq!(
            out.data,
            raw.as_ptr(),
            "an unknown layout borrows the input"
        );
        let atmos = b"ATMOS714";
        assert_eq!(
            unsafe { mradm_engine_normalize_layout(atmos.as_ptr(), atmos.len(), &mut out) },
            0
        );
        assert_eq!(text_of(out), "4+7+0");
    }

    #[test]
    fn backend_views_and_documents() {
        let mut view = ptr::null();
        for index in 0..6 {
            assert_eq!(unsafe { mradm_engine_backend(index, &mut view) }, 0);
        }
        assert_eq!(unsafe { mradm_engine_backend(6, &mut view) }, 1);
        let platform = EnginePlatform {
            apac: 0,
            iamf: 0,
            iamf_mp4_packager: 0,
            apple_lfe_routing_ok: -1,
            apple: ptr::null(),
            system_spatial: ptr::null(),
            system_spatial_len: 0,
        };
        for kind in 0..5 {
            let mut out = ptr::null_mut();
            let mut text = Text::new("");
            assert_eq!(
                unsafe { mradm_engine_document(kind, &platform, &mut out, &mut text) },
                0
            );
            assert!(text.len > 0);
            unsafe { mradm_engine_text_destroy(out) };
        }
        let mut out = ptr::null_mut();
        let mut text = Text::new("");
        assert_eq!(
            unsafe { mradm_engine_document(5, &platform, &mut out, &mut text) },
            1
        );
    }
}
