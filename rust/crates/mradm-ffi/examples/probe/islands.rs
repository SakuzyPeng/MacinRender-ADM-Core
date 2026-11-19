//! The C++ islands of the room device probes (feature `islands`): the island tables, the output
//! devices, a measuring proxy over the device table, the capture shim (`tests/manual/capture_tap.c`)
//! and the recordings, histograms and distributions of their reports.
//!
//! The measuring proxy uses the same device table as the Scene output.
#![allow(dead_code)]
use mradm_ffi::api::islands::{
    AppleIslands, DeviceEnumeration, DeviceIslands, DevicePull, DeviceTable, EngineIslandError,
    EngineSceneDeviceProgress, PlatformIslands, Text,
};
use mradm_json::{Map, Value};
use std::ffi::{CStr, c_char, c_void};
use std::path::Path;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Instant;

unsafe extern "C" {
    fn mradm_device_islands() -> *const DeviceIslands;
    fn mradm_platform_islands() -> *const PlatformIslands;
    #[cfg(target_os = "macos")]
    fn mradm_apple_islands() -> *const AppleIslands;
}

/// The device island table (process lifetime).
pub fn devices() -> *const DeviceIslands {
    unsafe { mradm_device_islands() }
}

/// The platform island table (process lifetime).
pub fn platform() -> *const PlatformIslands {
    unsafe { mradm_platform_islands() }
}

/// The Apple island table on macOS, null elsewhere.
pub fn apple() -> *const AppleIslands {
    #[cfg(target_os = "macos")]
    return unsafe { mradm_apple_islands() };
    #[cfg(not(target_os = "macos"))]
    std::ptr::null()
}

/// A JSON object of `(key, value)` pairs.
pub fn obj<const N: usize>(pairs: [(&str, Value); N]) -> Value {
    Value::Object(
        pairs
            .into_iter()
            .map(|(k, v)| (k.to_owned(), v))
            .collect::<Map>(),
    )
}

/// One compact JSON line on stdout.
pub fn print(value: &Value) {
    use std::io::Write;
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{}", mradm_json::dump(value, None));
    let _ = out.flush();
}

/// An output device: its island id, name and whether it is the default.
#[derive(Clone, Debug)]
pub struct Output {
    pub id: String,
    pub name: String,
    pub default: bool,
}

fn text(t: &Text) -> String {
    if t.data.is_null() || t.len == 0 {
        return String::new();
    }
    String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(t.data, t.len) }).into_owned()
}

/// The playback devices the islands enumerate.
pub fn outputs() -> Vec<Output> {
    unsafe extern "C" fn each(user: *mut c_void, id: Text, name: Text, default: u32) {
        let list = unsafe { &mut *user.cast::<Vec<Output>>() };
        list.push(Output {
            id: text(&id),
            name: text(&name),
            default: default != 0,
        });
    }
    let mut list = Vec::new();
    let enumeration = DeviceEnumeration {
        user: (&raw mut list).cast(),
        each: Some(each),
    };
    if let Some(enumerate) = unsafe { (*devices()).enumerate } {
        unsafe { enumerate(&enumeration) };
    }
    list
}

/// The output whose id or name is exactly `selection`; exactly one must match.
pub fn select(selection: &str) -> Result<Output, String> {
    let matches: Vec<Output> = outputs()
        .into_iter()
        .filter(|o| o.id == selection || o.name == selection)
        .collect();
    match <[Output; 1]>::try_from(matches) {
        Ok([one]) => Ok(one),
        Err(_) => {
            Err("Output selection must match exactly one current device name or ID".to_owned())
        }
    }
}

// ── Reports ──────────────────────────────────────────────────────────────────────

/// Count, min, p50, p99, p99.9 and max of `values` (nearest rank below).
pub fn distribution(mut values: Vec<f64>) -> Value {
    if values.is_empty() {
        return obj([("count", 0u64.into())]);
    }
    values.sort_by(f64::total_cmp);
    let at = |p: f64| values[(p * (values.len() - 1) as f64) as usize];
    obj([
        ("count", values.len().into()),
        ("min", values[0].into()),
        ("p50", at(0.5).into()),
        ("p99", at(0.99).into()),
        ("p999", at(0.999).into()),
        ("max", values[values.len() - 1].into()),
    ])
}

/// Fixed 20 us bins; the last bin holds longer values, with the exact maximum kept.
pub struct Histogram {
    bins: Box<[u64; 5001]>,
    count: u64,
    maximum_ms: f64,
}

