//! The offline render of a Rust backend preparation, moved from the C++
//! `render_window` loops with the same order, messages, locators, progress events, cancellation
//! points and logs: the loudspeaker driver (VBAP, HOA, EAR, Triple Balance; `render_speaker_pcm`),
//! the binaural driver and the room file stream with its meter.
//!
//! [`Run::next`] does one unit of work (an opening, a block, a seek step, the finish) and returns
//! what the caller emits; `cancelled` is sampled where the C++ loop checked its stop token.
use crate::backend::{Host, Kind, Prepared};
use crate::request::{Error, Level, Log, code};
use mradm_dsp::meter::{Meter, MeterMode};
use mradm_dsp::post_gain::amplitude_to_db;
use mradm_render::offline::{self, Driver, Location, Window};
use mradm_render::output::Writer;
use mradm_render::{binaural_render, room_stream};
use std::path::Path;
use std::sync::Arc;

/// The C++ `RenderStage` and `RenderOperation` values the drivers report.
pub mod stage {
    pub const RENDERING: u32 = 4;
    pub const FINISHED: u32 = 6;
}
pub mod operation {
    pub const RENDER_AUDIO: u32 = 7;
    pub const FINISH: u32 = 17;
}

const CANCELLED: i32 = 5;

/// One progress event of the render step (before the caller maps it into its range).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Progress {
    pub stage: u32,
    pub operation: u32,
    pub fraction: f64,
    pub stage_fraction: f64,
    pub current: u64,
    pub total: u64,
    pub message: &'static str,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Metrics {
    pub lufs: Option<f64>,
    pub peak_dbtp: Option<f64>,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Event {
    Progress(Progress),
    Log(Log),
    /// Work done, nothing to emit.
    Continue,
    Done(Metrics),
}

/// The `RenderPlan` fields of one offline render.
#[derive(Clone, Debug)]
pub struct Settings<'a> {
    /// The UTF-8 paths the files open with.
    pub input_path: &'a [u8],
    pub output_path: &'a [u8],
    /// The caller's text of the paths in locators and logs.
    pub input_display: &'a [u8],
    pub output_display: &'a [u8],
    pub output_layout: &'a [u8],
    pub inputs: usize,
    pub frames: u64,
    pub sample_rate: u32,
    pub default_interp_ms: u32,
    pub smoothing_frames: u32,
    pub window: Option<Window>,
    pub meter_window: Option<Window>,
    /// The listener orientation (yaw, pitch, roll).
    pub pose: [f32; 3],
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

/// A message as the C++ side read it from a 512-byte buffer: cut at a character boundary.
fn cut(message: &str, capacity: usize) -> String {
    let mut n = message.len().min(capacity - 1);
    while !message.is_char_boundary(n) {
        n -= 1;
    }
    message[..n].to_owned()
}

/// A driver or stream error with the locator its kind names (the C++ `DriverError`).
pub(crate) fn located(e: offline::Error, input: &str) -> Error {
    let context = match e.location {
        Location::None => String::new(),
        Location::Path => format!("path={input}"),
        Location::Input => format!("input={input}"),
        Location::Meter => "meter".to_owned(),
        Location::HoaDsp => "HOA DSP".to_owned(),
        Location::TripleBalanceDsp => "Triple Balance DSP".to_owned(),
        Location::Room => "room-binaural".to_owned(),
    };
    Error::new(e.code, cut(&e.message, 512), context)
}

fn wav_code(code: i32) -> i32 {
    if (1..=6).contains(&code) { code } else { 6 }
}

/// The private WAVE path checks (`crate::wav::path` of the FFI layer).
fn wav_path(bytes: &[u8]) -> Result<&str, (i32, &'static str)> {
    if bytes.is_empty() {
        return Err((1, "WAVE 路径为空"));
    }
    let path = std::str::from_utf8(bytes).map_err(|_| (1, "WAVE 路径必须使用 UTF-8"))?;
    if path.contains('\0') {
        return Err((1, "WAVE 路径包含 NUL"));
    }
    Ok(path)
}

/// The input path as the drivers opened it (`path=<display>` locator on failure).
fn input_path<'a>(bytes: &'a [u8], display: &str) -> Result<&'a str, Error> {
    wav_path(bytes).map_err(|(code, message)| Error::new(code, message, format!("path={display}")))
}

