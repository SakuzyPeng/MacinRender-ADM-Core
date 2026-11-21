//! miniaudio output devices (the hardware device and the null backend), driven from Rust over the
//! narrow C interface of `src/adm_realtime/miniaudio_island.h` (7.3). The device is a
//! [`DeviceTable`] like the system-spatial sinks, so the engine, the tests and the probe proxies
//! treat every output device alike.
use super::device_island::{DevicePull, DeviceTable, output};
use super::engine_monitor::{EngineIslandError, island_error};
use mradm_engine::request::code;
use std::ffi::{CStr, c_char, c_void};
use std::ptr::{self, NonNull};
use std::sync::Mutex;

/// What an opened device reports (`MradmMiniaudioFormats`).
#[repr(C)]
pub struct MiniaudioFormats {
    pub internal_channels: u32,
    pub native_count: u32,
    pub native_channels: [u32; 64],
}

/// The audio-thread callback `init` takes: `frames * channels` interleaved floats.
pub type MiniaudioData = unsafe extern "C" fn(*mut c_void, *mut f32, u32, u32);
/// One playback device of `enumerate`: its id bytes, its name and whether it is the default.
pub type MiniaudioEach = unsafe extern "C" fn(*mut c_void, *const u8, usize, *const c_char, u32);

/// The miniaudio calls (`MradmMiniaudioIsland`).
#[repr(C)]
pub struct MiniaudioIsland {
    pub init: Option<
        unsafe extern "C" fn(
            u32,
            *const u8,
            usize,
            u32,
            u32,
            Option<MiniaudioData>,
            *mut c_void,
            *mut *mut c_void,
        ) -> i32,
    >,
    pub formats: Option<unsafe extern "C" fn(*mut c_void, *mut MiniaudioFormats)>,
    pub start: Option<unsafe extern "C" fn(*mut c_void) -> i32>,
    pub close: Option<unsafe extern "C" fn(*mut c_void)>,
    pub enumerate: Option<unsafe extern "C" fn(*mut c_void, Option<MiniaudioEach>) -> i32>,
}

unsafe extern "C" {
    fn mradm_miniaudio_island() -> *const MiniaudioIsland;
}

fn island() -> &'static MiniaudioIsland {
    // SAFETY: the island returns its process-lifetime table.
    unsafe { &*mradm_miniaudio_island() }
}

/// `ErrorCode::internal_error`.
const INTERNAL: i32 = 6;

const MULTICHANNEL: &str = "multichannel monitoring requires a multichannel device or system spatial audio; use binaural output for headphones";

/// The lowercase hex token of a device id, as enumeration names devices.
fn token(id: &[u8]) -> Vec<u8> {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    id.iter()
        .flat_map(|b| [HEX[usize::from(b >> 4)], HEX[usize::from(b & 15)]])
        .collect()
}

/// The id bytes of a token, or `None` when it is not lowercase hex (the default device). The island
/// uses only an id of its own size, so a token of another length also opens the default device.
fn id_of(token: &[u8]) -> Option<Vec<u8>> {
    let nibble = |c: u8| match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        _ => None,
    };
    if token.is_empty() || !token.len().is_multiple_of(2) {
        return None;
    }
    let (pairs, _) = token.as_chunks::<2>();
    pairs
        .iter()
        .map(|&[hi, lo]| Some((nibble(hi)? << 4) | nibble(lo)?))
        .collect()
}

/// Whether a device opened for more than two channels would fold them to stereo: its internal
/// width is at most two, or every native format it reports is mono or stereo.
fn stereo_only(f: &MiniaudioFormats) -> bool {
    let native = &f.native_channels[..(f.native_count as usize).min(64)];
    let native_stereo = !native.is_empty() && native.iter().all(|&c| (1..=2).contains(&c));
    f.internal_channels <= 2 || native_stereo
}

/// A started device: the island's handle and the pull its callback reads (boxed, so its address
/// stays fixed while the device runs).
struct Running {
    handle: NonNull<c_void>,
    _pull: Box<DevicePull>,
}

/// An opened (not yet started) miniaudio device.
struct Miniaudio {
    null_backend: bool,
    id: Option<Vec<u8>>,
    running: Mutex<Option<Running>>,
}