impl Default for Histogram {
    fn default() -> Self {
        Self {
            bins: Box::new([0; 5001]),
            count: 0,
            maximum_ms: 0.,
        }
    }
}

impl Histogram {
    pub fn add(&mut self, ms: f64) {
        self.bins[(ms * 50.).clamp(0., 5000.) as usize] += 1;
        self.count += 1;
        self.maximum_ms = self.maximum_ms.max(ms);
    }
    pub fn report(&self) -> Value {
        let percentile = |p: f64| {
            let mut sum = 0u64;
            for (i, bin) in self.bins.iter().enumerate() {
                sum += bin;
                if sum > (p * self.count as f64) as u64 {
                    return i as f64 / 50.;
                }
            }
            self.maximum_ms
        };
        obj([
            ("count", self.count.into()),
            ("p50_ms", percentile(0.5).into()),
            ("p99_ms", percentile(0.99).into()),
            ("p999_ms", percentile(0.999).into()),
            ("max_ms", self.maximum_ms.into()),
            ("bin_width_ms", 0.02.into()),
        ])
    }
}

#[derive(Clone, Copy, Default)]
struct Window {
    first_ns: i64,
    frames: u32,
    maximum_callback_frames: u32,
}

/// One-second stereo windows recorded once a minute (from 10 s into a run longer than 20 s,
/// otherwise from 2 s), preallocated and touched so recording never grows the resident set.
pub struct Recording {
    origin: Instant,
    rate: u32,
    first_second: u32,
    windows: Vec<Window>,
    samples: Vec<f32>,
}

impl Recording {
    pub fn new(origin: Instant, seconds: u32, enabled: bool, rate: u32) -> Self {
        let first_second = if seconds > 20 { 10 } else { 2 };
        let count = if enabled {
            ((seconds - first_second) / 60 + 1) as usize
        } else {
            0
        };
        let mut samples = vec![0f32; count * rate as usize * 2];
        for i in (0..samples.len()).step_by(1024) {
            // SAFETY: an in-bounds element; the volatile write commits the page.
            unsafe { std::ptr::write_volatile(&raw mut samples[i], 0.) };
        }
        Self {
            origin,
            rate,
            first_second,
            windows: vec![Window::default(); count],
            samples,
        }
    }

    /// Records the stereo `samples` of a callback of `frames` frames entered at `now`.
    pub fn capture(&mut self, samples: &[f32], frames: usize, now: Instant) {
        let time = now.saturating_duration_since(self.origin).as_secs_f64();
        if self.windows.is_empty() || time < f64::from(self.first_second) {
            return;
        }
        let into = time - f64::from(self.first_second);
        let index = (into / 60.) as usize;
        if index >= self.windows.len() || into - index as f64 * 60. > 2. {
            return;
        }
        let rate = self.rate as usize;
        let window = &mut self.windows[index];
        if window.frames == 0 {
            window.first_ns = now.saturating_duration_since(self.origin).as_nanos() as i64;
        }
        let count = frames
            .min(rate - window.frames as usize)
            .min(samples.len() / 2);
        let start = (index * rate + window.frames as usize) * 2;
        self.samples[start..start + count * 2].copy_from_slice(&samples[..count * 2]);
        window.frames += count as u32;
        window.maximum_callback_frames = window.maximum_callback_frames.max(frames as u32);
    }

    /// Writes `<name>.f32` under `root` (when recording) and describes the windows.
    pub fn save(&self, root: &Path, name: &str) -> Result<Value, String> {
        let rows: Vec<Value> = self
            .windows
            .iter()
            .enumerate()
            .map(|(i, w)| {
                obj([
                    ("first_callback_ns", w.first_ns.into()),
                    ("frames", w.frames.into()),
                    ("maximum_callback_frames", w.maximum_callback_frames.into()),
                    ("offset_frames", (i * self.rate as usize).into()),
                ])
            })
            .collect();
        if !self.windows.is_empty() {
            let bytes: Vec<u8> = self.samples.iter().flat_map(|s| s.to_le_bytes()).collect();
            std::fs::write(root.join(format!("{name}.f32")), bytes)
                .map_err(|_| "Cannot save probe recording".to_owned())?;
        }
        Ok(obj([
            ("file", format!("{name}.f32").into()),
            ("sample_rate", self.rate.into()),
            ("channels", 2u64.into()),
            ("windows", Value::Array(rows)),
            (
                "timestamp",
                "steady clock at callback entry; first sample presentation/capture time is unknown"
                    .into(),
            ),
        ]))
    }
}

