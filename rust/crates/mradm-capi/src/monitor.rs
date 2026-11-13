//! Realtime monitors (`adm_monitor_*`) and `adm_hptf_parse_parametric_eq`. Each export validates
//! the caller's records as the C++ did and calls the same `mradm-ffi` monitor session, HpTF and
//! room entries the C++ `MonitorSession` shell called, with the same message buffers.
use crate::context::adm_context_t;
use crate::error::{self, Error, Failure, c_str, caught, clear, concat, report, set, utf8_path};
use crate::guard::{self, INTERNAL, INVALID_ARGUMENT, OK};
use crate::options::adm_render_options_t;
use crate::query::{Scene, import_warnings, import_with};
use crate::render::{apply_policy_logged, islands};
use crate::room::adm_room_config_t;
use crate::scene::{
    Control, Message, adm_hptf_band_t, adm_hptf_config_t, adm_hptf_info_t, adm_hptf_parameters_t,
    adm_hptf_stereo_config_t, adm_hptf_stereo_info_t, adm_hptf_stereo_parameters_t, hptf_infos,
    lock, mradm_device_islands, owned, room_source_json, room_status_json, switch_room, text,
    write_sized,
};
use mradm_ffi::adm::Text;
use mradm_ffi::hptf::settings::{
    Settings, mradm_dsp_hptf_settings_create, mradm_dsp_hptf_settings_destroy,
    mradm_dsp_hptf_validate,
};
use mradm_ffi::hptf::{Band, ParseResult, Snapshot, mradm_dsp_hptf_parse};
use mradm_ffi::render::RenderLiveOverride;
use mradm_ffi::render::RenderOverride;
use mradm_ffi::render::device_island::DeviceEnumeration;
use mradm_ffi::render::engine_monitor::{EngineLiveMonitorLevels, EngineLiveMonitorStatus};
use mradm_ffi::render::engine_monitor_session::{
    EngineMonitorSession, EngineMonitorSessionHost, mradm_engine_monitor_session_destroy,
    mradm_engine_monitor_session_hptf, mradm_engine_monitor_session_hptf_snapshot,
    mradm_engine_monitor_session_levels, mradm_engine_monitor_session_log,
    mradm_engine_monitor_session_log_count, mradm_engine_monitor_session_log_entry,
    mradm_engine_monitor_session_new, mradm_engine_monitor_session_orientation,
    mradm_engine_monitor_session_output_device, mradm_engine_monitor_session_overrides,
    mradm_engine_monitor_session_pause, mradm_engine_monitor_session_play,
    mradm_engine_monitor_session_room_control, mradm_engine_monitor_session_seek,
    mradm_engine_monitor_session_set_loop, mradm_engine_monitor_session_start,
    mradm_engine_monitor_session_status, mradm_engine_monitor_session_switch,
};
use mradm_ffi::scene::{SceneImportInfo, mradm_scene_view};
use std::ffi::{c_char, c_void};
use std::io::Write;
use std::mem::{MaybeUninit, offset_of};
use std::ptr;
use std::sync::Mutex;

// ── Records of `c_api.h` ─────────────────────────────────────────────────────────

/// `adm_monitor_override_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct adm_monitor_override_t {
    struct_size: u32,
    object_id: *const c_char,
    gain_db: f32,
    diffuse_scale: f32,
    extent_scale: f32,
    divergence_scale: f32,
    extent_width_scale: f32,
    extent_height_scale: f32,
    extent_depth_scale: f32,
    speaker_label: *const c_char,
    head_locked: i32,
    reserved_v1_29: u32,
    mute: i32,
    reserved_v1_33: u32,
    head_locked_valid: i32,
}

/// `adm_monitor_status_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_monitor_status_t {
    struct_size: u32,
    state: i32,
    playhead_frames: u64,
    underruns: u64,
    buffered_frames: u64,
    ring_fill: f32,
    ended: i32,
    failed: i32,
    override_revision: u64,
}

/// `adm_monitor_levels_t`.
#[allow(non_camel_case_types)]
#[repr(C)]
pub struct adm_monitor_levels_t {
    struct_size: u32,
    capacity: u32,
    out_count: u32,
    peak: *mut f32,
    rms: *mut f32,
    momentary_lufs: f32,
    shortterm_lufs: f32,
    integrated_lufs: f32,
}

// ── Session errors ───────────────────────────────────────────────────────────────

/// The session's code for an island or device that threw: the C++ rethrew it.
const THROWN: i32 = 100;
/// The message and context capacity of one session call (large enough for any path in a message).
const TEXT_CAPACITY: usize = 65536;

/// The message and context buffers of one session call (the shell's `Texts`).
struct Texts {
    message: Vec<u8>,
    context: Vec<u8>,
    message_len: usize,
    context_len: usize,
}

fn zeroed(len: usize) -> Result<Vec<u8>, Failure> {
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(len)
        .map_err(|_| Failure::Exception)?;
    bytes.resize(len, 0);
    Ok(bytes)
}

impl Texts {
    fn new(context_capacity: usize) -> Result<Self, Failure> {
        let message = zeroed(TEXT_CAPACITY)?;
        let context = zeroed(context_capacity.max(TEXT_CAPACITY))?;
        Ok(Self {
            message_len: message.len(),
            context_len: context.len(),
            message,
            context,
        })
    }

    /// Runs `call` with the buffers; a non-zero code is the error it wrote, and a thrown island
    /// is an exception.
    fn run(
        mut self,
        call: impl FnOnce(*mut u8, *mut usize, *mut u8, *mut usize) -> i32,
    ) -> Result<(), Failure> {
        let code = call(
            self.message.as_mut_ptr(),
            &mut self.message_len,
            self.context.as_mut_ptr(),
            &mut self.context_len,
        );
        if code == 0 {
            return Ok(());
        }
        if code == THROWN {
            return Err(Failure::Exception);
        }
        let message = &self.message[..self.message_len.min(self.message.len())];
        let context = &self.context[..self.context_len.min(self.context.len())];
        Err(Error::new(code, message, context)?.into())
    }
}

