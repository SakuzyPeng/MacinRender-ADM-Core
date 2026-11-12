//! The monitor session (formerly the C++ `MonitorSession`): the session's
//! log buffer, the stream opening of the C++ `RealtimeStreamFactory` (the rules are
//! [`stream::open_monitor`]), the last-used and pending backend options, the edits, orientation
//! and HpTF profile that survive a rebuild, and the backend-switch and output-device transactions
//! with their fallbacks. The device (miniaudio, the Apple and Windows system-spatial sinks) and the
//! Apple stream stay with the caller behind [`SessionHost`]. Rules, order and texts follow the C++
//! session.
use super::{
    CreateError, Levels, Monitor, MonitorHost, Native, Overrides, Receipt, Source, State, Status,
    code, level,
};
use crate::backend;
use crate::fmt;
use crate::render;
use crate::stream::{self, Monitor as Opened};
use mradm_dsp::hptf::StereoSnapshot;
use mradm_render::room_runtime::Control as RoomControl;
use mradm_render::routing::Error;
use mradm_scene::{RenderScene, direct_matrix};
use std::any::Any;
use std::sync::{Arc, Mutex, MutexGuard};

/// The code of an error the C++ session raised as an exception (a failed `MonitorEngine`
/// constructor or HpTF publication): the shell rethrows its message.
pub const THROWN: i32 = 100;

const INVALID_SESSION: &str = "监听会话无效:后端重建失败";

/// The caller's `RenderOptions` of a [`Choice`], for the device and the Apple island.
pub type Token = Arc<dyn Any + Send + Sync>;

/// One backend configuration: the request the Rust opening reads and the caller's options.
#[derive(Clone)]
pub struct Choice {
    pub request: render::Request,
    pub system_spatial: bool,
    pub token: Token,
}

/// An output device (`IAudioOutputDevice`).
pub trait Device: Send + Sync {
    /// `pull_is_realtime_playback`.
    fn realtime(&self) -> bool;
    /// Starts pulling `monitor`. The session stops the device before the monitor drops.
    fn start(&self, channels: usize, sample_rate: u32, monitor: &Monitor) -> Result<(), Error>;
    fn stop(&self);
    fn flush(&self);
    fn pause(&self);
    fn resume(&self);
}

/// A prepared HpTF profile (`HptfSettings`) designed for each engine's rate.
pub trait HptfDesign: Send + Sync {
    fn design(&self, sample_rate: u32) -> Result<StereoSnapshot, Error>;
}

/// What the session needs from its caller.
pub trait SessionHost: backend::Host + Send + Sync {
    /// The session's policy-applied scene.
    fn scene(&self) -> Option<&RenderScene>;
    /// Whether the Apple backend is available.
    fn apple(&self) -> bool;
    /// `make_monitor_device` for the options of `choice` on `device_id` ("" = default).
    fn device(&self, choice: &Token, device_id: &[u8]) -> Arc<dyn Device>;
    /// The Apple island's stream; `log` appends a line (level, module, message) to the session's
    /// log buffer.
    fn open_apple(
        &self,
        plan: &AppleOpen<'_>,
        log: &dyn Fn(u32, &str, &[u8]),
    ) -> Result<Box<dyn Source>, Error>;
}

/// The Apple island's stream over the resolved layout and DirectSpeakers matrix. System-spatial
/// monitoring hands head tracking to the system, so the rendered bed stays orientation-neutral
/// (not rotated twice).
pub struct AppleOpen<'a> {
    /// The caller's options of the choice.
    pub token: &'a Token,
    pub input_path: &'a render::PathText,
    pub layout: &'a str,
    pub matrix: Option<&'a direct_matrix::Matrix>,
    pub options: render::AppleOptions,
}

impl<'a> AppleOpen<'a> {
    fn of(choice: &'a Choice, layout: &'a str, matrix: Option<&'a direct_matrix::Matrix>) -> Self {
        let mut options = render::AppleOptions::of(&choice.request);
        if choice.system_spatial {
            options.orientation = [0.; 3];
        }
        Self {
            token: &choice.token,
            input_path: &choice.request.input,
            layout,
            matrix,
            options,
        }
    }
}

/// One buffered log line: level, module, message.
pub type Entry = (u32, String, Vec<u8>);

