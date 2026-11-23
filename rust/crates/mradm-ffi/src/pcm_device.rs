//! PCM output for file and Scene monitoring, with a hardware-independent clock for null output.
use super::device_island::{DevicePull, DeviceTable};
use super::engine_monitor::{EngineIslandError, island_error};
use super::engine_scene_output::EngineSceneDeviceProgress;
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{BufferSize, DeviceId, ErrorKind, FromSample, SampleFormat, SizedSample, StreamConfig};
use mradm_engine::request::code;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
#[cfg(windows)]
use std::sync::{OnceLock, mpsc};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

#[cfg(any(target_os = "macos", windows))]
const MULTICHANNEL: &str = "multichannel monitoring requires a multichannel device or system spatial audio; use binaural output for headphones";
const CLOSED: usize = 2;
const IO_TIMEOUT: Option<Duration> = Some(Duration::from_secs(2));
type Failure = (i32, String);

fn host() -> Result<cpal::Host, cpal::Error> {
    cpal::available_hosts()
        .into_iter()
        .find_map(|id| cpal::host_from_id(id).ok())
        .ok_or_else(|| ErrorKind::HostUnavailable.into())
}

fn legacy_id(token: &[u8], windows: bool) -> Option<String> {
    fn nibble(value: u8) -> Option<u8> {
        match value {
            b'0'..=b'9' => Some(value - b'0'),
            b'a'..=b'f' => Some(value - b'a' + 10),
            _ => None,
        }
    }
    let hex: &[u8; 512] = token.try_into().ok()?;
    let mut bytes = [0; 256];
    for (byte, &[hi, lo]) in bytes.iter_mut().zip(hex.as_chunks::<2>().0) {
        *byte = nibble(hi)? << 4 | nibble(lo)?;
    }
    if windows {
        let units: Vec<_> = bytes[..128]
            .as_chunks::<2>()
            .0
            .iter()
            .map(|pair| u16::from_le_bytes(*pair))
            .take_while(|&unit| unit != 0)
            .collect();
        String::from_utf16(&units).ok().filter(|id| !id.is_empty())
    } else {
        let end = bytes.iter().position(|&byte| byte == 0)?;
        std::str::from_utf8(&bytes[..end])
            .ok()
            .filter(|id| !id.is_empty())
            .map(str::to_owned)
    }
}

fn output_device(host: &cpal::Host, token: &[u8], _channels: u16) -> Option<(cpal::Device, bool)> {
    let id = std::str::from_utf8(token)
        .ok()
        .and_then(|text| text.parse::<DeviceId>().ok())
        .filter(|id| id.host() == host.id())
        .or_else(|| {
            legacy_id(token, cfg!(windows)).map(|id| {
                #[cfg(target_os = "linux")]
                let id = if id.starts_with(':') {
                    format!("hw{id}")
                } else {
                    id
                };
                DeviceId::new(host.id(), id)
            })
        });
    if let Some(device) = id
        .and_then(|id| host.device_by_id(&id))
        .filter(DeviceTrait::supports_output)
    {
        return Some((device, false));
    }
    let default = host.default_output_device()?;
    // DefaultOutput can reroute without rechecking the width. Pin multichannel CoreAudio output.
    #[cfg(target_os = "macos")]
    let default = if _channels > 2 {
        host.device_by_id(&default.id().ok()?)?
    } else {
        default
    };
    Some((default, true))
}

pub(crate) fn enumerate(mut each: impl FnMut(&[u8], &[u8], bool)) {
    let Ok(host) = host() else { return };
    let default = host
        .default_output_device()
        .and_then(|device| device.id().ok());
    if let Ok(devices) = host.output_devices() {
        for device in devices {
            if let (Ok(id), Ok(label)) = (device.id(), device.description()) {
                each(
                    id.to_string().as_bytes(),
                    label.name().as_bytes(),
                    default.as_ref() == Some(&id),
                );
            }
        }
    }
}

struct Callbacks {
    pull: DevicePull,
    channels: usize,
    gate: AtomicUsize,
    failed: AtomicBool,
    xruns: AtomicU64,
    #[cfg(windows)]
    route: OnceLock<DefaultRoute>,
}