// ── Monitors ─────────────────────────────────────────────────────────────────────

#[allow(non_camel_case_types)] // The `c_api.h` name.
pub struct adm_monitor_t {
    session: *mut EngineMonitorSession,
    /// The imported scene the session borrows; dropped after the session.
    _scene: Scene,
    /// The UTF-8 input path of the session's requests.
    input: Vec<u8>,
    last_error: Mutex<Message>,
    /// Back `adm_monitor_log_entry`'s module and message until the next call.
    log: Mutex<(Message, Message)>,
}

// SAFETY: The session is thread-safe (the C++ shell was used from any thread).
unsafe impl Send for adm_monitor_t {}
unsafe impl Sync for adm_monitor_t {}

impl Drop for adm_monitor_t {
    fn drop(&mut self) {
        unsafe { mradm_engine_monitor_session_destroy(self.session) };
    }
}

impl adm_monitor_t {
    fn clear(&self) {
        lock(&self.last_error).clear();
    }

    fn store_message(&self, message: &[u8]) -> bool {
        lock(&self.last_error).set(message)
    }

    /// `store_last_error`: `message: locator` (just the message without a locator).
    fn store(&self, e: &Error) -> bool {
        let separator = if e.context.is_empty() {
            &b""[..]
        } else {
            b": "
        };
        concat(&[&e.message, separator, &e.context]).is_ok_and(|text| self.store_message(&text))
    }

    /// Clears the last error and runs `body` as the C++ `try` did; an exception stores
    /// `unexpected` (nothing for `None`).
    fn call(&self, unexpected: Option<&str>, body: impl FnOnce() -> Result<i32, Failure>) -> i32 {
        let result = guard::guard(Err(Failure::Exception), || {
            self.clear();
            body()
        });
        match result {
            Ok(code) => code,
            Err(Failure::Error(e)) if self.store(&e) => error::map(e.code),
            Err(_) => {
                if let Some(message) = unexpected {
                    self.store_message(message.as_bytes());
                }
                INTERNAL
            }
        }
    }
}

unsafe fn monitor_ref<'a>(monitor: *const adm_monitor_t) -> Option<&'a adm_monitor_t> {
    unsafe { monitor.as_ref() }
}

/// The caller's options, or the defaults (`render_options` of NULL).
#[allow(clippy::large_enum_variant)] // One per call, on the stack as the C++ `RenderOptions`.
enum Options<'a> {
    Borrowed(&'a adm_render_options_t),
    Owned(adm_render_options_t),
}

impl Options<'_> {
    unsafe fn of(opts: *const adm_render_options_t) -> Result<Self, Failure> {
        match unsafe { opts.as_ref() } {
            Some(o) => Ok(Options::Borrowed(o)),
            None => adm_render_options_t::defaults()
                .map(Options::Owned)
                .ok_or(Failure::Exception),
        }
    }

    fn get(&self) -> &adm_render_options_t {
        match self {
            Options::Borrowed(o) => o,
            Options::Owned(o) => o,
        }
    }
}

/// The session's island tables: the output devices and, in macOS builds, the Apple backend.
fn host() -> EngineMonitorSessionHost {
    EngineMonitorSessionHost {
        devices: unsafe { mradm_device_islands() },
        apple: islands::apple(),
    }
}

const CREATE_UNEXPECTED: &str = "unexpected exception while creating realtime monitor";

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_monitor_ex(
    context: *mut adm_context_t,
    input_path: *const c_char,
    opts: *const adm_render_options_t,
    device_id: *const c_char,
    out: *mut *mut adm_monitor_t,
) -> i32 {
    if let Some(out) = unsafe { out.as_mut() } {
        *out = ptr::null_mut();
    }
    if context.is_null() || input_path.is_null() || unsafe { *input_path } == 0 || out.is_null() {
        return INVALID_ARGUMENT;
    }
    caught(context, Some(CREATE_UNEXPECTED), || unsafe {
        clear(context);
        let result = (|| -> Result<adm_monitor_t, Failure> {
            let options = Options::of(opts)?;
            let device = text(device_id);
            create(c_str(input_path), options.get(), device)
        })();
        match result {
            Ok(monitor) => {
                let handle = error::try_box(monitor);
                if handle.is_null() {
                    set(context, b"failed to allocate realtime monitor handle");
                    return INTERNAL;
                }
                *out = handle;
                OK
            }
            Err(failure) => report(context, failure, Some(CREATE_UNEXPECTED)),
        }
    })
}