/// The C++ `BufferingLogSink`: append-only, polled by the caller.
#[derive(Default)]
struct Logs(Mutex<Vec<Entry>>);

impl Logs {
    fn push(&self, level: u32, module: &str, message: impl Into<Vec<u8>>) {
        self.0.lock().unwrap_or_else(|e| e.into_inner()).push((
            level,
            module.into(),
            message.into(),
        ));
    }
}

/// The monitor's host for one engine: the session's log buffer and its device.
struct EngineHost {
    logs: Arc<Logs>,
    device: Arc<dyn Device>,
    host: Arc<dyn SessionHost>,
}

impl MonitorHost for EngineHost {
    fn log(&self, level: u32, module: &str, message: &[u8]) {
        self.logs.push(level, module, message);
    }
    fn device_flush(&self) {
        self.device.flush();
    }
    fn device_pause(&self) {
        self.device.pause();
    }
    fn device_resume(&self) {
        self.device.resume();
    }
    fn room_worker(&self, active: bool) {
        self.host.room_worker(active);
    }
    fn room_wait(&self, duration: std::time::Duration) {
        self.host.room_wait(duration);
    }
}

struct Inner {
    loop_start: u64,
    loop_end: u64,
    device: Option<Arc<dyn Device>>,
    monitor: Option<Box<Monitor>>,
    current: Option<Choice>,
    pending: Vec<(Arc<Receipt>, Choice)>,
    overrides: Overrides,
    orientation: [f32; 3],
    hptf: Option<Arc<dyn HptfDesign>>,
    device_id: Vec<u8>,
}

impl Inner {
    fn refresh(&mut self) {
        let current = &mut self.current;
        self.pending.retain(|(receipt, choice)| {
            let state = receipt.state();
            if state == Receipt::APPLIED {
                *current = Some(choice.clone());
            }
            state == Receipt::PENDING
        });
    }

    /// Under system-spatial monitoring the system owns head tracking: the stream stays neutral.
    fn effective_orientation(&self) -> [f32; 3] {
        if self.current.as_ref().is_some_and(|c| c.system_spatial) {
            [0.; 3]
        } else {
            self.orientation
        }
    }

    /// The engine destructor: cancel, stop the device, join.
    fn stop_engine(&mut self) {
        if let Some(monitor) = self.monitor.take() {
            monitor.begin_shutdown();
            if let Some(device) = &self.device {
                device.stop();
            }
            drop(monitor);
        }
    }
}

/// The session.
pub struct Session {
    logs: Arc<Logs>,
    host: Arc<dyn SessionHost>,
    sample_rate: u32,
    total_frames: u64,
    inner: Mutex<Inner>,
}

fn error(code: i32, message: &str) -> Error {
    Error {
        code,
        message: message.as_bytes().to_vec(),
        context: Vec::new(),
    }
}

fn thrown(e: CreateError) -> Error {
    match e {
        CreateError::Error(e) => e,
        CreateError::Construction(message) => error(THROWN, &message),
    }
}

fn text(e: &Error) -> String {
    String::from_utf8_lossy(&e.message).into_owned()
}

/// `std::to_string(float)`: `%f` of the value widened to double.
fn to_string(value: f32) -> String {
    fmt::fixed(f64::from(value), 6, false)
}

impl Session {
    /// A session without an engine yet; `sample_rate` and `total_frames` are the scene's.
    pub fn new(sample_rate: u32, total_frames: u64, host: Arc<dyn SessionHost>) -> Self {
        Self {
            logs: Arc::default(),
            host,
            sample_rate,
            total_frames,
            inner: Mutex::new(Inner {
                loop_start: 0,
                loop_end: 0,
                device: None,
                monitor: None,
                current: None,
                pending: Vec::new(),
                overrides: Overrides::default(),
                orientation: [0.; 3],
                hptf: None,
                device_id: Vec::new(),
            }),
        }
    }

    /// Appends a line to the session's log buffer (the import's and the Apple island's lines).
    pub fn log(&self, level: u32, module: &str, message: &[u8]) {
        self.logs.push(level, module, message);
    }

    /// `MonitorSession::create` after the import: opens the stream on a device for `device_id`
    /// and starts it.
    pub fn start(&self, choice: Choice, device_id: &[u8]) -> Result<(), Error> {
        let mut inner = self.lock();
        inner.current = Some(choice.clone());
        inner.device_id = device_id.to_vec();
        let device = self.host.device(&choice.token, device_id);
        inner.device = Some(device.clone());
        inner.monitor = Some(self.engine(&choice, device, None)?);
        Ok(())
    }