/// What the measuring proxy records on the output callback.
pub struct OutputTrace {
    pub intervals: Histogram,
    pub pull_duration: Histogram,
    pub recording: Recording,
    previous: Option<Instant>,
    pub valid_frames: u64,
    pub callbacks: u64,
}

impl OutputTrace {
    pub fn new(origin: Instant, seconds: u32, record: bool, rate: u32) -> Self {
        Self {
            intervals: Histogram::default(),
            pull_duration: Histogram::default(),
            recording: Recording::new(origin, seconds, record, rate),
            previous: None,
            valid_frames: 0,
            callbacks: 0,
        }
    }
}

// ── The measuring device proxy ───────────────────────────────────────────────────

/// The trace the next opened device records into.
static TRACE: OnceLock<Arc<Mutex<OutputTrace>>> = OnceLock::new();

/// A proxied device: the island's table, the trace and the engine's pull.
struct Measured {
    real: DeviceTable,
    trace: Arc<Mutex<OutputTrace>>,
    /// Published once before the device starts invoking callbacks.
    inner: OnceLock<DevicePull>,
}

fn real() -> &'static DeviceIslands {
    // SAFETY: the island returns its process-lifetime table.
    unsafe { &*devices() }
}

unsafe fn measured<'a>(ctx: *mut c_void) -> &'a Measured {
    // The caller keeps the context alive until the device stops. Control calls and the
    // audio callback share this reference, including when a pull reenters mark_end.
    unsafe { &*ctx.cast::<Measured>() }
}

unsafe extern "C" fn measured_pull(
    user: *mut c_void,
    out: *mut f32,
    len: usize,
    frames: usize,
) -> usize {
    let m = unsafe { measured(user) };
    let now = Instant::now();
    let Some(DevicePull {
        user,
        pull: Some(inner),
    }) = m.inner.get().copied()
    else {
        return 0;
    };
    // The audio thread is the only writer while the device runs.
    let mut trace = m.trace.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(previous) = trace.previous {
        trace.intervals.add((now - previous).as_secs_f64() * 1000.);
    }
    trace.previous = Some(now);
    trace.callbacks += 1;
    let result = unsafe { inner(user, out, len, frames) };
    trace.pull_duration.add(now.elapsed().as_secs_f64() * 1000.);
    trace.valid_frames += result as u64;
    if !out.is_null() && len != 0 {
        let samples = unsafe { std::slice::from_raw_parts_mut(out, len) };
        // The production device pads short pulls after this wrapper returns.
        let from = (result * 2).min(len);
        samples[from..].fill(0.);
        trace.recording.capture(samples, frames, now);
    }
    result
}

unsafe extern "C" fn start(
    ctx: *mut c_void,
    channels: u32,
    rate: u32,
    pull: *const DevicePull,
    error: *const EngineIslandError,
) -> i32 {
    let m = unsafe { measured(ctx) };
    let Some(real_start) = m.real.start else {
        return 0;
    };
    // A Scene output starts its device once and stops it even when start fails.
    if m.inner.set(unsafe { *pull }).is_err() {
        return mradm_engine::request::code::INVALID_ARGUMENT;
    }
    let wrapped = DevicePull {
        user: ctx,
        pull: Some(measured_pull),
    };
    unsafe { real_start(m.real.ctx, channels, rate, &wrapped, error) }
}

macro_rules! forward {
    ($name:ident) => {
        unsafe extern "C" fn $name(ctx: *mut c_void) {
            let m = unsafe { measured(ctx) };
            if let Some(f) = m.real.$name {
                unsafe { f(m.real.ctx) };
            }
        }
    };
}
forward!(stop);
forward!(flush);
forward!(pause);
forward!(resume);
forward!(mark_end);