#[cfg(windows)]
struct DefaultRoute {
    worker: thread::Thread,
    changed: AtomicBool,
    reopening: AtomicBool,
    unavailable: AtomicBool,
}

/// The caller keeps the pull target alive through stop. Gate bit 0 marks the active pull and
/// bit 1 closes admission. A late driver callback therefore only writes silence.
unsafe impl Sync for Callbacks {}
unsafe impl Send for Callbacks {}

impl Callbacks {
    fn new(pull: DevicePull, channels: u16) -> Self {
        Self {
            pull,
            channels: channels.into(),
            gate: AtomicUsize::new(0),
            failed: AtomicBool::new(false),
            xruns: AtomicU64::new(0),
            #[cfg(windows)]
            route: OnceLock::new(),
        }
    }

    fn fill(&self, buffer: &mut [f32], begins_callback: bool) {
        if buffer.is_empty() {
            return;
        }
        #[cfg(windows)]
        if self
            .route
            .get()
            .is_some_and(|route| route.changed.load(Ordering::Acquire))
        {
            buffer.fill(0.0);
            return;
        }
        if self.failed.load(Ordering::Acquire)
            || !buffer.len().is_multiple_of(self.channels)
            || self
                .gate
                .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
                .is_err()
        {
            buffer.fill(0.0);
            return;
        }
        let frames = buffer.len() / self.channels;
        let produced = self.pull.pull.map_or(0, |pull| unsafe {
            pull(
                self.pull.user,
                buffer.as_mut_ptr(),
                buffer.len(),
                frames,
                u32::from(begins_callback),
            )
        });
        buffer[produced.min(frames) * self.channels..].fill(0.0);
        self.gate.fetch_sub(1, Ordering::Release);
    }

    fn event(&self, kind: ErrorKind) {
        #[cfg(windows)]
        if matches!(
            kind,
            ErrorKind::StreamInvalidated | ErrorKind::DeviceNotAvailable
        ) && let Some(route) = self.route.get()
        {
            route.changed.store(true, Ordering::Release);
            route.worker.unpark();
            return;
        }
        match kind {
            ErrorKind::DeviceChanged | ErrorKind::RealtimeDenied => {}
            ErrorKind::Xrun => {
                self.xruns.fetch_add(1, Ordering::Relaxed);
            }
            _ => self.failed.store(true, Ordering::Release),
        }
    }

    fn close(&self) {
        self.gate.fetch_or(CLOSED, Ordering::AcqRel);
    }

    fn closed(&self) -> bool {
        self.gate.load(Ordering::Acquire) & CLOSED != 0
    }

    fn health(&self) -> (bool, bool) {
        let failed = self.failed.load(Ordering::Acquire);
        #[cfg(windows)]
        if let Some(route) = self.route.get() {
            return (
                failed || route.unavailable.load(Ordering::Acquire),
                route.changed.load(Ordering::Acquire) || route.reopening.load(Ordering::Acquire),
            );
        }
        (failed, false)
    }

    fn drain(&self) {
        while self.gate.load(Ordering::Acquire) != CLOSED {
            thread::yield_now();
        }
    }
}

fn integer_pcm<Sample: FromSample<i64>>(sample: f32) -> Sample {
    Sample::from_sample_(i64::from_sample_(sample))
}

fn converted_stream<Sample: SizedSample>(
    device: &cpal::Device,
    config: StreamConfig,
    callbacks: Arc<Callbacks>,
    convert: impl Fn(f32) -> Sample + Send + 'static,
) -> Result<cpal::Stream, cpal::Error> {
    let events = Arc::clone(&callbacks);
    let mut scratch = vec![0.0; 1024 * callbacks.channels];
    device.build_output_stream(
        config,
        move |output: &mut [Sample], _| {
            for (index, chunk) in output.chunks_mut(scratch.len()).enumerate() {
                let samples = &mut scratch[..chunk.len()];
                callbacks.fill(samples, index == 0);
                for (dest, &sample) in chunk.iter_mut().zip(samples.iter()) {
                    *dest = convert(sample);
                }
            }
        },
        move |error| events.event(error.kind()),
        IO_TIMEOUT,
    )
}

