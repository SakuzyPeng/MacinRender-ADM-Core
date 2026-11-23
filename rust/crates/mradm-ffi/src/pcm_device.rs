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
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

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

fn output_device(host: &cpal::Host, token: &[u8], channels: u16) -> Option<cpal::Device> {
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
        return Some(device);
    }
    let default = host.default_output_device()?;
    if channels > 2 {
        host.device_by_id(&default.id().ok()?)
    } else {
        Some(default)
    }
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
        }
    }

    fn fill(&self, buffer: &mut [f32]) {
        if buffer.is_empty() {
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
            pull(self.pull.user, buffer.as_mut_ptr(), buffer.len(), frames)
        });
        buffer[produced.min(frames) * self.channels..].fill(0.0);
        self.gate.fetch_sub(1, Ordering::Release);
    }

    fn event(&self, kind: ErrorKind) {
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
            for chunk in output.chunks_mut(scratch.len()) {
                let samples = &mut scratch[..chunk.len()];
                callbacks.fill(samples);
                for (dest, &sample) in chunk.iter_mut().zip(samples.iter()) {
                    *dest = convert(sample);
                }
            }
        },
        move |error| events.event(error.kind()),
        IO_TIMEOUT,
    )
}

fn hardware(
    token: &[u8],
    config: StreamConfig,
    callbacks: &Arc<Callbacks>,
) -> Result<cpal::Stream, Failure> {
    let device = output_device(&host().map_err(driver_error)?, token, config.channels)
        .ok_or_else(|| (6, "No audio output device is available".into()))?;
    let mix = device.default_output_config().map_err(driver_error)?;
    if config.channels > 2 && config.channels > mix.channels() {
        #[cfg(any(
            target_os = "linux",
            target_os = "freebsd",
            target_os = "netbsd",
            target_os = "dragonfly"
        ))]
        let supported = device
            .supported_output_configs()
            .map_err(driver_error)?
            .any(|range| {
                range.channels() == config.channels
                    && !range.sample_format().is_dsd()
                    && (range.min_sample_rate()..=range.max_sample_rate())
                        .contains(&config.sample_rate)
            });
        #[cfg(not(any(
            target_os = "linux",
            target_os = "freebsd",
            target_os = "netbsd",
            target_os = "dragonfly"
        )))]
        let supported = false;
        if !supported {
            return Err((code::UNSUPPORTED, MULTICHANNEL.into()));
        }
    }
    let pull = Arc::clone(callbacks);
    let events = Arc::clone(callbacks);
    let result = device.build_output_stream(
        config,
        move |out: &mut [f32], _| pull.fill(out),
        move |error| events.event(error.kind()),
        IO_TIMEOUT,
    );
    let stream = match result {
        Err(error) if error.kind() == ErrorKind::UnsupportedConfig => {
            macro_rules! converted {
                ($($variant:ident => $sample:ty),+) => {
                    match mix.sample_format() {
                        $(SampleFormat::$variant => converted_stream(&device, config, Arc::clone(callbacks), integer_pcm::<$sample>),)+
                        SampleFormat::F64 => converted_stream(&device, config, Arc::clone(callbacks), f64::from),
                        _ => Err(error),
                    }
                }
            }
            converted!(I8 => i8, I16 => i16, I24 => cpal::I24, I32 => i32, I64 => i64,
                U8 => u8, U16 => u16, U24 => cpal::U24, U32 => u32, U64 => u64)
        }
        result => result,
    }.map_err(driver_error)?;
    if let Err(error) = stream.play() {
        callbacks.close();
        drop(stream);
        callbacks.drain();
        return Err(driver_error(error));
    }
    Ok(stream)
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
            while callbacks.gate.load(Ordering::Acquire) & CLOSED == 0 {
                let now = Instant::now();
                if now < next {
                    thread::park_timeout(next - now);
                } else {
                    callbacks.fill(&mut samples);
                    next = (next + period).max(now);
                }
            }
        })
        .map_err(|error| (6, format!("Cannot start the null output clock: {error}")))
}

enum Driver {
    Hardware(cpal::Stream),
    Null(JoinHandle<()>),
}

struct Running {
    callbacks: Arc<Callbacks>,
    driver: Driver,
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
        let driver = if self.null_backend {
            Driver::Null(timer(config, Arc::clone(&callbacks))?)
        } else {
            Driver::Hardware(hardware(&self.token, config, &callbacks)?)
        };
        *slot = Some(Running { callbacks, driver });
        Ok(())
    }

    fn stop(&self) {
        let mut slot = self
            .running
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        if let Some(Running { callbacks, driver }) = slot.take() {
            callbacks.close();
            match driver {
                Driver::Hardware(stream) => drop(stream),
                Driver::Null(worker) => {
                    worker.thread().unpark();
                    let _ = worker.join();
                }
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
    let (failed, xruns) = slot.as_ref().map_or((false, 0), |run| {
        (
            run.callbacks.failed.load(Ordering::Acquire),
            run.callbacks.xruns.load(Ordering::Relaxed),
        )
    });
    unsafe {
        *out = EngineSceneDeviceProgress {
            presented_frames: 0,
            media_clock: 0,
            failed: u32::from(failed),
            recovering: 0,
            underruns: xruns,
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
        state.fill(&mut buffer);
        assert_eq!(crate::test_alloc::COUNT.with(|count| count.take()), Some(0));
        assert_eq!(buffer, [0.5, 0.5, 0.5, 0.5, 0., 0., 0., 0.]);
        assert_eq!(state.xruns.load(Ordering::Relaxed), 1);
        state.event(ErrorKind::DeviceNotAvailable);
        state.fill(&mut buffer);
        assert_eq!(buffer, [0.; 8]);
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
            scope.spawn(|| state.fill(&mut [0.0; 4]));
            events.0.wait();
            state.close();
            assert_eq!(state.gate.load(Ordering::Acquire), CLOSED | 1);
            let mut late = [1.0; 4];
            state.fill(&mut late);
            assert_eq!(late, [0.0; 4]);
            events.1.wait();
            state.drain();
        });
        assert_eq!(state.gate.load(Ordering::Acquire), CLOSED);
    }
}