unsafe extern "C" fn realtime(ctx: *mut c_void) -> u32 {
    let m = unsafe { measured(ctx) };
    m.real.realtime.map_or(1, |f| unsafe { f(m.real.ctx) })
}
unsafe extern "C" fn has_volume(ctx: *mut c_void) -> u32 {
    let m = unsafe { measured(ctx) };
    m.real.has_volume.map_or(0, |f| unsafe { f(m.real.ctx) })
}
unsafe extern "C" fn set_volume(ctx: *mut c_void, gain: f32) {
    let m = unsafe { measured(ctx) };
    if let Some(f) = m.real.set_volume {
        unsafe { f(m.real.ctx, gain) };
    }
}
unsafe extern "C" fn progress(ctx: *mut c_void, out: *mut EngineSceneDeviceProgress) {
    let m = unsafe { measured(ctx) };
    if let Some(f) = m.real.progress {
        unsafe { f(m.real.ctx, out) };
    }
}
unsafe extern "C" fn destroy(ctx: *mut c_void) {
    // Scene output stops and joins the native callbacks before destroying the device.
    let m = unsafe { Box::from_raw(ctx.cast::<Measured>()) };
    if let Some(f) = m.real.destroy {
        unsafe { f(m.real.ctx) };
    }
}

unsafe extern "C" fn open(
    kind: u32,
    layout: Text,
    geometry: u32,
    device_id: Text,
    out: *mut DeviceTable,
    error: *const EngineIslandError,
) -> i32 {
    let Some(real_open) = real().open else {
        return 6;
    };
    let mut table = DeviceTable::empty();
    let status = unsafe { real_open(kind, layout, geometry, device_id, &mut table, error) };
    if status != 0 {
        return status;
    }
    let Some(trace) = TRACE.get().cloned() else {
        unsafe { *out = table };
        return 0;
    };
    let ctx = Box::into_raw(Box::new(Measured {
        real: table,
        trace,
        inner: OnceLock::new(),
    }))
    .cast();
    unsafe {
        *out = DeviceTable {
            ctx,
            start: Some(start),
            stop: Some(stop),
            flush: Some(flush),
            pause: Some(pause),
            resume: Some(resume),
            mark_end: Some(mark_end),
            realtime: Some(realtime),
            has_volume: Some(has_volume),
            set_volume: Some(set_volume),
            progress: Some(progress),
            destroy: Some(destroy),
        }
    };
    0
}

static PROXY: OnceLock<DeviceIslands> = OnceLock::new();

/// The island table with every device opened afterwards measured into `trace` (set once).
pub fn measured_devices(trace: Arc<Mutex<OutputTrace>>) -> *const DeviceIslands {
    let _ = TRACE.set(trace);
    PROXY.get_or_init(|| DeviceIslands {
        system_spatial: real().system_spatial,
        open: Some(open),
        enumerate: real().enumerate,
    })
}

// ── The capture shim ─────────────────────────────────────────────────────────────

#[repr(C)]
struct CaptureHandle {
    _private: [u8; 0],
}

type CaptureFn = unsafe extern "C" fn(*mut c_void, *const f32, u32);

unsafe extern "C" {
    fn mr_probe_capture_open(
        name: *const c_char,
        loopback_token: *const c_char,
        rate: u32,
        callback: CaptureFn,
        user: *mut c_void,
        out: *mut *mut CaptureHandle,
        device_name: *mut c_char,
        device_name_len: usize,
    ) -> i32;
    fn mr_probe_capture_close(handle: *mut CaptureHandle);
    fn mr_probe_capture_inputs(
        each: unsafe extern "C" fn(*mut c_void, *const c_char, i32),
        user: *mut c_void,
    ) -> i32;
}

/// The selected capture (or loopback) input, recording into one-second windows.
pub struct Capture {
    handle: *mut CaptureHandle,
    recording: Box<Mutex<Recording>>,
    name: String,
}

unsafe extern "C" fn captured(user: *mut c_void, input: *const f32, frames: u32) {
    let recording = unsafe { &*user.cast::<Mutex<Recording>>() };
    let samples = unsafe { std::slice::from_raw_parts(input, frames as usize * 2) };
    recording.lock().unwrap_or_else(|e| e.into_inner()).capture(
        samples,
        frames as usize,
        Instant::now(),
    );
}

fn c_string(text: &str) -> Result<std::ffi::CString, String> {
    std::ffi::CString::new(text).map_err(|_| "capture name contains NUL".to_owned())
}