fn play_hardware(
    device: &cpal::Device,
    config: StreamConfig,
    callbacks: &Arc<Callbacks>,
) -> Result<cpal::Stream, Failure> {
    #[cfg(target_os = "macos")]
    if config.channels > 2
        && !device
            .supported_output_configs()
            .map_err(driver_error)?
            .any(|range| range.channels() >= config.channels)
    {
        return Err((code::UNSUPPORTED, MULTICHANNEL.into()));
    }
    #[cfg(windows)]
    if config.channels > 2
        && config.channels
            > device
                .default_output_config()
                .map_err(driver_error)?
                .channels()
    {
        return Err((code::UNSUPPORTED, MULTICHANNEL.into()));
    }
    let pull = Arc::clone(callbacks);
    let events = Arc::clone(callbacks);
    let result = device.build_output_stream(
        config,
        move |out: &mut [f32], _| pull.fill(out, true),
        move |error| events.event(error.kind()),
        IO_TIMEOUT,
    );
    let stream = match result {
        Err(error) if error.kind() == ErrorKind::UnsupportedConfig => {
            let Ok(mix) = device.default_output_config() else {
                return Err(driver_error(error));
            };
            macro_rules! converted {
                ($($variant:ident => $sample:ty),+) => {
                    match mix.sample_format() {
                        $(SampleFormat::$variant => converted_stream(device, config, Arc::clone(callbacks), integer_pcm::<$sample>),)+
                        SampleFormat::F64 => converted_stream(device, config, Arc::clone(callbacks), f64::from),
                        _ => Err(error),
                    }
                }
            }
            converted!(I8 => i8, I16 => i16, I24 => cpal::I24, I32 => i32, I64 => i64,
                U8 => u8, U16 => u16, U24 => cpal::U24, U32 => u32, U64 => u64)
        }
        result => result,
    }.map_err(driver_error)?;
    stream.play().map_err(driver_error)?;
    Ok(stream)
}

#[cfg(windows)]
fn follow_default_output<S>(
    callbacks: Arc<Callbacks>,
    open: impl Fn(&Arc<Callbacks>) -> Result<S, Failure> + Send + 'static,
) -> Result<JoinHandle<()>, Failure> {
    let (ready, setup) = mpsc::sync_channel(1);
    let worker = thread::Builder::new()
        .name("mradm-pcm-default-output".into())
        .spawn(move || {
            let route = callbacks.route.get_or_init(|| DefaultRoute {
                worker: thread::current(),
                changed: AtomicBool::new(false),
                reopening: AtomicBool::new(false),
                unavailable: AtomicBool::new(false),
            });
            let mut stream = match open(&callbacks) {
                Ok(stream) => stream,
                Err(error) => {
                    let _ = ready.send(Err(error));
                    return;
                }
            };
            if ready.send(Ok(())).is_err() {
                return;
            }
            while !callbacks.closed() {
                if !route.changed.load(Ordering::Acquire) {
                    thread::park();
                    continue;
                }
                route.reopening.store(true, Ordering::Release);
                drop(stream);
                // Keep the engine and its pull target across an endpoint change. Drop the old
                // stream before clearing its notification and starting a replacement.
                thread::park_timeout(Duration::from_millis(25));
                loop {
                    if callbacks.closed() {
                        return;
                    }
                    route.changed.store(false, Ordering::Release);
                    callbacks.failed.store(false, Ordering::Release);
                    if let Ok(next) = open(&callbacks) {
                        stream = next;
                        route.unavailable.store(false, Ordering::Release);
                        route.reopening.store(false, Ordering::Release);
                        break;
                    }
                    route.unavailable.store(true, Ordering::Release);
                    thread::park_timeout(Duration::from_millis(250));
                }
            }
        })
        .map_err(|error| (6, format!("Cannot start the output route worker: {error}")))?;
    match setup.recv() {
        Ok(Ok(())) => Ok(worker),
        result => {
            let _ = worker.join();
            Err(match result {
                Ok(Err(error)) => error,
                _ => (
                    6,
                    "Output route worker stopped during initialization".into(),
                ),
            })
        }
    }
}

fn driver_error(error: cpal::Error) -> Failure {
    let code = if error.kind() == ErrorKind::UnsupportedConfig {
        code::UNSUPPORTED
    } else {
        6
    };
    (code, format!("CPAL output: {error}"))
}