impl Miniaudio {
    fn start(&self, channels: u32, rate: u32, pull: DevicePull) -> Result<(), (i32, &'static str)> {
        let mut running = self.running.lock().unwrap_or_else(|e| e.into_inner());
        if running.is_some() {
            return Err((code::INVALID_ARGUMENT, "audio device already started"));
        }
        let api = island();
        let (Some(init), Some(formats), Some(start), Some(close)) =
            (api.init, api.formats, api.start, api.close)
        else {
            return Err((INTERNAL, "miniaudio device init failed"));
        };
        let pull = Box::new(pull);
        let user = ptr::from_ref::<DevicePull>(&pull).cast_mut().cast();
        let (id, id_len) = self
            .id
            .as_deref()
            .map_or((ptr::null(), 0), |id| (id.as_ptr(), id.len()));
        let mut raw = ptr::null_mut();
        let status = unsafe {
            init(
                u32::from(self.null_backend),
                id,
                id_len,
                channels,
                rate,
                Some(data),
                user,
                &mut raw,
            )
        };
        let handle = match (status, NonNull::new(raw)) {
            (0, Some(handle)) => handle,
            (1, _) => return Err((INTERNAL, "miniaudio null context init failed")),
            _ => return Err((INTERNAL, "miniaudio device init failed")),
        };
        if channels > 2 {
            let mut f = MiniaudioFormats {
                internal_channels: 0,
                native_count: 0,
                native_channels: [0; 64],
            };
            unsafe { formats(handle.as_ptr(), &mut f) };
            if stereo_only(&f) {
                unsafe { close(handle.as_ptr()) };
                return Err((code::UNSUPPORTED, MULTICHANNEL));
            }
        }
        if unsafe { start(handle.as_ptr()) } != 0 {
            unsafe { close(handle.as_ptr()) };
            return Err((INTERNAL, "miniaudio device start failed"));
        }
        *running = Some(Running {
            handle,
            _pull: pull,
        });
        Ok(())
    }

    /// Stops and closes the device; its callback has returned afterwards.
    fn stop(&self) {
        let taken = self
            .running
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .take();
        if let (Some(r), Some(close)) = (taken, island().close) {
            unsafe { close(r.handle.as_ptr()) };
        }
    }
}

/// The audio thread: drains the engine's pull into the buffer and pads a short pull with silence.
unsafe extern "C" fn data(user: *mut c_void, out: *mut f32, frames: u32, channels: u32) {
    let pull = unsafe { &*user.cast::<DevicePull>() };
    let (frames, channels) = (frames as usize, channels as usize);
    let total = frames * channels;
    let produced = pull
        .pull
        .map_or(0, |f| unsafe { f(pull.user, out, total, frames) });
    if produced < frames {
        let samples = unsafe { output(out, total) };
        samples[produced * channels..].fill(0.);
    }
}

unsafe fn device<'a>(ctx: *mut c_void) -> &'a Miniaudio {
    unsafe { &*ctx.cast::<Miniaudio>() }
}

unsafe extern "C" fn table_start(
    ctx: *mut c_void,
    channels: u32,
    rate: u32,
    pull: *const DevicePull,
    e: *const EngineIslandError,
) -> i32 {
    match unsafe { device(ctx) }.start(channels, rate, unsafe { *pull }) {
        Ok(()) => 0,
        Err((status, message)) => unsafe { island_error(e, status, message) },
    }
}

unsafe extern "C" fn table_stop(ctx: *mut c_void) {
    unsafe { device(ctx) }.stop();
}

unsafe extern "C" fn table_realtime(_: *mut c_void) -> u32 {
    1
}

unsafe extern "C" fn table_destroy(ctx: *mut c_void) {
    let device = unsafe { Box::from_raw(ctx.cast::<Miniaudio>()) };
    device.stop();
}

/// The table of a miniaudio device: the null backend, or the device of `token` (empty or unknown:
/// the default device). Opening does not touch the hardware; `start` does.
pub(crate) fn open(null_backend: bool, token: &[u8]) -> DeviceTable {
    let device = Box::new(Miniaudio {
        null_backend,
        id: id_of(token),
        running: Mutex::new(None),
    });
    DeviceTable {
        ctx: Box::into_raw(device).cast(),
        start: Some(table_start),
        stop: Some(table_stop),
        realtime: Some(table_realtime),
        destroy: Some(table_destroy),
        ..DeviceTable::empty()
    }
}

/// A playback device's token, name and whether it is the default.
type Each<'a> = dyn FnMut(&[u8], &[u8], bool) + 'a;