impl Capture {
    /// The capture device named `name`, or with a non-empty `loopback_output` the loopback of
    /// that output device id.
    pub fn open(
        origin: Instant,
        seconds: u32,
        name: &str,
        loopback_output: &str,
        rate: u32,
    ) -> Result<Self, String> {
        let recording = Box::new(Mutex::new(Recording::new(origin, seconds, true, rate)));
        let (name, token) = (c_string(name)?, c_string(loopback_output)?);
        let mut handle = std::ptr::null_mut();
        let mut device_name = [0 as c_char; 256];
        let status = unsafe {
            mr_probe_capture_open(
                name.as_ptr(),
                token.as_ptr(),
                rate,
                captured,
                std::ptr::from_ref::<Mutex<Recording>>(&recording)
                    .cast_mut()
                    .cast(),
                &mut handle,
                device_name.as_mut_ptr(),
                device_name.len(),
            )
        };
        let message = match status {
            0 => None,
            1 => Some("Capture context initialization failed"),
            2 => Some("Capture device enumeration failed"),
            3 => Some("Capture selection must match exactly one device"),
            4 => Some("Selected capture/loopback device could not be opened"),
            5 => Some("Capture start failed"),
            _ => Some("Capture allocation failed"),
        };
        if let Some(message) = message {
            return Err(message.to_owned());
        }
        let name = unsafe { CStr::from_ptr(device_name.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        Ok(Self {
            handle,
            recording,
            name,
        })
    }

    /// Stops the input; its callback has returned afterwards.
    pub fn stop(&mut self) {
        if !self.handle.is_null() {
            unsafe { mr_probe_capture_close(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }

    pub fn save(&self, root: &Path) -> Result<Value, String> {
        let mut result = self
            .recording
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .save(root, "capture")?;
        if let Value::Object(map) = &mut result {
            map.insert("device_name".to_owned(), self.name.clone().into());
        }
        Ok(result)
    }

    /// The capture devices: name and whether each is the default.
    pub fn inputs() -> Result<Value, String> {
        unsafe extern "C" fn each(user: *mut c_void, name: *const c_char, default: i32) {
            let list = unsafe { &mut *user.cast::<Vec<Value>>() };
            let name = unsafe { CStr::from_ptr(name) }
                .to_string_lossy()
                .into_owned();
            list.push(obj([
                ("name", name.into()),
                ("default", (default != 0).into()),
            ]));
        }
        let mut list: Vec<Value> = Vec::new();
        if unsafe { mr_probe_capture_inputs(each, (&raw mut list).cast()) } != 0 {
            return Err("Capture enumeration failed".to_owned());
        }
        Ok(Value::Array(list))
    }
}

impl Drop for Capture {
    fn drop(&mut self) {
        self.stop();
    }
}

// ── Process clocks and waits ──────────────────────────────────────────────────────

/// The process CPU time in seconds (`std::clock` on POSIX, `GetProcessTimes` on Windows).
pub fn cpu_seconds() -> f64 {
    #[cfg(windows)]
    {
        #[repr(C)]
        #[derive(Default, Clone, Copy)]
        struct FileTime(u32, u32);
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn GetCurrentProcess() -> *mut c_void;
            fn GetProcessTimes(
                process: *mut c_void,
                created: *mut FileTime,
                exit: *mut FileTime,
                kernel: *mut FileTime,
                user: *mut FileTime,
            ) -> i32;
        }
        let (mut c, mut e, mut k, mut u) = (
            FileTime::default(),
            FileTime::default(),
            FileTime::default(),
            FileTime::default(),
        );
        if unsafe { GetProcessTimes(GetCurrentProcess(), &mut c, &mut e, &mut k, &mut u) } == 0 {
            return 0.;
        }
        let value = |t: FileTime| (u64::from(t.1) << 32) | u64::from(t.0);
        (value(k) + value(u)) as f64 * 1e-7
    }
    #[cfg(not(windows))]
    {
        unsafe extern "C" {
            fn clock() -> std::ffi::c_long;
        }
        // CLOCKS_PER_SEC is 1e6 on the POSIX targets (XSI).
        (unsafe { clock() }) as f64 / 1e6
    }
}

/// Waits until `deadline`: on Windows in 1 ms steps of the room worker's high-resolution timer
/// (`Sleep(1)` may last a whole timer tick), elsewhere one sleep.
pub fn wait_until(deadline: Instant) {
    #[cfg(windows)]
    while Instant::now() < deadline {
        mradm_engine::backend::Host::room_wait(
            &mradm_ffi::api::render::FfiHost,
            std::time::Duration::from_millis(1),
        );
    }
    #[cfg(not(windows))]
    if let Some(wait) = deadline.checked_duration_since(Instant::now()) {
        std::thread::sleep(wait);
    }
}