fn timer(config: StreamConfig, callbacks: Arc<Callbacks>) -> Result<JoinHandle<()>, Failure> {
    let frames = (config.sample_rate / 100).clamp(1, 2048);
    let period = Duration::from_secs_f64(f64::from(frames) / f64::from(config.sample_rate));
    let mut samples = vec![0.0; frames as usize * callbacks.channels];
    thread::Builder::new()
        .name("mradm-null-output".into())
        .spawn(move || {
            let mut next = Instant::now();
            while !callbacks.closed() {
                let now = Instant::now();
                if now < next {
                    thread::park_timeout(next - now);
                } else {
                    callbacks.fill(&mut samples, true);
                    next = (next + period).max(now);
                }
            }
        })
        .map_err(|error| (6, format!("Cannot start the null output clock: {error}")))
}

enum Driver {
    Hardware(cpal::Stream),
    #[cfg(windows)]
    DefaultOutput(JoinHandle<()>),
    Null(JoinHandle<()>),
}

fn start_hardware(
    token: &[u8],
    config: StreamConfig,
    callbacks: &Arc<Callbacks>,
) -> Result<(Driver, bool), Failure> {
    let (device, default) = output_device(&host().map_err(driver_error)?, token, config.channels)
        .ok_or_else(|| (6, "No audio output device is available".into()))?;
    let using_fallback = default && !token.is_empty();
    #[cfg(windows)]
    if default {
        let worker = follow_default_output(Arc::clone(callbacks), move |callbacks| {
            let device = host()
                .map_err(driver_error)?
                .default_output_device()
                .ok_or_else(|| (6, "No audio output device is available".into()))?;
            let endpoint = device.id().map_err(driver_error)?;
            let stream = play_hardware(&device, config, callbacks)?;
            // Cover a route change that happened before CPAL installed its listener.
            if device.id().as_ref().ok() != Some(&endpoint) {
                callbacks.event(ErrorKind::StreamInvalidated);
            }
            Ok(stream)
        })?;
        return Ok((Driver::DefaultOutput(worker), using_fallback));
    }
    Ok((
        Driver::Hardware(play_hardware(&device, config, callbacks)?),
        using_fallback,
    ))
}

fn join(worker: JoinHandle<()>) {
    worker.thread().unpark();
    let _ = worker.join();
}

struct Running {
    callbacks: Arc<Callbacks>,
    driver: Driver,
    using_fallback: bool,
}

struct PcmDevice {
    token: Vec<u8>,
    null_backend: bool,
    running: Mutex<Option<Running>>,
}

impl PcmDevice {
    fn start(&self, channels: u32, rate: u32, pull: DevicePull) -> Result<(), Failure> {
        let mut slot = self
            .running
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if slot.is_some() {
            return Err((
                code::INVALID_ARGUMENT,
                "audio device already started".into(),
            ));
        }
        if !(1..=254).contains(&channels) || rate == 0 {
            return Err((code::INVALID_ARGUMENT, "invalid audio output format".into()));
        }
        let config = StreamConfig {
            channels: channels as u16,
            sample_rate: rate,
            buffer_size: BufferSize::Default,
        };
        let callbacks = Arc::new(Callbacks::new(pull, config.channels));
        let started = if self.null_backend {
            timer(config, Arc::clone(&callbacks)).map(|worker| (Driver::Null(worker), false))
        } else {
            start_hardware(&self.token, config, &callbacks)
        };
        match started {
            Ok((driver, using_fallback)) => {
                *slot = Some(Running {
                    callbacks,
                    driver,
                    using_fallback,
                });
                Ok(())
            }
            Err(error) => {
                callbacks.close();
                callbacks.drain();
                Err(error)
            }
        }
    }

    fn stop(&self) {
        let mut slot = self
            .running
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if let Some(Running {
            callbacks, driver, ..
        }) = slot.take()
        {
            callbacks.close();
            match driver {
                Driver::Hardware(stream) => drop(stream),
                #[cfg(windows)]
                Driver::DefaultOutput(worker) => join(worker),
                Driver::Null(worker) => join(worker),
            }
            callbacks.drain();
        }
    }
}

unsafe extern "C" fn start(
    ctx: *mut c_void,
    channels: u32,
    rate: u32,
    pull: *const DevicePull,
    error: *const EngineIslandError,
) -> i32 {
    let device = unsafe { &*ctx.cast::<PcmDevice>() };
    match device.start(channels, rate, unsafe { *pull }) {
        Ok(()) => 0,
        Err((code, detail)) => unsafe { island_error(error, code, &detail) },
    }
}