/// `std::filesystem::path::extension`, ASCII-lowercased.
fn extension(path: &str) -> String {
    let name = path
        .rsplit(['/', std::path::MAIN_SEPARATOR])
        .next()
        .unwrap_or("");
    if name == "." || name == ".." {
        return String::new();
    }
    match name.rfind('.') {
        Some(i) if i > 0 => name[i..].to_ascii_lowercase(),
        _ => String::new(),
    }
}

/// `audio::WriterHandle::open`: CAF with the layout's tag for `.caf`, RF64 WAVE otherwise.
fn open_writer(
    path: &[u8],
    display: &str,
    channels: u32,
    rate: u32,
    layout: &[u8],
) -> Result<Writer, Error> {
    let locate = |code: i32, message: &str| {
        Error::new(wav_code(code), cut(message, 512), format!("path={display}"))
    };
    let utf8 = wav_path(path).map_err(|(code, message)| locate(code, message))?;
    let ext = extension(utf8);
    if ext == ".flac" {
        return Err(Error::new(
            code::UNSUPPORTED,
            "offline rendering writes WAV or CAF; FLAC is encoded from a WAV render",
            format!("path={display}"),
        ));
    }
    let layout = (ext == ".caf").then_some(layout);
    Writer::create(Path::new(utf8), channels, rate, layout).map_err(|e| locate(e.code, &e.message))
}

fn finish_writer(writer: &mut Writer, display: &str) -> Result<(), Error> {
    writer.finish().map_err(|e| {
        Error::new(
            wav_code(e.code),
            cut(&e.message, 512),
            format!("path={display}"),
        )
    })
}

fn rendering(
    fraction: f64,
    stage_fraction: f64,
    current: u64,
    total: u64,
    message: &'static str,
) -> Event {
    Event::Progress(Progress {
        stage: stage::RENDERING,
        operation: operation::RENDER_AUDIO,
        fraction,
        stage_fraction,
        current,
        total,
        message,
    })
}

fn finished() -> Event {
    Event::Progress(Progress {
        stage: stage::FINISHED,
        operation: operation::FINISH,
        fraction: 1.,
        stage_fraction: 1.,
        current: 0,
        total: 0,
        message: "done",
    })
}

fn info(module: &'static str, message: String) -> Event {
    Event::Log(Log {
        level: Level::Info,
        module,
        message,
    })
}

/// The plan fields a run keeps.
struct Common {
    prepared: Arc<Prepared>,
    /// The display text of the input (locators).
    input: String,
    input_path: Vec<u8>,
    output: Vec<u8>,
    output_display: String,
    layout: Vec<u8>,
    inputs: usize,
    frames: u64,
    sample_rate: u32,
    default_interp_ms: u32,
    smoothing_frames: u32,
    window: Option<Window>,
    meter_window: Option<Window>,
    pose: [f32; 3],
}