/// `MonitorSession::create`: the preview import of the input (`prepare_preview_scene`), the session
/// over it with the import's log lines, and its start.
unsafe fn create(
    native: &[u8],
    options: &adm_render_options_t,
    device: &[u8],
) -> Result<adm_monitor_t, Failure> {
    let scene = import_with(
        native,
        true,
        options.input_layout(),
        options.input_channel_labels(),
    )?;
    let input = utf8_path(native).ok_or(Failure::Exception)?;
    // The import's log lines, replayed into the session once it exists.
    let mut lines: Vec<(u32, Vec<u8>, Vec<u8>)> = Vec::new();
    for warning in import_warnings(&scene, native)? {
        lines.try_reserve(1).map_err(|_| Failure::Exception)?;
        lines.push((2, concat(&[b"importer"])?, warning));
    }
    let mut failed = false;
    let applied = apply_policy_logged(&scene, &input, options, |level, module, message| {
        if failed {
            return;
        }
        match (concat(&[module]), concat(&[message]), lines.try_reserve(1)) {
            (Ok(module), Ok(message), Ok(())) => lines.push((level, module, message)),
            _ => failed = true,
        }
    });
    if failed {
        return Err(Failure::Exception);
    }
    applied?;
    let mut info = unsafe { MaybeUninit::<SceneImportInfo>::zeroed().assume_init() };
    let mut view = unsafe { MaybeUninit::<mradm_ffi::adm::View>::zeroed().assume_init() };
    let mut message = [0u8; 256];
    if unsafe {
        mradm_scene_view(
            scene.0,
            &mut info,
            &mut view,
            message.as_mut_ptr(),
            message.len(),
        )
    } != 0
    {
        return Err(Failure::error(6, b"invalid monitor scene", b""));
    }
    let host = host();
    let session = unsafe {
        mradm_engine_monitor_session_new(
            islands::mradm_platform_islands(),
            scene.0,
            info.sample_rate,
            info.frames,
            &host,
        )
    };
    if session.is_null() {
        return Err(Failure::error(6, b"invalid monitor session", b""));
    }
    let monitor = adm_monitor_t {
        session,
        _scene: scene,
        input,
        last_error: Mutex::new(Message::default()),
        log: Mutex::new((Message::default(), Message::default())),
    };
    for (level, module, message) in &lines {
        unsafe {
            mradm_engine_monitor_session_log(
                session,
                *level,
                module.as_ptr(),
                module.len(),
                message.as_ptr(),
                message.len(),
            )
        };
    }
    let request = options.request(&monitor.input, None)?;
    Texts::new(0)?.run(|m, ml, c, cl| unsafe {
        mradm_engine_monitor_session_start(
            session,
            &request.record,
            u32::from(options.monitor_system_spatial),
            device.as_ptr(),
            device.len(),
            m,
            ml,
            c,
            cl,
        )
    })?;
    Ok(monitor)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_create_monitor(
    context: *mut adm_context_t,
    input_path: *const c_char,
    opts: *const adm_render_options_t,
    out: *mut *mut adm_monitor_t,
) -> i32 {
    unsafe { adm_create_monitor_ex(context, input_path, opts, ptr::null(), out) }
}

/// The JSON string escape of the device list: `"`, `\`, and `\n` / `\r` / `\t` only (device
/// names are arbitrary).
fn push_escaped(json: &mut Vec<u8>, value: &[u8]) {
    for &b in value {
        match b {
            b'"' => json.extend_from_slice(b"\\\""),
            b'\\' => json.extend_from_slice(b"\\\\"),
            b'\n' => json.extend_from_slice(b"\\n"),
            b'\r' => json.extend_from_slice(b"\\r"),
            b'\t' => json.extend_from_slice(b"\\t"),
            _ => json.push(b),
        }
    }
}

/// The device list being built; `failed` once a growth failed.
struct DeviceList {
    json: Vec<u8>,
    first: bool,
    failed: bool,
}

unsafe extern "C" fn each_device(user: *mut c_void, id: Text, name: Text, is_default: u32) {
    let list = unsafe { &mut *user.cast::<DeviceList>() };
    let bytes = |t: &Text| {
        if t.len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(t.data, t.len) }
        }
    };
    let (id, name) = (bytes(&id), bytes(&name));
    // Every escape doubles a byte at most.
    let room = 2 * (id.len() + name.len()) + 40;
    if list.failed || list.json.try_reserve(room).is_err() {
        list.failed = true;
        return;
    }
    if !list.first {
        list.json.push(b',');
    }
    list.first = false;
    list.json.extend_from_slice(b"{\"id\":\"");
    push_escaped(&mut list.json, id);
    list.json.extend_from_slice(b"\",\"name\":\"");
    push_escaped(&mut list.json, name);
    list.json.extend_from_slice(b"\",\"default\":");
    list.json
        .extend_from_slice(if is_default != 0 { b"true" } else { b"false" });
    list.json.push(b'}');
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_output_devices_json(
    context: *mut adm_context_t,
    out_json: *mut *mut c_char,
) -> i32 {
    if let Some(out) = unsafe { out_json.as_mut() } {
        *out = ptr::null_mut();
    }
    if context.is_null() || out_json.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| {
        let mut list = DeviceList {
            json: Vec::new(),
            first: true,
            failed: false,
        };
        if list.json.try_reserve(2).is_err() {
            return INTERNAL;
        }
        list.json.push(b'[');
        let each = DeviceEnumeration {
            user: ptr::from_mut(&mut list).cast(),
            each: Some(each_device),
        };
        if let Some(table) = unsafe { mradm_device_islands().as_ref() }
            && let Some(enumerate) = table.enumerate
        {
            unsafe { enumerate(&each) };
        }
        if list.failed || list.json.try_reserve(1).is_err() {
            return INTERNAL;
        }
        list.json.push(b']');
        match unsafe { owned(&list.json, out_json) } {
            Ok(()) => OK,
            Err(_) => INTERNAL,
        }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_destroy_monitor(monitor: *mut adm_monitor_t) {
    if !monitor.is_null() {
        guard::unit(|| drop(unsafe { Box::from_raw(monitor) }));
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_last_error_message(
    monitor: *const adm_monitor_t,
) -> *const c_char {
    match unsafe { monitor_ref(monitor) } {
        Some(m) => lock(&m.last_error).as_ptr(),
        None => c"".as_ptr(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_play(monitor: *mut adm_monitor_t) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    m.call(
        Some("unexpected exception while starting realtime monitor playback"),
        || {
            unsafe { mradm_engine_monitor_session_play(m.session) };
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_pause(monitor: *mut adm_monitor_t) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    m.call(
        Some("unexpected exception while pausing realtime monitor playback"),
        || {
            unsafe { mradm_engine_monitor_session_pause(m.session) };
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_seek(monitor: *mut adm_monitor_t, seconds: f64) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    if !seconds.is_finite() {
        return INVALID_ARGUMENT;
    }
    m.call(
        Some("unexpected exception while seeking realtime monitor"),
        || {
            Texts::new(0)?.run(|ms, ml, c, cl| unsafe {
                mradm_engine_monitor_session_seek(m.session, seconds, ms, ml, c, cl)
            })?;
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_set_loop(
    monitor: *mut adm_monitor_t,
    start_seconds: f64,
    end_seconds: f64,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    if !start_seconds.is_finite() || !end_seconds.is_finite() {
        return INVALID_ARGUMENT;
    }
    m.call(
        Some("unexpected exception while setting realtime monitor loop"),
        || {
            unsafe { mradm_engine_monitor_session_set_loop(m.session, start_seconds, end_seconds) };
            Ok(OK)
        },
    )
}

/// One override entry as the C++ read it, with the fields a shorter caller struct lacks at their
/// defaults; `None` for a non-finite gain or scale.
unsafe fn override_entry(base: *const u8, stride: usize) -> Option<adm_monitor_override_t> {
    let mut src = unsafe { MaybeUninit::<adm_monitor_override_t>::zeroed().assume_init() };
    let len = stride.min(size_of::<adm_monitor_override_t>());
    unsafe { ptr::copy_nonoverlapping(base, ptr::from_mut(&mut src).cast::<u8>(), len) };
    let has = |offset: usize, size: usize| stride >= offset + size;
    if !has(offset_of!(adm_monitor_override_t, extent_width_scale), 4) {
        src.extent_width_scale = 1.0;
    }
    if !has(offset_of!(adm_monitor_override_t, extent_height_scale), 4) {
        src.extent_height_scale = 1.0;
    }
    if !has(offset_of!(adm_monitor_override_t, extent_depth_scale), 4) {
        src.extent_depth_scale = 1.0;
    }
    if !has(
        offset_of!(adm_monitor_override_t, speaker_label),
        size_of::<*const c_char>(),
    ) {
        src.speaker_label = ptr::null();
    }
    if !has(offset_of!(adm_monitor_override_t, head_locked), 4) {
        src.head_locked = 0;
    }
    if !has(offset_of!(adm_monitor_override_t, mute), 4) {
        src.mute = 0;
    }
    // A missing valid field is an old caller: every entry supplied head_locked (v1.23).
    if !has(offset_of!(adm_monitor_override_t, head_locked_valid), 4) {
        src.head_locked_valid = 1;
    }
    let finite = [
        src.gain_db,
        src.diffuse_scale,
        src.extent_scale,
        src.divergence_scale,
        src.extent_width_scale,
        src.extent_height_scale,
        src.extent_depth_scale,
    ];
    finite.iter().all(|v| v.is_finite()).then_some(src)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_set_overrides(
    monitor: *mut adm_monitor_t,
    overrides: *const adm_monitor_override_t,
    count: u32,
    revision: u64,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    // overrides may be NULL only when clearing all overrides (count == 0).
    if overrides.is_null() && count != 0 {
        return INVALID_ARGUMENT;
    }
    const UNEXPECTED: &str = "unexpected exception while applying realtime monitor overrides";
    let result = guard::guard(Err(Failure::Exception), || unsafe {
        m.clear();
        // The first element's struct_size is the array stride; it covers at least divergence_scale.
        let stride = if count > 0 {
            let stride = overrides.cast::<u32>().read_unaligned() as usize;
            if stride < offset_of!(adm_monitor_override_t, divergence_scale) + 4 {
                return Ok(INVALID_ARGUMENT);
            }
            stride
        } else {
            0
        };
        let base = overrides.cast::<u8>();
        let mut entries: Vec<adm_monitor_override_t> = Vec::new();
        entries
            .try_reserve_exact(count as usize)
            .map_err(|_| Failure::Exception)?;
        for index in 0..count as usize {
            match override_entry(base.add(index * stride), stride) {
                Some(entry) => entries.push(entry),
                None => return Ok(INVALID_ARGUMENT),
            }
        }
        let mut records: Vec<RenderLiveOverride> = Vec::new();
        records
            .try_reserve_exact(entries.len())
            .map_err(|_| Failure::Exception)?;
        let mut capacity = 0usize;
        for e in &entries {
            let object_id = text(e.object_id);
            capacity = capacity.max(object_id.len() + 1);
            let head_locked = e.head_locked_valid != 0;
            records.push(RenderLiveOverride {
                base: RenderOverride {
                    object_id: Text::bytes(object_id),
                    speaker_label: Text::bytes(text(e.speaker_label)),
                    mute: u32::from(e.mute != 0),
                    gain_db: e.gain_db,
                    has_head_locked: u32::from(head_locked),
                    head_locked: u32::from(head_locked && e.head_locked != 0),
                },
                extent_scale: e.extent_scale,
                extent_width_scale: e.extent_width_scale,
                extent_height_scale: e.extent_height_scale,
                extent_depth_scale: e.extent_depth_scale,
                diffuse_scale: e.diffuse_scale,
                divergence_scale: e.divergence_scale,
            });
        }
        Texts::new(capacity)?.run(|ms, ml, c, cl| {
            mradm_engine_monitor_session_overrides(
                m.session,
                records.as_ptr(),
                records.len(),
                revision,
                ms,
                ml,
                c,
                cl,
            )
        })?;
        Ok(OK)
    });
    match result {
        Ok(code) => code,
        // The override errors keep their former form: `message: locator`, the code unmapped.
        Err(Failure::Error(e))
            if concat(&[&e.message, b": ", &e.context])
                .is_ok_and(|text| m.store_message(&text)) =>
        {
            e.code
        }
        Err(_) => {
            m.store_message(UNEXPECTED.as_bytes());
            INTERNAL
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_set_listener_orientation(
    monitor: *mut adm_monitor_t,
    yaw_deg: f32,
    pitch_deg: f32,
    roll_deg: f32,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    if !yaw_deg.is_finite() || !pitch_deg.is_finite() || !roll_deg.is_finite() {
        return INVALID_ARGUMENT;
    }
    m.call(
        Some("unexpected exception while setting realtime monitor listener orientation"),
        || {
            unsafe {
                mradm_engine_monitor_session_orientation(m.session, yaw_deg, pitch_deg, roll_deg)
            };
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_switch_backend(
    monitor: *mut adm_monitor_t,
    opts: *const adm_render_options_t,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    m.call(
        Some("unexpected exception while switching realtime monitor backend"),
        || {
            let options = unsafe { Options::of(opts)? };
            let options = options.get();
            let request = options.request(&m.input, None)?;
            Texts::new(0)?.run(|ms, ml, c, cl| unsafe {
                mradm_engine_monitor_session_switch(
                    m.session,
                    &request.record,
                    u32::from(options.monitor_system_spatial),
                    ms,
                    ml,
                    c,
                    cl,
                )
            })?;
            Ok(OK)
        },
    )
}

/// `prepare_hptf` and `set_hptf_prepared`: the request of `kind` (0 file, 1 parameters, 2 stereo
/// files, 3 stereo parameters) as Rust settings, designed and published by the session.
unsafe fn apply_hptf(m: &adm_monitor_t, config: *const c_void, kind: u32) -> Result<(), Failure> {
    let mut message = [0u8; 512];
    let mut raw: *mut Settings = ptr::null_mut();
    let status = unsafe {
        mradm_dsp_hptf_settings_create(config, kind, &mut raw, message.as_mut_ptr(), message.len())
    };
    if status != 0 {
        return Err(Error::new(status, error::until_nul(&message), b"")?.into());
    }
    struct Owned(*mut Settings);
    impl Drop for Owned {
        fn drop(&mut self) {
            unsafe { mradm_dsp_hptf_settings_destroy(self.0) };
        }
    }
    let settings = Owned(raw);
    Texts::new(0)?.run(|ms, ml, c, cl| unsafe {
        mradm_engine_monitor_session_hptf(m.session, settings.0.cast(), ms, ml, c, cl)
    })
}

fn monitor_hptf(monitor: *mut adm_monitor_t, config: *const c_void, kind: u32) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    m.call(None, || {
        unsafe { apply_hptf(m, config, kind)? };
        Ok(OK)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_set_hptf(
    monitor: *mut adm_monitor_t,
    config: *const adm_hptf_config_t,
) -> i32 {
    monitor_hptf(monitor, config.cast(), 0)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_set_hptf_parameters(
    monitor: *mut adm_monitor_t,
    parameters: *const adm_hptf_parameters_t,
) -> i32 {
    monitor_hptf(monitor, parameters.cast(), 1)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_set_hptf_stereo(
    monitor: *mut adm_monitor_t,
    config: *const adm_hptf_stereo_config_t,
) -> i32 {
    monitor_hptf(monitor, config.cast(), 2)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_set_hptf_stereo_parameters(
    monitor: *mut adm_monitor_t,
    parameters: *const adm_hptf_stereo_parameters_t,
) -> i32 {
    monitor_hptf(monitor, parameters.cast(), 3)
}

/// `hptf_stereo_info`: the left and right infos and whether they are the same curve; a session
/// without a snapshot reports the default (bypassed, symmetric) infos.
fn stereo_info(m: &adm_monitor_t) -> (adm_hptf_info_t, adm_hptf_info_t, bool) {
    let mut snapshot: Snapshot = unsafe { std::mem::zeroed() };
    if unsafe { mradm_engine_monitor_session_hptf_snapshot(m.session, &mut snapshot) } == 0 {
        let empty = adm_hptf_info_t::empty();
        return (empty, empty, true);
    }
    hptf_infos(&snapshot)
}

/// `monitor_hptf_info`: no clear, and only a failed query stores its message.
fn monitor_hptf_info<T>(
    monitor: *mut adm_monitor_t,
    out: *mut T,
    write: impl FnOnce(*mut T, (adm_hptf_info_t, adm_hptf_info_t, bool)) -> Result<(), Failure>,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    if out.is_null() || unsafe { out.cast::<u32>().read_unaligned() } < 4 {
        return INVALID_ARGUMENT;
    }
    guard::code(|| match write(out, stereo_info(m)) {
        Ok(()) => OK,
        Err(Failure::Error(e)) => {
            if m.store(&e) {
                error::map(e.code)
            } else {
                INTERNAL
            }
        }
        Err(Failure::Exception) => INTERNAL,
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_get_hptf_info(
    monitor: *mut adm_monitor_t,
    out: *mut adm_hptf_info_t,
) -> i32 {
    monitor_hptf_info(monitor, out, |out, (left, _, symmetric)| {
        if !symmetric {
            return Err(Failure::error(
                2,
                "左右 HpTF 不同,请使用双耳状态查询".as_bytes(),
                b"",
            ));
        }
        unsafe { write_sized(out, left) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_get_hptf_stereo_info(
    monitor: *mut adm_monitor_t,
    out: *mut adm_hptf_stereo_info_t,
) -> i32 {
    monitor_hptf_info(monitor, out, |out, (left, right, _)| {
        unsafe { write_sized(out, adm_hptf_stereo_info_t::of(left, right)) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_set_output_device(
    monitor: *mut adm_monitor_t,
    device_id: *const c_char,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    m.call(
        Some("unexpected exception while switching realtime monitor output device"),
        || {
            let device = unsafe { text(device_id) };
            Texts::new(0)?.run(|ms, ml, c, cl| unsafe {
                mradm_engine_monitor_session_output_device(
                    m.session,
                    device.as_ptr(),
                    device.len(),
                    ms,
                    ml,
                    c,
                    cl,
                )
            })?;
            Ok(OK)
        },
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_get_status(
    monitor: *mut adm_monitor_t,
    out: *mut adm_monitor_status_t,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    if out.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| {
        let mut s: EngineLiveMonitorStatus = unsafe { std::mem::zeroed() };
        let caller = unsafe { out.cast::<u32>().read_unaligned() };
        let filled = if unsafe { mradm_engine_monitor_session_status(m.session, &mut s) } == 0 {
            // An invalid session is a stopped, failed snapshot.
            adm_monitor_status_t {
                struct_size: caller,
                state: 0,
                playhead_frames: 0,
                underruns: 0,
                buffered_frames: 0,
                ring_fill: 0.0,
                ended: 0,
                failed: 1,
                override_revision: 0,
            }
        } else {
            adm_monitor_status_t {
                struct_size: caller,
                state: i32::from(s.state as u8),
                playhead_frames: s.playhead_frames,
                underruns: s.underruns,
                buffered_frames: s.buffered_frames,
                ring_fill: s.ring_fill,
                ended: i32::from(s.ended != 0),
                failed: i32::from(s.failed != 0),
                override_revision: s.override_revision,
            }
        };
        // struct_size forward-compat: copy only the bytes the caller's struct holds.
        let len = (caller as usize).min(size_of::<adm_monitor_status_t>());
        unsafe { ptr::copy_nonoverlapping(ptr::from_ref(&filled).cast::<u8>(), out.cast(), len) };
        OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_get_levels(
    monitor: *mut adm_monitor_t,
    out: *mut adm_monitor_levels_t,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    let Some(out) = (unsafe { out.as_mut() }) else {
        return INVALID_ARGUMENT;
    };
    guard::code(|| {
        let mut l: EngineLiveMonitorLevels = unsafe { std::mem::zeroed() };
        // An invalid session reports silent levels (no channels).
        if unsafe { mradm_engine_monitor_session_levels(m.session, &mut l) } == 0 {
            l = unsafe { std::mem::zeroed() };
        }
        out.out_count = l.channels;
        for c in 0..out.capacity.min(l.channels) as usize {
            // `std::array::at` past the meter's channels threw.
            let (Some(&peak), Some(&rms)) = (l.peak.get(c), l.rms.get(c)) else {
                return INTERNAL;
            };
            if !out.peak.is_null() {
                unsafe { *out.peak.add(c) = peak };
            }
            if !out.rms.is_null() {
                unsafe { *out.rms.add(c) = rms };
            }
        }
        // Only the LUFS fields the caller's struct_size reserves (v1.18).
        if out.struct_size as usize >= offset_of!(adm_monitor_levels_t, integrated_lufs) + 4 {
            out.momentary_lufs = l.momentary_lufs;
            out.shortterm_lufs = l.shortterm_lufs;
            out.integrated_lufs = l.integrated_lufs;
        }
        OK
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_log_count(monitor: *mut adm_monitor_t) -> u32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return 0;
    };
    // The former `static_cast<uint32_t>` of the count.
    guard::guard(
        0,
        || unsafe { mradm_engine_monitor_session_log_count(m.session) } as u32,
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_log_entry(
    monitor: *mut adm_monitor_t,
    index: u32,
    out_level: *mut i32,
    out_module: *mut *const c_char,
    out_message: *mut *const c_char,
) -> i32 {
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return 0;
    };
    guard::guard(0, || {
        let mut log = lock(&m.log);
        let mut level = 0u32;
        let (mut module, mut message) = (Text::bytes(b""), Text::bytes(b""));
        if unsafe {
            mradm_engine_monitor_session_log_entry(
                m.session,
                index as usize,
                &mut level,
                &mut module,
                &mut message,
            )
        } == 0
        {
            return 0;
        }
        let bytes = |t: &Text| {
            if t.len == 0 {
                &[][..]
            } else {
                unsafe { std::slice::from_raw_parts(t.data, t.len) }
            }
        };
        if !log.0.set(bytes(&module)) || !log.1.set(bytes(&message)) {
            return 0;
        }
        unsafe {
            if let Some(out) = out_level.as_mut() {
                *out = level as i32;
            }
            if let Some(out) = out_module.as_mut() {
                *out = log.0.as_ptr();
            }
            if let Some(out) = out_message.as_mut() {
                *out = log.1.as_ptr();
            }
        }
        1
    })
}

fn control(m: &adm_monitor_t) -> Option<Control> {
    Control::new(unsafe { mradm_engine_monitor_session_room_control(m.session) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_switch_room(
    monitor: *mut adm_monitor_t,
    config: *const adm_room_config_t,
    out_request: *mut u64,
    out_error: *mut *mut c_char,
) -> i32 {
    if let Some(r) = unsafe { out_request.as_mut() } {
        *r = 0;
    }
    if let Some(e) = unsafe { out_error.as_mut() } {
        *e = ptr::null_mut();
    }
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    if out_request.is_null() || out_error.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| unsafe { switch_room(control(m), config, out_request, out_error) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_room_status_json(
    monitor: *const adm_monitor_t,
    out_json: *mut *mut c_char,
) -> i32 {
    if let Some(out) = unsafe { out_json.as_mut() } {
        *out = ptr::null_mut();
    }
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    if out_json.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| unsafe { room_status_json(control(m), out_json) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_monitor_room_source_json(
    monitor: *const adm_monitor_t,
    element_id: u64,
    out_json: *mut *mut c_char,
) -> i32 {
    if let Some(out) = unsafe { out_json.as_mut() } {
        *out = ptr::null_mut();
    }
    let Some(m) = (unsafe { monitor_ref(monitor) }) else {
        return INVALID_ARGUMENT;
    };
    if out_json.is_null() {
        return INVALID_ARGUMENT;
    }
    guard::code(|| unsafe { room_source_json(control(m), element_id, out_json) })
}

// ── AutoEq ParametricEQ text ─────────────────────────────────────────────────────

/// A parsed profile: the preamp and the bands.
struct Profile {
    preamp_db: f64,
    bands: Vec<Band>,
}

/// The validation error's locator, formatted on the stack before the fallible copy.
fn band_locator(index: usize) -> Result<Vec<u8>, Failure> {
    if index == 0 {
        return Ok(Vec::new());
    }
    let mut buffer = [0u8; b"band=".len() + usize::MAX.ilog10() as usize + 1];
    let capacity = buffer.len();
    let mut output = buffer.as_mut_slice();
    write!(output, "band={index}").map_err(|_| Failure::Exception)?;
    let written = capacity - output.len();
    concat(&[&buffer[..written]])
}

/// `parse_parametric_eq`: the grammar in Rust, the messages and line locators as the C++ wrote them.
fn parse(text: &[u8]) -> Result<Profile, Failure> {
    let mut bands: Vec<Band> = Vec::new();
    let grow = |bands: &mut Vec<Band>, len: usize| -> Result<(), Failure> {
        bands
            .try_reserve_exact(len.saturating_sub(bands.len()))
            .map_err(|_| Failure::Exception)?;
        while bands.len() < len {
            bands.push(unsafe { std::mem::zeroed() });
        }
        Ok(())
    };
    grow(&mut bands, 16)?;
    let mut result: ParseResult = unsafe { std::mem::zeroed() };
    loop {
        let mut message = [0u8; 256];
        if unsafe {
            mradm_dsp_hptf_parse(
                text.as_ptr(),
                text.len(),
                bands.as_mut_ptr(),
                bands.len(),
                &mut result,
                message.as_mut_ptr(),
                message.len(),
            )
        } != 0
        {
            return Err(Failure::error(
                6,
                "HpTF:ParametricEQ 解析失败".as_bytes(),
                error::until_nul(&message),
            ));
        }
        if result.error != 0 || result.band_count <= bands.len() {
            break;
        }
        grow(&mut bands, result.band_count)?;
    }
    let line = || {
        if result.line_offset > text.len() || result.line_len > text.len() - result.line_offset {
            &[][..]
        } else {
            &text[result.line_offset..result.line_offset + result.line_len]
        }
    };
    let invalid =
        |message: &str, locator: &[u8]| Err(Failure::error(1, message.as_bytes(), locator));
    match result.error {
        0 => {}
        1 => return invalid("HpTF:Preamp 行数值无法解析", line()),
        2 => return invalid("HpTF:滤波器行缺少可解析的 Fc", line()),
        3 => return invalid("HpTF:Gain 或 Q 数值无法解析", line()),
        _ => return invalid("HpTF:没有可用的 ParametricEQ 参数", b""),
    }
    bands.truncate(result.band_count);
    let mut message = [0u8; 256];
    let mut invalid_band = 0usize;
    let status = unsafe {
        mradm_dsp_hptf_validate(
            bands.as_ptr(),
            bands.len(),
            result.preamp_db,
            &mut invalid_band,
            message.as_mut_ptr(),
            message.len(),
        )
    };
    if status != 0 {
        let locator = band_locator(invalid_band)?;
        return Err(Failure::error(status, error::until_nul(&message), &locator));
    }
    Ok(Profile {
        preamp_db: result.preamp_db,
        bands,
    })
}

/// `hptf_output_stride`: the caller's band array stride (the first band's `struct_size`, which
/// every band repeats and which covers `q`); `None` for an invalid array.
unsafe fn output_stride(output: *const adm_hptf_band_t, count: u32) -> Option<usize> {
    if count == 0 {
        return Some(0);
    }
    if output.is_null() {
        return None;
    }
    let bytes = output.cast::<u8>();
    let stride = unsafe { bytes.cast::<u32>().read_unaligned() } as usize;
    if stride < size_of::<adm_hptf_band_t>() || count as usize > usize::MAX / stride {
        return None;
    }
    (0..count as usize)
        .all(|i| unsafe { bytes.add(i * stride).cast::<u32>().read_unaligned() } as usize == stride)
        .then_some(stride)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn adm_hptf_parse_parametric_eq(
    context: *mut adm_context_t,
    text: *const c_char,
    out_preamp_db: *mut f64,
    out_bands: *mut adm_hptf_band_t,
    capacity: u32,
    out_count: *mut u32,
) -> i32 {
    if context.is_null() {
        return INVALID_ARGUMENT;
    }
    caught(context, None, || unsafe {
        clear(context);
        let invalid =
            |message: &str| report(context, Failure::error(1, message.as_bytes(), b""), None);
        if text.is_null()
            || out_preamp_db.is_null()
            || out_count.is_null()
            || (out_bands.is_null() && capacity != 0)
        {
            return invalid("HpTF:解析输入或输出指针无效");
        }
        let profile = match parse(c_str(text)) {
            Ok(profile) => profile,
            Err(failure) => return report(context, failure, None),
        };
        let Ok(count) = u32::try_from(profile.bands.len()) else {
            return invalid("HpTF:解析结果的段数超出范围");
        };
        if out_bands.is_null() && capacity == 0 {
            *out_count = count;
            *out_preamp_db = profile.preamp_db;
            return OK;
        }
        if capacity < count {
            let code = invalid("HpTF:输出数组容量不足");
            *out_count = count;
            *out_preamp_db = profile.preamp_db;
            return code;
        }
        let Some(stride) = output_stride(out_bands, count) else {
            return invalid("HpTF:输出数组的 struct_size 或 stride 无效");
        };
        let base = out_bands.cast::<u8>();
        for (index, band) in profile.bands.iter().enumerate() {
            let value = adm_hptf_band_t::of(stride as u32, band);
            let len = stride.min(size_of::<adm_hptf_band_t>());
            ptr::copy_nonoverlapping(
                ptr::from_ref(&value).cast::<u8>(),
                base.add(index * stride),
                len,
            );
        }
        *out_count = count;
        *out_preamp_db = profile.preamp_db;
        OK
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::with_allocation_failure;

    #[test]
    fn band_locators_recover_from_allocation_failure() {
        let (empty, failed) = with_allocation_failure(|| band_locator(0));
        assert!(!failed);
        assert!(empty.unwrap().is_empty());
        for index in [1, 9, 10, 32, usize::MAX] {
            let (result, failed) = with_allocation_failure(|| band_locator(index));
            assert!(failed);
            assert!(matches!(result, Err(Failure::Exception)));
            assert_eq!(
                band_locator(index).unwrap(),
                format!("band={index}").as_bytes()
            );
        }
    }

    #[test]
    fn parametric_eq_allocation_failure_preserves_outputs_and_allows_retry() {
        let context = crate::context::adm_create_context();
        assert!(!context.is_null());
        let mut preamp = 123.0;
        let mut count = 456;
        let invalid = c"Filter 1: ON PK Fc -1 Hz Gain 0 dB Q 1";
        let parse = |text: *const c_char, preamp: &mut f64, count: &mut u32| unsafe {
            adm_hptf_parse_parametric_eq(context, text, preamp, ptr::null_mut(), 0, count)
        };
        let (code, failed) =
            with_allocation_failure(|| parse(invalid.as_ptr(), &mut preamp, &mut count));
        assert!(failed);
        assert_eq!(code, INTERNAL);
        assert_eq!((preamp, count), (123.0, 456));
        assert_eq!(
            parse(invalid.as_ptr(), &mut preamp, &mut count),
            INVALID_ARGUMENT
        );
        assert_eq!((preamp, count), (123.0, 456));
        let message = unsafe {
            std::ffi::CStr::from_ptr(crate::context::adm_context_last_error_message(context))
        };
        assert_eq!(
            message.to_bytes(),
            "HpTF:滤波器类型必须有效,参数必须为有限数且 Fc/Q 为正: band=1".as_bytes()
        );
        assert_eq!(
            parse(c"Preamp: -6 dB".as_ptr(), &mut preamp, &mut count),
            OK
        );
        assert_eq!((preamp, count), (-6.0, 0));
        unsafe {
            assert_eq!(*crate::context::adm_context_last_error_message(context), 0);
            crate::context::adm_destroy_context(context);
        }
    }

    #[test]
    fn device_names_escape_only_the_former_characters() {
        let mut json = Vec::new();
        push_escaped(&mut json, b"a\"b\\c\nd\re\tf\x01");
        assert_eq!(json, b"a\\\"b\\\\c\\nd\\re\\tf\x01");
    }

    #[test]
    fn parametric_eq_errors_keep_their_messages() {
        let message = |text: &[u8]| match parse(text) {
            Err(Failure::Error(e)) => (
                e.code,
                String::from_utf8(e.message).unwrap(),
                String::from_utf8(e.context).unwrap(),
            ),
            Err(Failure::Exception) => panic!("exception"),
            Ok(_) => (0, String::new(), String::new()),
        };
        assert_eq!(
            message(b""),
            (1, "HpTF:没有可用的 ParametricEQ 参数".into(), String::new())
        );
        let profile = parse(b"Preamp: -6.5 dB\nFilter 1: ON PK Fc 105 Hz Gain -2.0 dB Q 0.70\n")
            .ok()
            .unwrap();
        assert_eq!(profile.preamp_db, -6.5);
        assert_eq!(profile.bands.len(), 1);
        assert_eq!(profile.bands[0].frequency, 105.0);
    }

    #[test]
    fn override_entries_fill_the_fields_of_older_callers() {
        let mut entry = unsafe { MaybeUninit::<adm_monitor_override_t>::zeroed().assume_init() };
        entry.struct_size = (offset_of!(adm_monitor_override_t, divergence_scale) + 4) as u32;
        entry.gain_db = -3.0;
        entry.extent_width_scale = 7.0;
        entry.head_locked = 1;
        let read =
            unsafe { override_entry(ptr::from_ref(&entry).cast(), entry.struct_size as usize) }
                .unwrap();
        assert_eq!(read.gain_db, -3.0);
        assert_eq!(read.extent_width_scale, 1.0);
        assert_eq!((read.head_locked, read.head_locked_valid), (0, 1));
        entry.struct_size = size_of::<adm_monitor_override_t>() as u32;
        entry.gain_db = f32::NAN;
        assert!(
            unsafe { override_entry(ptr::from_ref(&entry).cast(), entry.struct_size as usize) }
                .is_none()
        );
    }

    #[test]
    fn null_monitors_are_rejected() {
        unsafe {
            assert_eq!(adm_monitor_play(ptr::null_mut()), INVALID_ARGUMENT);
            assert_eq!(adm_monitor_log_count(ptr::null_mut()), 0);
            assert_eq!(*adm_monitor_last_error_message(ptr::null()), 0);
            let mut json = ptr::null_mut();
            assert_eq!(
                adm_monitor_room_status_json(ptr::null(), &mut json),
                INVALID_ARGUMENT
            );
            assert!(json.is_null());
            adm_destroy_monitor(ptr::null_mut());
            let mut count = 0u32;
            let mut preamp = 0.0;
            assert_eq!(
                adm_hptf_parse_parametric_eq(
                    ptr::null_mut(),
                    c"".as_ptr(),
                    &mut preamp,
                    ptr::null_mut(),
                    0,
                    &mut count
                ),
                INVALID_ARGUMENT
            );
        }
    }
}