/// Calls `each(token, name, is_default)` for every playback device (none when the context failed).
pub(crate) fn enumerate(each: &mut Each<'_>) {
    unsafe extern "C" fn one(
        user: *mut c_void,
        id: *const u8,
        len: usize,
        name: *const c_char,
        is_default: u32,
    ) {
        let each = unsafe { &mut *user.cast::<&mut Each<'_>>() };
        let id = if id.is_null() || len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(id, len) }
        };
        let name = if name.is_null() {
            &[][..]
        } else {
            unsafe { CStr::from_ptr(name) }.to_bytes()
        };
        each(&token(id), name, is_default != 0);
    }
    if let Some(enumerate) = island().enumerate {
        let mut each = each;
        let user = ptr::from_mut(&mut each).cast();
        unsafe { enumerate(user, Some(one)) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tokens_round_trip_and_reject_what_the_former_parser_rejected() {
        let id = [0x00, 0x7f, 0xa5, 0xff];
        assert_eq!(token(&id), b"007fa5ff");
        assert_eq!(id_of(b"007fa5ff").as_deref(), Some(&id[..]));
        for bad in [&b""[..], b"0", b"0A", b"0g", b"0 "] {
            assert_eq!(id_of(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn multichannel_needs_a_wide_device_and_a_wide_native_format() {
        let formats = |internal: u32, native: &[u32]| {
            let mut f = MiniaudioFormats {
                internal_channels: internal,
                native_count: native.len() as u32,
                native_channels: [0; 64],
            };
            f.native_channels[..native.len()].copy_from_slice(native);
            f
        };
        // The internal width decides first.
        assert!(stereo_only(&formats(2, &[12])));
        assert!(stereo_only(&formats(1, &[])));
        // Without native formats (or only "any" = 0) a wide device is accepted.
        assert!(!stereo_only(&formats(12, &[])));
        assert!(!stereo_only(&formats(12, &[0])));
        assert!(!stereo_only(&formats(12, &[2, 0])));
        // Only mono and stereo native formats fold.
        assert!(stereo_only(&formats(12, &[2])));
        assert!(stereo_only(&formats(12, &[1, 2, 2])));
        assert!(!stereo_only(&formats(12, &[2, 8])));
        // A count beyond the record is clamped.
        let mut f = formats(12, &[2; 64]);
        f.native_count = 1000;
        assert!(stereo_only(&f));
    }

    unsafe extern "C" fn pulls_three(_: *mut c_void, out: *mut f32, len: usize, _: usize) -> usize {
        let samples = unsafe { std::slice::from_raw_parts_mut(out, len) };
        samples[..6].fill(1.);
        3
    }

    #[test]
    fn a_short_pull_is_padded_with_silence() {
        let mut pull = DevicePull {
            user: ptr::null_mut(),
            pull: Some(pulls_three),
        };
        let mut out = [7.0_f32; 10];
        unsafe { data(ptr::from_mut(&mut pull).cast(), out.as_mut_ptr(), 5, 2) };
        assert_eq!(out, [1., 1., 1., 1., 1., 1., 0., 0., 0., 0.]);
    }

    unsafe extern "C" fn fills(_: *mut c_void, out: *mut f32, len: usize, frames: usize) -> usize {
        unsafe { std::slice::from_raw_parts_mut(out, len) }.fill(0.5);
        frames
    }

    /// The null backend pulls in real time with the requested width; a second start is rejected.
    #[test]
    fn the_null_backend_pulls_and_rejects_a_second_start() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        static CALLS: AtomicUsize = AtomicUsize::new(0);
        unsafe extern "C" fn counted(
            user: *mut c_void,
            out: *mut f32,
            len: usize,
            frames: usize,
        ) -> usize {
            CALLS.fetch_add(1, Ordering::SeqCst);
            unsafe { fills(user, out, len, frames) }
        }
        let table = open(true, b"");
        let pull = DevicePull {
            user: ptr::null_mut(),
            pull: Some(counted),
        };
        let device = unsafe { super::super::device_island::Device::adopt(&table) };
        let started = device.start(2, 48_000, pull.user, counted);
        assert!(started.is_ok(), "{started:?}");
        let again = device.start(2, 48_000, pull.user, counted).unwrap_err();
        assert_eq!(
            (again.code, &again.message[..]),
            (code::INVALID_ARGUMENT, &b"audio device already started"[..])
        );
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while CALLS.load(Ordering::SeqCst) < 3 && std::time::Instant::now() < deadline {
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        assert!(CALLS.load(Ordering::SeqCst) >= 3);
        assert!(device.realtime());
        device.stop();
        let after = CALLS.load(Ordering::SeqCst);
        std::thread::sleep(std::time::Duration::from_millis(50));
        assert_eq!(CALLS.load(Ordering::SeqCst), after);
        // A stopped device starts again.
        assert!(device.start(2, 48_000, pull.user, counted).is_ok());
        device.stop();
    }
}