impl Common {
    fn module(&self) -> &'static str {
        self.prepared.kind.module()
    }
    fn output_text(&self) -> String {
        self.output_display.clone()
    }
    fn output_context(&self) -> String {
        format!("output={}", self.output_text())
    }
    fn located(&self, e: offline::Error) -> Error {
        located(e, &self.input)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Style {
    Speaker,
    Hoa,
    Ear,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum SpeakerPhase {
    BeginLog,
    BeginProgress,
    Open,
    Blocks,
    Wrote,
    Done,
}

/// `render_speaker_pcm`: VBAP, HOA, EAR and Triple Balance.
struct Speaker {
    c: Common,
    style: Style,
    phase: SpeakerPhase,
    driver: Option<Driver>,
    writer: Option<Writer>,
    metrics: Metrics,
}

impl Speaker {
    fn wording(&self) -> (&'static str, &'static str, &'static str) {
        if self.style == Style::Hoa {
            ("encoding HOA", "encoding", "short write while encoding HOA")
        } else {
            (
                "rendering audio",
                "rendering",
                "short write while rendering",
            )
        }
    }

    fn next(&mut self, cancelled: bool) -> Result<Event, Error> {
        let module = self.c.module();
        let outputs = self.c.prepared.outputs;
        let (begin, step, short) = self.wording();
        match self.phase {
            SpeakerPhase::BeginLog => {
                self.phase = SpeakerPhase::BeginProgress;
                let tracks = self.c.prepared.bindings.len();
                let frames = self.c.frames;
                Ok(info(
                    module,
                    match self.style {
                        Style::Hoa => format!(
                            "encoding {tracks} input channels (Objects + DirectSpeakers) → HOA3 ({outputs} ch), {frames} frames"
                        ),
                        Style::Ear => {
                            format!(
                                "rendering {tracks} tracks → {outputs} channels, {frames} frames"
                            )
                        }
                        Style::Speaker => format!(
                            "rendering {tracks} tracks (Objects + DirectSpeakers) → {outputs} channels, {frames} frames"
                        ),
                    },
                ))
            }
            SpeakerPhase::BeginProgress => {
                self.phase = SpeakerPhase::Open;
                Ok(rendering(0.3, 0., 0, 0, begin))
            }
            SpeakerPhase::Open => {
                let c = &self.c;
                let path = input_path(&c.input_path, &c.input)?;
                let kernel =
                    c.prepared.kernel.clone().ok_or_else(|| {
                        Error::new(code::INVALID_ARGUMENT, "无效离线渲染参数", "")
                    })?;
                let reader = Driver::open_reader(path).map_err(|e| c.located(e))?;
                let driver = Driver::new(
                    reader,
                    offline::Settings {
                        kernel,
                        inputs: c.inputs,
                        outputs,
                        frames: c.frames,
                        sample_rate: c.sample_rate,
                        default_interp_ms: c.default_interp_ms,
                        smoothing_frames: u64::from(c.smoothing_frames),
                        window: c.window,
                        meter_window: c.meter_window,
                        backend: module.to_owned(),
                    },
                )
                .map_err(|e| c.located(e))?;
                self.driver = Some(driver);
                self.writer = Some(open_writer(
                    &c.output,
                    &c.output_display,
                    outputs as u32,
                    c.sample_rate,
                    &c.layout,
                )?);
                self.phase = SpeakerPhase::Blocks;
                Ok(Event::Continue)
            }
            SpeakerPhase::Blocks => {
                if cancelled {
                    return Err(cancelled_error("render cancelled", self.c.output_context()));
                }
                let (Some(driver), Some(writer)) = (self.driver.as_mut(), self.writer.as_mut())
                else {
                    unreachable!("opened")
                };
                let Some(block) = driver.render().map_err(|e| located(e, &self.c.input))? else {
                    let m = self
                        .driver
                        .take()
                        .expect("opened")
                        .finish()
                        .map_err(|e| located(e, &self.c.input))?;
                    finish_writer(
                        self.writer.as_mut().expect("opened"),
                        &self.c.output_display,
                    )?;
                    self.metrics = Metrics {
                        lufs: m.lufs,
                        peak_dbtp: m.peak_dbtp,
                    };
                    self.phase = SpeakerPhase::Wrote;
                    return Ok(finished());
                };
                if block.emit_frames > 0 {
                    let from = block.emit_offset * outputs;
                    let to = from + block.emit_frames * outputs;
                    if writer.write(&driver.output()[from..to]).is_err() {
                        return Err(Error::new(code::IO_ERROR, short, self.c.output_context()));
                    }
                }
                let p = driver.commit().map_err(|e| located(e, &self.c.input))?;
                Ok(rendering(
                    p.fraction,
                    p.stage_fraction,
                    p.done,
                    p.total,
                    step,
                ))
            }
            SpeakerPhase::Wrote => {
                self.phase = SpeakerPhase::Done;
                let frames = self.c.frames;
                let (start, end) = match self.c.window {
                    Some(w) => {
                        let start = w.start.min(frames);
                        (start, start + w.count.min(frames - start))
                    }
                    None => (0, frames),
                };
                let window = self
                    .c
                    .window
                    .map(|_| format!(" (window [{start}, {end}) of {frames} frames)"))
                    .unwrap_or_default();
                Ok(info(
                    module,
                    format!(
                        "wrote {} frames to {}{window}",
                        end - start,
                        self.c.output_text()
                    ),
                ))
            }
            SpeakerPhase::Done => Ok(Event::Done(self.metrics)),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum BinauralPhase {
    Open,
    BeginProgress,
    Steps,
    Wrote,
    Done,
}

/// The binaural `render_window`.
struct Binaural {
    c: Common,
    phase: BinauralPhase,
    driver: Option<binaural_render::Offline>,
    writer: Option<Writer>,
    written: u64,
    metrics: Metrics,
}

impl Binaural {
    fn next(&mut self, cancelled: bool) -> Result<Event, Error> {
        let module = self.c.module();
        match self.phase {
            BinauralPhase::Open => {
                let c = &self.c;
                let path = input_path(&c.input_path, &c.input)?;
                let plan =
                    c.prepared.binaural.clone().ok_or_else(|| {
                        Error::new(code::INVALID_ARGUMENT, "无效双耳离线参数", "")
                    })?;
                let reader =
                    binaural_render::Offline::open_reader(path).map_err(|e| c.located(e))?;
                let identity = c.pose == [0.; 3];
                let driver = binaural_render::Offline::new(
                    plan,
                    reader,
                    binaural_render::Settings {
                        inputs: c.inputs,
                        frames: c.frames,
                        sample_rate: c.sample_rate,
                        window: c.window,
                        smoothing_frames: c.smoothing_frames,
                        pose: (!identity).then_some(c.pose),
                    },
                )
                .map_err(|e| c.located(e))?;
                let pools = driver.diagnostic_pools().unwrap_or("").to_owned();
                self.driver = Some(driver);
                self.writer = Some(open_writer(
                    &c.output,
                    &c.output_display,
                    2,
                    c.sample_rate,
                    b"binaural",
                )?);
                self.phase = BinauralPhase::BeginProgress;
                if pools.is_empty() {
                    self.next(cancelled)
                } else {
                    Ok(info(module, pools))
                }
            }
            BinauralPhase::BeginProgress => {
                self.phase = BinauralPhase::Steps;
                Ok(rendering(0.2, 0., 0, 0, "rendering"))
            }
            BinauralPhase::Steps => {
                let driver = self.driver.as_mut().expect("opened");
                match driver.pending() {
                    binaural_render::Pending::Done => {
                        let (written, m) = self.driver.take().expect("opened").finish();
                        finish_writer(
                            self.writer.as_mut().expect("opened"),
                            &self.c.output_display,
                        )?;
                        self.written = written;
                        self.metrics = Metrics {
                            lufs: m.lufs,
                            peak_dbtp: m.peak_dbtp,
                        };
                        self.phase = BinauralPhase::Wrote;
                        return Ok(finished());
                    }
                    binaural_render::Pending::Block if cancelled => {
                        return Err(cancelled_error("render cancelled", self.c.output_context()));
                    }
                    _ => {}
                }
                let Some(step) = driver.next_step().map_err(|e| located(e, &self.c.input))? else {
                    return Ok(Event::Continue);
                };
                if step.emit != 0 {
                    let writer = self.writer.as_mut().expect("opened");
                    if writer.write(driver.output(step.emit)).is_err() {
                        return Err(Error::new(
                            code::IO_ERROR,
                            if step.tail {
                                "short write during binaural spreader tail"
                            } else {
                                "short write during binaural render"
                            },
                            self.c.output_context(),
                        ));
                    }
                }
                Ok(match step.progress {
                    Some(p) => {
                        rendering(p.fraction, p.stage_fraction, p.done, p.total, "rendering")
                    }
                    None => Event::Continue,
                })
            }
            BinauralPhase::Wrote => {
                self.phase = BinauralPhase::Done;
                let frames = self.c.frames;
                let window = self
                    .c
                    .window
                    .map(|w| {
                        let start = w.start.min(frames);
                        let end = start.wrapping_add(w.count).min(frames);
                        format!(" (window [{start}, {end}) of {frames} frames)")
                    })
                    .unwrap_or_default();
                Ok(info(
                    module,
                    format!(
                        "wrote {} frames to {}{window}",
                        self.written,
                        self.c.output_text()
                    ),
                ))
            }
            BinauralPhase::Done => Ok(Event::Done(self.metrics)),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum RoomPhase {
    Open,
    Seek,
    Chunks,
    Done,
}

/// The room `render_window`: the file stream, seeked to the window, with a meter.
struct Room {
    c: Common,
    phase: RoomPhase,
    stream: Option<room_stream::FileStream>,
    writer: Option<Writer>,
    meter: Option<Meter>,
    seeking: bool,
    start: u64,
    count: u64,
    written: u64,
    output: Vec<f32>,
    metrics: Metrics,
}

const ROOM_READ_FRAMES: usize = 2048;

fn meter_error(e: &mradm_dsp::Error) -> Error {
    let code = match e {
        mradm_dsp::Error::InvalidArgument(_) => 1,
        mradm_dsp::Error::Unsupported(_) => 2,
        mradm_dsp::Error::Io(_) => 3,
        mradm_dsp::Error::RenderFailed(_) => 4,
    };
    Error::new(code, cut(&e.to_string(), 256), "meter")
}

/// The part of a block `[start, start + len)` inside the meter window (`meter_window_chunk`).
fn meter_chunk(window: Option<Window>, start: u64, len: u64) -> (usize, usize) {
    let Some(w) = window else {
        return (0, len as usize);
    };
    let lo = start.max(w.start);
    let hi = (start + len).min(w.start + w.count);
    if hi <= lo {
        (0, 0)
    } else {
        ((lo - start) as usize, (hi - lo) as usize)
    }
}

impl Room {
    fn next(&mut self, cancelled: bool, host: &dyn Host) -> Result<Event, Error> {
        match self.phase {
            RoomPhase::Open => {
                let c = &self.c;
                let p = &c.prepared;
                let room = p
                    .room
                    .as_ref()
                    .ok_or_else(|| Error::new(code::INVALID_ARGUMENT, "无效流式渲染参数", ""))?;
                if c.frames != p.frames || c.inputs != p.inputs || c.sample_rate != room.sample_rate
                {
                    return Err(Error::new(
                        code::INVALID_ARGUMENT,
                        "room-binaural prepared 输入格式不匹配",
                        "",
                    ));
                }
                let path = input_path(&c.input_path, &c.input)?;
                let mut stream = room_stream::FileStream::open(
                    path,
                    Arc::clone(&room.table),
                    Arc::clone(&room.resources),
                    room.config.clone(),
                    c.pose,
                    host.room_scope(),
                    host.room_switch(),
                )
                .map_err(|e| c.located(e))?;
                let total = c.frames + stream.tail_frames();
                let (start, count) = match c.window {
                    Some(w) => {
                        let start = w.start.min(total);
                        (start, w.count.min(total - start))
                    }
                    None => (0, total),
                };
                self.seeking = stream.seek_begin(start).map_err(|e| c.located(e))?;
                self.start = start;
                self.count = count;
                self.stream = Some(stream);
                self.phase = RoomPhase::Seek;
                Ok(Event::Continue)
            }
            RoomPhase::Seek => {
                let stream = self.stream.as_mut().expect("opened");
                if self.seeking {
                    if cancelled {
                        return Err(cancelled_error("room-binaural 预热已取消", String::new()));
                    }
                    self.seeking = stream.seek_step().map_err(|e| located(e, &self.c.input))?;
                    return Ok(Event::Continue);
                }
                let rate = stream.sample_rate();
                self.writer = Some(open_writer(
                    &self.c.output,
                    &self.c.output_display,
                    2,
                    rate,
                    b"binaural",
                )?);
                self.meter = Some(
                    Meter::new(2, rate, MeterMode::IntegratedTruePeak, &[])
                        .map_err(|e| meter_error(&e))?,
                );
                self.phase = RoomPhase::Chunks;
                Ok(Event::Continue)
            }
            RoomPhase::Chunks => {
                if self.written >= self.count {
                    finish_writer(
                        self.writer.as_mut().expect("opened"),
                        &self.c.output_display,
                    )?;
                    let meter = self.meter.as_ref().expect("opened");
                    self.metrics = Metrics {
                        lufs: meter.integrated().ok().filter(|v| v.is_finite()),
                        peak_dbtp: meter
                            .max_true_peak()
                            .ok()
                            .map(|v| amplitude_to_db(v.max(1e-10))),
                    };
                    self.phase = RoomPhase::Done;
                    return Ok(Event::Done(self.metrics));
                }
                if cancelled {
                    return Err(cancelled_error(
                        "room-binaural 离线渲染已取消",
                        String::new(),
                    ));
                }
                let frames = (ROOM_READ_FRAMES as u64).min(self.count - self.written) as usize;
                let stream = self.stream.as_mut().expect("opened");
                let rendered = stream
                    .process(&mut self.output, frames)
                    .map_err(|e| located(e, &self.c.input))?;
                let incomplete = || Error::new(code::IO_ERROR, "room-binaural 输出未完成", "");
                let samples = &self.output[..rendered * 2];
                if rendered == 0
                    || self
                        .writer
                        .as_mut()
                        .expect("opened")
                        .write(samples)
                        .is_err()
                {
                    return Err(incomplete());
                }
                let (offset, len) = meter_chunk(
                    self.c.meter_window,
                    self.start + self.written,
                    rendered as u64,
                );
                if len != 0 {
                    self.meter
                        .as_mut()
                        .expect("opened")
                        .add_frames(&samples[offset * 2..(offset + len) * 2])
                        .map_err(|e| meter_error(&e))?;
                }
                self.written += rendered as u64;
                let fraction = self.written as f64 / self.count as f64;
                Ok(rendering(
                    fraction,
                    fraction,
                    self.written,
                    self.count,
                    "room-binaural",
                ))
            }
            RoomPhase::Done => Ok(Event::Done(self.metrics)),
        }
    }
}

enum Inner {
    Speaker(Box<Speaker>),
    Binaural(Box<Binaural>),
    Room(Box<Room>),
    Spent,
}

/// One offline render of a preparation, stepped by [`Run::next`] until it returns
/// [`Event::Done`] or an error.
pub struct Run(Inner);

impl Run {
    pub fn new(prepared: Arc<Prepared>, s: &Settings) -> Self {
        let c = Common {
            prepared: Arc::clone(&prepared),
            input: text(s.input_display),
            input_path: s.input_path.to_vec(),
            output: s.output_path.to_vec(),
            output_display: text(s.output_display),
            layout: s.output_layout.to_vec(),
            inputs: s.inputs,
            frames: s.frames,
            sample_rate: s.sample_rate,
            default_interp_ms: s.default_interp_ms,
            smoothing_frames: s.smoothing_frames,
            window: s.window,
            meter_window: s.meter_window,
            pose: s.pose,
        };
        Self(match prepared.kind {
            Kind::Binaural => Inner::Binaural(Box::new(Binaural {
                c,
                phase: BinauralPhase::Open,
                driver: None,
                writer: None,
                written: 0,
                metrics: Metrics::default(),
            })),
            Kind::Room => Inner::Room(Box::new(Room {
                c,
                phase: RoomPhase::Open,
                stream: None,
                writer: None,
                meter: None,
                seeking: false,
                start: 0,
                count: 0,
                written: 0,
                output: vec![0.; ROOM_READ_FRAMES * 2],
                metrics: Metrics::default(),
            })),
            kind => Inner::Speaker(Box::new(Speaker {
                c,
                style: match kind {
                    Kind::Hoa => Style::Hoa,
                    Kind::Ear => Style::Ear,
                    _ => Style::Speaker,
                },
                phase: SpeakerPhase::BeginLog,
                driver: None,
                writer: None,
                metrics: Metrics::default(),
            })),
        })
    }

    /// The next event; `cancelled` is the caller's stop token now. After an error or
    /// [`Event::Done`] the run is spent.
    pub fn next(&mut self, cancelled: bool, host: &dyn Host) -> Result<Event, Error> {
        let result = match &mut self.0 {
            Inner::Speaker(run) => run.next(cancelled),
            Inner::Binaural(run) => run.next(cancelled),
            Inner::Room(run) => run.next(cancelled, host),
            Inner::Spent => Err(Error::new(code::INVALID_ARGUMENT, "离线渲染已结束", "")),
        };
        if matches!(result, Err(_) | Ok(Event::Done(_))) {
            self.0 = Inner::Spent;
        }
        result
    }
}

fn cancelled_error(message: &str, context: String) -> Error {
    Error::new(CANCELLED, message, context)
}