unsafe extern "C" fn stop(ctx: *mut c_void) {
    unsafe { &*ctx.cast::<PcmDevice>() }.stop();
}

unsafe extern "C" fn destroy(ctx: *mut c_void) {
    unsafe { Box::from_raw(ctx.cast::<PcmDevice>()) }.stop();
}

unsafe extern "C" fn realtime(_: *mut c_void) -> u32 {
    1
}

unsafe extern "C" fn progress(ctx: *mut c_void, out: *mut EngineSceneDeviceProgress) {
    let device = unsafe { &*ctx.cast::<PcmDevice>() };
    let slot = device
        .running
        .lock()
        .unwrap_or_else(|error| error.into_inner());
    let (failed, recovering) = slot
        .as_ref()
        .map_or((false, false), |run| run.callbacks.health());
    unsafe {
        *out = EngineSceneDeviceProgress {
            presented_frames: 0,
            media_clock: 0,
            failed: u32::from(failed),
            recovering: u32::from(recovering),
            using_fallback: u32::from(slot.as_ref().is_some_and(|run| run.using_fallback)),
            underruns: slot
                .as_ref()
                .map_or(0, |run| run.callbacks.xruns.load(Ordering::Relaxed)),
        };
    }
}

pub(crate) fn open(null_backend: bool, token: &[u8]) -> DeviceTable {
    let device = PcmDevice {
        null_backend,
        token: token.to_vec(),
        running: Mutex::new(None),
    };
    DeviceTable {
        ctx: Box::into_raw(Box::new(device)).cast(),
        start: Some(start),
        stop: Some(stop),
        progress: Some(progress),
        realtime: Some(realtime),
        destroy: Some(destroy),
        ..DeviceTable::empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn integer_devices_saturate_and_pad_with_their_own_silence_value() {
        assert_eq!(integer_pcm::<cpal::I24>(1.0).inner(), 8_388_607);
        assert_eq!(integer_pcm::<cpal::I24>(-2.0).inner(), -8_388_608);
        assert_eq!(integer_pcm::<cpal::U24>(2.0).inner(), 16_777_215);
        assert_eq!(integer_pcm::<u16>(0.0), 32_768);
    }

    fn hex(bytes: &[u8]) -> Vec<u8> {
        use std::fmt::Write;
        let mut text = String::new();
        for &byte in bytes.iter().chain(std::iter::repeat(&0)).take(256) {
            write!(text, "{byte:02x}").unwrap();
        }
        text.into_bytes()
    }

    #[test]
    fn saved_device_ids_preserve_native_endpoint_names() {
        let name = "音频 UID";
        assert_eq!(
            legacy_id(&hex(name.as_bytes()), false).as_deref(),
            Some(name)
        );
        let id = "{0.0.0.00000000}.{27928c85-0515-48d4-a84c-3efa72a20dfb}";
        let wide: Vec<_> = id.encode_utf16().flat_map(u16::to_le_bytes).collect();
        assert_eq!(legacy_id(&hex(&wide), true).as_deref(), Some(id));
        for invalid in [b"".as_slice(), b"001", &[b'z'; 512], &[b'0'; 512]] {
            assert!(legacy_id(invalid, false).is_none());
        }
    }

    unsafe extern "C" fn short(
        user: *mut c_void,
        out: *mut f32,
        len: usize,
        frames: usize,
        _: u32,
    ) -> usize {
        unsafe { &*user.cast::<AtomicUsize>() }.fetch_add(1, Ordering::Relaxed);
        let count = frames / 2;
        let buffer = unsafe { std::slice::from_raw_parts_mut(out, len) };
        buffer[..count * 2].fill(0.5);
        count
    }

    #[test]
    fn callback_pads_short_reads_and_distinguishes_device_events() {
        let calls = AtomicUsize::new(0);
        let state = Callbacks::new(
            DevicePull {
                user: (&raw const calls).cast_mut().cast(),
                pull: Some(short),
            },
            2,
        );
        let mut buffer = [1.0; 8];
        state.event(ErrorKind::DeviceChanged);
        state.event(ErrorKind::Xrun);
        crate::test_alloc::COUNT.with(|count| count.set(Some(0)));
        state.fill(&mut buffer, true);
        assert_eq!(crate::test_alloc::COUNT.with(|count| count.take()), Some(0));
        assert_eq!(buffer, [0.5, 0.5, 0.5, 0.5, 0., 0., 0., 0.]);
        assert_eq!(state.xruns.load(Ordering::Relaxed), 1);
        state.event(ErrorKind::DeviceNotAvailable);
        state.fill(&mut buffer, true);
        assert_eq!(buffer, [0.; 8]);
        assert_eq!(calls.load(Ordering::Relaxed), 1);
    }

    #[cfg(windows)]
    #[test]
    fn default_route_retries_until_the_replacement_is_available() {
        let calls = AtomicUsize::new(0);
        let callbacks = Arc::new(Callbacks::new(
            DevicePull {
                user: (&raw const calls).cast_mut().cast(),
                pull: Some(short),
            },
            2,
        ));
        let available = Arc::new(AtomicBool::new(true));
        let device_available = Arc::clone(&available);
        let worker = follow_default_output(Arc::clone(&callbacks), move |_| {
            if device_available.load(Ordering::Acquire) {
                Ok(())
            } else {
                Err((6, "no default output".into()))
            }
        })
        .unwrap();
        let wait_for_health = |expected| {
            let deadline = Instant::now() + Duration::from_secs(4);
            while callbacks.health() != expected && Instant::now() < deadline {
                thread::sleep(Duration::from_millis(5));
            }
            callbacks.health() == expected
        };
        available.store(false, Ordering::Release);
        callbacks.event(ErrorKind::StreamInvalidated);
        let lost = wait_for_health((true, true));
        available.store(true, Ordering::Release);
        let recovered = wait_for_health((false, false));
        callbacks.fill(&mut [0.0; 4], true);
        callbacks.close();
        join(worker);
        assert!(lost && recovered);
        assert_eq!(calls.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn stopping_null_output_joins_its_callback_and_permits_restart() {
        let calls = AtomicUsize::new(0);
        let pull = DevicePull {
            user: (&raw const calls).cast_mut().cast(),
            pull: Some(short),
        };
        let device = PcmDevice {
            token: Vec::new(),
            null_backend: true,
            running: Mutex::default(),
        };
        for _ in 0..2 {
            let before = calls.load(Ordering::Relaxed);
            device.start(2, 48_000, pull).unwrap();
            assert_eq!(
                device.start(2, 48_000, pull).unwrap_err().0,
                code::INVALID_ARGUMENT
            );
            let deadline = Instant::now() + Duration::from_secs(3);
            while calls.load(Ordering::Relaxed) == before {
                assert!(Instant::now() < deadline);
                thread::sleep(Duration::from_millis(1));
            }
            device.stop();
            let stopped = calls.load(Ordering::Relaxed);
            thread::sleep(Duration::from_millis(25));
            assert_eq!(calls.load(Ordering::Relaxed), stopped);
        }
    }

    #[test]
    fn closing_with_an_inflight_pull_prevents_late_access_to_the_target() {
        unsafe extern "C" fn blocked(
            user: *mut c_void,
            _: *mut f32,
            _: usize,
            frames: usize,
            _: u32,
        ) -> usize {
            let events = unsafe { &*user.cast::<(std::sync::Barrier, std::sync::Barrier)>() };
            events.0.wait();
            events.1.wait();
            frames
        }
        let events = (std::sync::Barrier::new(2), std::sync::Barrier::new(2));
        let state = Callbacks::new(
            DevicePull {
                user: std::ptr::from_ref(&events).cast_mut().cast(),
                pull: Some(blocked),
            },
            2,
        );
        thread::scope(|scope| {
            scope.spawn(|| state.fill(&mut [0.0; 4], true));
            events.0.wait();
            state.close();
            assert_eq!(state.gate.load(Ordering::Acquire), CLOSED | 1);
            let mut late = [1.0; 4];
            state.fill(&mut late, true);
            assert_eq!(late, [0.0; 4]);
            events.1.wait();
            state.drain();
        });
        assert_eq!(state.gate.load(Ordering::Acquire), CLOSED);
    }
}