    /// [`Self::new`] and [`Self::start`].
    pub fn create(
        choice: Choice,
        sample_rate: u32,
        total_frames: u64,
        device_id: &[u8],
        host: Arc<dyn SessionHost>,
    ) -> Result<Self, Error> {
        let session = Self::new(sample_rate, total_frames, host);
        session.start(choice, device_id)?;
        Ok(session)
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// `RealtimeStreamFactory::open`.
    fn open(&self, choice: &Choice) -> Result<Box<dyn Source>, Error> {
        let opened = stream::open_monitor(
            &choice.request,
            choice.system_spatial,
            self.host.scene(),
            self.host.apple(),
            &*self.host,
        );
        for line in opened.logs {
            self.logs.push(line.level as u32, line.module, line.message);
        }
        match opened.result {
            Ok(Opened::Stream {
                validator,
                body,
                info,
                ..
            }) => Ok(Box::new(Native::new(validator, body, info))),
            Ok(Opened::Apple { layout, matrix }) => self.host.open_apple(
                &AppleOpen::of(choice, &layout, matrix.as_ref()),
                &|level, module, message| self.logs.push(level, module, message),
            ),
            Err(e) => Err(Error {
                code: if (1..=6).contains(&e.code) {
                    e.code
                } else {
                    code::INTERNAL_ERROR
                },
                message: e.message.into_bytes(),
                context: e.context.into_bytes(),
            }),
        }
    }

    /// `MonitorEngine::create`; `channels` is the `MonitorOutputFactory` check of a rebuild.
    fn engine(
        &self,
        choice: &Choice,
        device: Arc<dyn Device>,
        channels: Option<usize>,
    ) -> Result<Box<Monitor>, Error> {
        let source = self.open(choice)?;
        if let Some(channels) = channels
            && source.out_channels() != channels
        {
            drop(source);
            return Err(error(
                code::UNSUPPORTED,
                "monitor cannot change its output channel count",
            ));
        }
        let host = Arc::new(EngineHost {
            logs: self.logs.clone(),
            device: device.clone(),
            host: self.host.clone(),
        });
        let monitor = Box::new(Monitor::create(source, device.realtime(), host).map_err(thrown)?);
        if let Err(e) = device.start(monitor.channels(), monitor.sample_rate(), &monitor) {
            monitor.begin_shutdown();
            drop(monitor);
            return Err(e);
        }
        Ok(monitor)
    }

    /// Seconds → absolute frame, clamped to [0, total frames]; NaN and negatives map to 0.
    fn clamp_frame(&self, seconds: f64) -> u64 {
        if seconds == f64::INFINITY {
            return self.total_frames;
        }
        mradm_dsp::timeline::seconds_to_frame(seconds, self.sample_rate)
            .unwrap_or(0)
            .min(self.total_frames)
    }

    pub fn play(&self) {
        if let Some(m) = &self.lock().monitor {
            m.play();
        }
    }

    pub fn pause(&self) {
        if let Some(m) = &self.lock().monitor {
            m.pause();
        }
    }

    pub fn seek_seconds(&self, seconds: f64) -> Result<(), Error> {
        let inner = self.lock();
        let Some(m) = &inner.monitor else {
            return Err(error(code::INTERNAL_ERROR, INVALID_SESSION));
        };
        m.seek(self.clamp_frame(seconds));
        Ok(())
    }

    pub fn set_loop_seconds(&self, start: f64, end: f64) {
        let mut inner = self.lock();
        if inner.monitor.is_none() {
            return;
        }
        inner.loop_start = self.clamp_frame(start);
        inner.loop_end = self.clamp_frame(end);
        let (start, end) = (inner.loop_start, inner.loop_end);
        if let Some(m) = &inner.monitor {
            m.set_loop(start, end);
        }
    }

    pub fn set_overrides(&self, overrides: Overrides) -> Result<(), Error> {
        let mut inner = self.lock();
        let Some(m) = &inner.monitor else {
            return Err(error(
                code::INTERNAL_ERROR,
                "monitor session is unavailable",
            ));
        };
        m.set_overrides(overrides.clone())?;
        inner.overrides = overrides;
        Ok(())
    }

    pub fn room_control(&self) -> Option<RoomControl> {
        self.lock().monitor.as_ref().and_then(|m| m.room_control())
    }

    pub fn set_orientation(&self, pose: [f32; 3]) {
        let mut inner = self.lock();
        inner.refresh();
        inner.orientation = pose;
        let effective = inner.effective_orientation();
        if let Some(m) = &inner.monitor {
            m.set_orientation(effective);
        }
    }

    /// `set_hptf_prepared`.
    pub fn set_hptf(&self, design: Arc<dyn HptfDesign>) -> Result<(), Error> {
        let mut inner = self.lock();
        let Some(m) = &inner.monitor else {
            return Err(error(code::INTERNAL_ERROR, INVALID_SESSION));
        };
        if m.channels() != 2 {
            return Err(error(
                code::UNSUPPORTED,
                "耳机补偿(HpTF)只适用于双声道耳机监听",
            ));
        }
        let snapshot = design.design(m.sample_rate())?;
        let left = snapshot.coefficients[0].max_response_db;
        let right = snapshot.coefficients[usize::from(snapshot.independent)].max_response_db;
        // std::max keeps its first argument unless it is less (NaN included).
        if (if left < right { right } else { left }) > 0. {
            self.logs.push(
                level::WARNING,
                "monitor",
                format!(
                    "耳机补偿曲线可能削波(左 {} dB,右 {} dB)",
                    to_string(left),
                    to_string(right)
                ),
            );
        }
        let result = m.hptf().publish(snapshot).map_err(|e| publication(&e));
        inner.hptf = Some(design);
        result
    }

    /// The HpTF target the callback has taken up; None without an engine.
    pub fn hptf_snapshot(&self) -> Option<StereoSnapshot> {
        self.lock()
            .monitor
            .as_ref()
            .map(|m| m.hptf().active_snapshot())
    }

    /// `republish_hptf`: the stored profile designed for the current engine's rate.
    fn republish_hptf(&self, inner: &Inner) -> Result<(), Error> {
        let Some(m) = &inner.monitor else {
            return Ok(());
        };
        if m.channels() != 2 {
            return Ok(());
        }
        let Some(design) = &inner.hptf else {
            return m.hptf().publish_bypass(0).map_err(|e| publication(&e));
        };
        match design.design(m.sample_rate()) {
            Ok(snapshot) => m.hptf().publish(snapshot).map_err(|e| publication(&e)),
            Err(e) => {
                self.logs.push(
                    level::WARNING,
                    "monitor",
                    format!("耳机补偿重建失败:{}", text(&e)),
                );
                Ok(())
            }
        }
    }

    /// Re-applies the edits, loop, playhead, orientation, HpTF and play state on a rebuilt engine.
    fn restore(&self, inner: &Inner, playhead: u64, playing: bool) -> Result<(), Error> {
        let Some(m) = &inner.monitor else {
            return Ok(());
        };
        let _ = m.set_overrides(inner.overrides.clone());
        m.set_loop(inner.loop_start, inner.loop_end);
        m.seek(playhead);
        m.set_orientation(inner.effective_orientation());
        self.republish_hptf(inner)?;
        if playing {
            m.play();
        }
        Ok(())
    }

    pub fn switch_backend(&self, choice: Choice) -> Result<(), Error> {
        let mut inner = self.lock();
        inner.refresh();
        let Some(m) = &inner.monitor else {
            return Err(error(code::INTERNAL_ERROR, INVALID_SESSION));
        };
        let stream = self.open(&choice)?;
        stream.validator().validate(&inner.overrides)?;
        let channels = m.channels();
        if stream.out_channels() != channels {
            return Err(error(
                code::UNSUPPORTED,
                "monitor backend switches cannot change the output channel count; use binaural output for headphones",
            ));
        }
        if stream.sample_rate() != m.sample_rate() {
            return Err(error(
                code::UNSUPPORTED,
                "monitor backend switch requires the same sample rate",
            ));
        }
        if stream.renders_orientation_at_output() != m.output_stage() {
            // The ring layout is fixed per engine: rebuild it on the same device.
            drop(stream);
            let status = m.status();
            let (playhead, playing) = (status.playhead_frames, status.state == State::Playing);
            inner.stop_engine();
            inner.refresh();
            let Some(previous) = inner.current.clone() else {
                return Err(error(code::INTERNAL_ERROR, INVALID_SESSION));
            };
            inner.device = None;
            let mut switch_error = None;
            if let Err(e) = self.rebuild(&mut inner, &choice, channels) {
                if e.code == THROWN {
                    return Err(e);
                }
                self.logs.push(
                    level::WARNING,
                    "monitor",
                    format!("切换后端失败,回退到原后端:{}", text(&e)),
                );
                if let Err(restored) = self.rebuild(&mut inner, &previous, channels) {
                    return Err(if restored.code == THROWN { restored } else { e });
                }
                switch_error = Some(e);
            }
            self.restore(&inner, playhead, playing)?;
            return switch_error.map_or(Ok(()), Err);
        }
        let receipt = Arc::new(Receipt::default());
        inner.pending.push((receipt.clone(), choice));
        if let Some(m) = &inner.monitor {
            m.switch(stream, Some(receipt));
        }
        Ok(())
    }

    /// A fresh device and engine for `choice`; on failure both are dropped (an exception keeps the
    /// device, as the C++ unwinding did).
    fn rebuild(&self, inner: &mut Inner, choice: &Choice, channels: usize) -> Result<(), Error> {
        inner.current = Some(choice.clone());
        let device = self.host.device(&choice.token, &inner.device_id);
        inner.device = Some(device.clone());
        match self.engine(choice, device, Some(channels)) {
            Ok(monitor) => {
                inner.monitor = Some(monitor);
                Ok(())
            }
            Err(e) => {
                if e.code != THROWN {
                    inner.device = None;
                }
                Err(e)
            }
        }
    }

    pub fn set_output_device(&self, device_id: &[u8]) -> Result<(), Error> {
        let mut inner = self.lock();
        let Some(m) = &inner.monitor else {
            return Err(error(code::INTERNAL_ERROR, INVALID_SESSION));
        };
        if device_id == inner.device_id.as_slice() {
            return Ok(());
        }
        let status = m.status();
        let (playhead, playing) = (status.playhead_frames, status.state == State::Playing);
        let channels = m.channels();
        inner.stop_engine();
        inner.refresh();
        inner.device = None;
        if let Err(e) = self.open_on(&mut inner, device_id, channels) {
            if e.code == THROWN {
                return Err(e);
            }
            self.logs.push(
                level::WARNING,
                "monitor",
                format!("切换输出设备失败,回落到默认设备:{}", text(&e)),
            );
            self.open_on(&mut inner, b"", channels)?;
        }
        self.restore(&inner, playhead, playing)
    }

    fn open_on(&self, inner: &mut Inner, device_id: &[u8], channels: usize) -> Result<(), Error> {
        let Some(choice) = inner.current.clone() else {
            return Err(error(code::INTERNAL_ERROR, INVALID_SESSION));
        };
        let device = self.host.device(&choice.token, device_id);
        inner.device = Some(device.clone());
        match self.engine(&choice, device, Some(channels)) {
            Ok(monitor) => {
                inner.monitor = Some(monitor);
                inner.device_id = device_id.to_vec();
                Ok(())
            }
            Err(e) => {
                if e.code != THROWN {
                    inner.device = None;
                }
                Err(e)
            }
        }
    }

    /// None for an invalidated session (the caller reports stopped and failed).
    pub fn status(&self) -> Option<Status> {
        let mut inner = self.lock();
        inner.refresh();
        inner.monitor.as_ref().map(|m| m.status())
    }

    /// None for an invalidated session (silent levels).
    pub fn levels(&self) -> Option<Levels> {
        self.lock().monitor.as_ref().map(|m| m.levels())
    }

    pub fn log_count(&self) -> usize {
        self.logs.0.lock().unwrap_or_else(|e| e.into_inner()).len()
    }

    pub fn log_entry(&self, index: usize) -> Option<Entry> {
        self.logs
            .0
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .get(index)
            .cloned()
    }
}

/// `dsp::check`: a failed HpTF publication threw from the C++ session.
fn publication(e: &mradm_dsp::Error) -> Error {
    error(THROWN, &format!("Rust DSP: {e}"))
}

impl Drop for Session {
    fn drop(&mut self) {
        let inner = self.inner.get_mut().unwrap_or_else(|e| e.into_inner());
        inner.stop_engine();
        inner.device = None;
    }
}
