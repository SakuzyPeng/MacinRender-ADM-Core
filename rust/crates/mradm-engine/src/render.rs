//! The offline render service (PCM consistency 5.2 C2), moved from the C++
//! `RenderService::render` with the same order, messages, locators, logs, progress events and
//! cancellation points: validation, import and policy, the request resolution (5.2 B), the
//! semantic report, the output decisions and temporary files, the backend preparation and render
//! (5.2 C1), the post-processing passes, encoding, installation, the WAV layout and metadata.
//!
//! [`Render::next`] advances one step. What only the platform layer can do arrives as an
//! [`Island`] request the caller executes and answers with [`Render::resume`]: the encoders, the
//! FLAC / Matroska / MP4 metadata, the MP4 packager probe and the Apple backend.
use crate::backend::{
    self, BinauralSpreadMode, Host, Kind, Prepared, Routing, Screens, SpeakerSpread,
};
use crate::catalog::{self, Backend, Platform};
use crate::fmt::fixed;
use crate::offline::{self, Event as RunEvent, Metrics, Progress, Run};
use crate::request::{self, BitDepth, DsRouting, Error, Level, Log, Packager, Selection, code};
use mradm_dsp::post_gain::{self, RenderGainRequest, Stage};
use mradm_dsp::room::loading::Package;
use mradm_render::offline::Window;
use mradm_render::postprocess::{self, Begin, Failure, Pass, Profile};
use mradm_scene::{Context, ImportOptions, RenderScene, direct_matrix, policy, report};
use std::collections::VecDeque;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;

/// The C++ `RenderStage` values.
pub mod stage {
    pub const VALIDATING: u32 = 0;
    pub const PROBING: u32 = 1;
    pub const IMPORTING_SCENE: u32 = 2;
    pub const PLANNING: u32 = 3;
    pub const RENDERING: u32 = 4;
    pub const POST_PROCESSING: u32 = 5;
    pub const FINISHED: u32 = 6;
}

/// The C++ `RenderOperation` values.
pub mod operation {
    pub const UNKNOWN: u32 = 0;
    pub const VALIDATE_REQUEST: u32 = 1;
    pub const PROBE_INPUT: u32 = 2;
    pub const IMPORT_SCENE: u32 = 3;
    pub const APPLY_SEMANTIC_POLICY: u32 = 4;
    pub const PLAN_RENDER: u32 = 5;
    pub const PREPARE_BACKEND: u32 = 6;
    pub const RENDER_AUDIO: u32 = 7;
    pub const TRIM_OUTPUT: u32 = 8;
    pub const APPLY_GAIN: u32 = 9;
    pub const CONVERT_BIT_DEPTH: u32 = 10;
    pub const ENCODE_FLAC: u32 = 11;
    pub const ENCODE_OPUS: u32 = 12;
    pub const ENCODE_APAC: u32 = 13;
    pub const ENCODE_IAMF: u32 = 14;
    pub const PACKAGE_IAMF_MP4: u32 = 15;
    pub const WRITE_METADATA: u32 = 16;
    pub const FINISH: u32 = 17;
}

const CANCELLED: i32 = 5;
const ENCODER: &str = "MacinRender ADM Core Alpha";

/// A path as the files open it (UTF-8) and as the caller spells it in messages.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PathText {
    pub utf8: String,
    pub display: String,
}

impl PathText {
    /// A path the engine built (temporary files): one spelling.
    fn built(path: &Path) -> Self {
        let text = path.to_string_lossy().into_owned();
        Self {
            utf8: text.clone(),
            display: text,
        }
    }
}

/// A user SOFA file: its paths and file name (UTF-8).
#[derive(Clone, Debug, Default)]
pub struct SofaPath {
    pub path: PathText,
    pub file_name: Vec<u8>,
}

/// `RoomConfig`.
#[derive(Clone, Default)]
pub struct RoomOptions {
    pub package: Option<Arc<Package>>,
    pub hrir: Vec<u8>,
    pub metres: f64,
    pub quality: u32,
}

/// `RenderRequest` with its `RenderOptions`; the enum fields are the C++ values.
#[derive(Clone)]
pub struct Request {
    pub input: PathText,
    pub output: Option<PathText>,
    pub renderer: u32,
    pub output_layout: String,
    pub input_layout: Option<String>,
    pub input_channel_labels: Vec<String>,
    pub measure_loudness: bool,
    pub loudness_target_lufs: f32,
    pub peak_limit: bool,
    pub peak_limit_dbtp: f32,
    pub peak_normalize_to_limit: bool,
    pub final_gain_db: f64,
    pub output_bit_depth: u32,
    pub opus_bitrate_per_ch_kbps: u32,
    pub apac_bitrate_kbps: u32,
    pub apac_drc_music: bool,
    pub apac_caf: bool,
    pub sofa: Option<SofaPath>,
    pub semantic_policy_path: Option<PathText>,
    pub semantic_policy_json: Option<Vec<u8>>,
    pub matrix_path: Option<PathText>,
    pub matrix_json: Option<Vec<u8>>,
    pub semantic_report_path: Option<PathText>,
    pub capture_semantic_report: bool,
    pub default_interp_ms: u32,
    pub object_smoothing_frames: u32,
    pub apple_geometry: bool,
    pub routing: u32,
    pub speaker_spread: u32,
    pub triple_balance_mode: u32,
    pub binaural_spread: u32,
    pub split_power: bool,
    pub apple_spatial_preset: u32,
    pub apple_speaker_rendering_flags: bool,
    pub listener_orientation: [f32; 3],
    pub render_start_sec: f64,
    pub render_end_sec: Option<f64>,
    pub room: RoomOptions,
    pub screens: Screens,
    pub internal_allow_speaker_stereo: bool,
    pub iamf_mp4: bool,
    pub iamf_layers: Vec<String>,
}

/// Optional reusable inputs. A cached backend preparation is independent of whether the caller
/// supplies a policy-applied scene or asks the render to import and apply the policy again.
#[derive(Default)]
pub struct Reuse {
    pub scene: Option<RenderScene>,
    pub prepared: Option<Arc<Prepared>>,
    /// The Apple preparation the caller caches.
    pub apple_prepared: bool,
}

/// The `RenderPlan` fields the Apple island needs besides the request.
#[derive(Clone, Debug)]
pub struct ApplePlan {
    pub output_path: PathText,
    pub output_layout: String,
    pub window: Option<Window>,
    pub meter_window: Option<Window>,
    pub matrix: Option<direct_matrix::Matrix>,
}

/// `audio::MetadataFields`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MetadataFields {
    pub encoder: String,
    pub date_utc: String,
    pub renderer: String,
    pub output_layout: String,
    pub lufs: Option<f64>,
    pub peak_dbtp: Option<f64>,
}

/// What the caller executes; its progress events map through [`Render::map_progress`].
#[derive(Clone, Debug)]
pub enum Island {
    /// `audio::detect_iamf_mp4_packager`.
    ProbeMp4Packager,
    /// The Apple backend's `prepare` (skipped when the caller holds its preparation).
    ApplePrepare(ApplePlan),
    /// The Apple backend's `render_window`; the reply carries the metrics.
    AppleRender(ApplePlan),
    EncodeFlac {
        src: PathText,
        dst: PathText,
    },
    EncodeOpus {
        src: PathText,
        dst: PathText,
        layout: String,
        bitrate: u32,
    },
    EncodeApac {
        src: PathText,
        dst: PathText,
        layout: String,
        bitrate: u32,
        drc_music: bool,
        caf: bool,
    },
    EncodeIamf {
        src: PathText,
        dst: PathText,
        layout: String,
        bitrate: u32,
        lufs: Option<f64>,
        peak_dbtp: Option<f64>,
        layers: Vec<String>,
    },
    PackageMp4 {
        src: PathText,
        dst: PathText,
    },
    /// FLAC, Matroska and MP4 metadata (`audio::write_file_metadata`).
    WriteMetadata {
        path: PathText,
        fields: MetadataFields,
    },
}

/// The caller's answer to an island.
#[derive(Clone, Debug, Default)]
pub struct Reply {
    pub error: Option<Error>,
    /// AppleRender.
    pub metrics: Metrics,
    /// ProbeMp4Packager.
    pub packager: Packager,
    pub ffmpeg_major: i32,
}

/// `RenderResult`: `error` none on success; one log line per result as the C++ service gave.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Outcome {
    pub error: Option<Error>,
    pub output_path: Option<String>,
    pub metrics: Option<Metrics>,
    pub logs: Vec<(Level, String)>,
    pub semantic_report: Option<String>,
}

#[derive(Clone, Debug)]
pub enum Event {
    Progress(Progress),
    Log(Log),
    Continue,
    Island(Island),
    /// The result is ready ([`Render::outcome`]).
    Done,
}

/// A progress range (`ProgressRangeSink`).
#[derive(Clone, Copy, Debug)]
struct Range {
    stage: u32,
    operation: u32,
    start: f64,
    end: f64,
    fallback: &'static str,
}

fn clamp(value: f64) -> f64 {
    if value.is_finite() {
        value.clamp(0., 1.)
    } else {
        0.
    }
}

impl Range {
    fn new(stage: u32, operation: u32, start: f64, end: f64, fallback: &'static str) -> Self {
        Self {
            stage,
            operation,
            start: clamp(start),
            end: clamp(end),
            fallback,
        }
    }
    fn map(&self, e: &Progress) -> Progress {
        let local = if e.stage == stage::FINISHED {
            1.
        } else {
            clamp(e.stage_fraction)
        };
        let mapped = self.start + (self.end - self.start) * local;
        let operation = if e.operation == operation::UNKNOWN || e.stage == stage::FINISHED {
            self.operation
        } else {
            e.operation
        };
        Progress {
            stage: self.stage,
            operation,
            fraction: clamp(mapped),
            stage_fraction: local,
            current: e.current,
            total: e.total,
            message: if e.message.is_empty() {
                self.fallback
            } else {
                e.message
            },
        }
    }
}

/// `emit_progress`.
fn progress(
    stage: u32,
    operation: u32,
    fraction: f64,
    stage_fraction: f64,
    message: &'static str,
) -> Event {
    Event::Progress(Progress {
        stage,
        operation,
        fraction: clamp(fraction),
        stage_fraction: clamp(stage_fraction),
        current: 0,
        total: 0,
        message,
    })
}

fn log(level: Level, module: &'static str, message: impl Into<String>) -> Event {
    Event::Log(Log {
        level,
        module,
        message: message.into(),
    })
}

fn cut(message: &str, capacity: usize) -> String {
    let mut n = message.len().min(capacity - 1);
    while !message.is_char_boundary(n) {
        n -= 1;
    }
    message[..n].to_owned()
}

fn code_of(code: i32) -> i32 {
    if (1..=6).contains(&code) { code } else { 6 }
}

fn cancelled_error(context: impl Into<String>) -> Error {
    Error::new(CANCELLED, "render cancelled", context)
}

/// A scene import or policy error as `io::RenderScene` reported it (1024-byte message buffer).
fn scene_error(e: &mradm_scene::Error, path: &str, label: &str) -> Error {
    let context = match e.context {
        Context::None => String::new(),
        Context::Input => format!("input={path}"),
        Context::Path => format!("path={path}"),
        Context::Label => label.to_owned(),
    };
    Error::new(code_of(e.code), cut(&e.message, 1024), context)
}

/// A post-processing failure through the 512 / 1024-byte C++ buffers.
fn pass_error(f: Failure) -> Error {
    Error::new(code_of(f.code), cut(&f.message, 512), cut(&f.context, 1024))
}

/// A temporary file removed unless dismissed.
struct Temp {
    path: PathBuf,
    active: bool,
}

impl Temp {
    fn new(path: PathBuf) -> Self {
        Self { path, active: true }
    }
    fn remove_now(&mut self) {
        if self.active {
            let _ = std::fs::remove_file(&self.path);
            self.active = false;
        }
    }
    fn dismiss(&mut self) {
        self.active = false;
    }
}

impl Drop for Temp {
    fn drop(&mut self) {
        self.remove_now();
    }
}

/// The final output, removed on failure once armed (`OutputCleanupGuard`).
struct OutputGuard {
    path: PathBuf,
    active: bool,
}

impl OutputGuard {
    fn remove_now(&mut self) {
        if self.active {
            let _ = std::fs::remove_file(&self.path);
            self.active = false;
        }
    }
}

impl Drop for OutputGuard {
    fn drop(&mut self) {
        self.remove_now();
    }
}

/// `std::filesystem::path` parts of a UTF-8 path.
fn file_name(path: &str) -> &str {
    path.rsplit(['/', std::path::MAIN_SEPARATOR])
        .next()
        .unwrap_or("")
}

fn parent(path: &str) -> &str {
    match path.rfind(['/', std::path::MAIN_SEPARATOR]) {
        Some(0) => &path[..1],
        Some(i) => &path[..i],
        None => "",
    }
}

fn extension(path: &str) -> &str {
    let name = file_name(path);
    if name == "." || name == ".." {
        return "";
    }
    match name.rfind('.') {
        Some(i) if i > 0 => &name[i..],
        _ => "",
    }
}

fn stem(path: &str) -> &str {
    let name = file_name(path);
    let ext = extension(path);
    &name[..name.len() - ext.len()]
}

fn join(parent: &str, name: &str) -> String {
    if parent.is_empty() {
        name.to_owned()
    } else if parent.ends_with(['/', std::path::MAIN_SEPARATOR]) {
        format!("{parent}{name}")
    } else {
        format!("{parent}{}{name}", std::path::MAIN_SEPARATOR)
    }
}

/// `unique_sidecar_temp_path`: `<stem>.<purpose>.<16 hex><ext>` beside the final output, from 16
/// candidates that do not exist (the last draw otherwise).
fn sidecar(final_path: &str, purpose: &str, ext: &str) -> PathBuf {
    use std::hash::{BuildHasher, Hasher};
    let state = std::collections::hash_map::RandomState::new();
    let mut counter = 0u64;
    let mut draw = || {
        let mut h = state.build_hasher();
        h.write_u64(counter);
        counter += 1;
        h.finish()
    };
    let (dir, stem) = (parent(final_path), stem(final_path));
    for _ in 0..16 {
        let candidate = PathBuf::from(join(dir, &format!("{stem}.{purpose}.{:016x}{ext}", draw())));
        if !candidate.exists() {
            return candidate;
        }
    }
    PathBuf::from(join(dir, &format!("{stem}.{purpose}.{:016x}{ext}", draw())))
}

/// `replace_output_file`: rename, or remove the final file and rename again.
fn replace_output(temp: &Path, final_path: &Path, display: &str) -> Result<(), Error> {
    if std::fs::rename(temp, final_path).is_ok() {
        return Ok(());
    }
    let _ = std::fs::remove_file(final_path);
    std::fs::rename(temp, final_path).map_err(|e| {
        Error::new(
            code::IO_ERROR,
            format!(
                "failed to replace output file: {}",
                postprocess::os_message(&e)
            ),
            format!("path={display}"),
        )
    })
}

/// The civil UTC date of a Unix time (`gmtime`).
pub fn utc_date(seconds: i64) -> String {
    let days = seconds.div_euclid(86_400);
    let secs = seconds.rem_euclid(86_400);
    // Howard Hinnant's civil_from_days.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        secs / 3600,
        secs % 3600 / 60,
        secs % 60
    )
}

fn now() -> i64 {
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => d.as_secs() as i64,
        Err(e) => -(e.duration().as_secs() as i64),
    }
}

/// `compact_metadata_comment`.
fn comment(m: &MetadataFields) -> String {
    let mut parts = Vec::new();
    if !m.renderer.is_empty() {
        parts.push(format!("renderer={}", m.renderer));
    }
    if !m.output_layout.is_empty() {
        parts.push(format!("layout={}", m.output_layout));
    }
    if let Some(l) = m.lufs {
        parts.push(format!("loudness={}LUFS", fixed(l, 1, false)));
    }
    if let Some(p) = m.peak_dbtp {
        parts.push(format!("peak={}dBTP", fixed(p, 2, false)));
    }
    parts.join(" ")
}

fn wav_failure(e: mradm_wav::Error, display: &str) -> Error {
    Error::new(
        code_of(e.code),
        cut(&e.message, 512),
        format!("path={display}"),
    )
}

/// The WAV (bext, HOA3 `ambi`) and CAF (`info`) metadata (`write_wav_metadata`,
/// `write_caf_metadata`); `None` for another format.
fn write_own_metadata(ext: &str, path: &PathText, m: &MetadataFields) -> Option<Result<(), Error>> {
    match ext {
        ".caf" => Some(
            mradm_wav::caf::append_info(
                Path::new(&path.utf8),
                m.encoder.as_bytes(),
                m.date_utc.as_bytes(),
                comment(m).as_bytes(),
            )
            .map_err(|e| wav_failure(e, &path.display)),
        ),
        ".wav" => Some((|| {
            let description = if m.renderer.is_empty() {
                String::new()
            } else {
                format!("renderer={} layout={}", m.renderer, m.output_layout)
            };
            let payload = mradm_wav::bext_payload(&mradm_wav::BextFields {
                description: description.as_bytes(),
                originator: m.encoder.as_bytes(),
                originator_reference: m.output_layout.as_bytes(),
                date_utc: m.date_utc.as_bytes(),
                loudness: m.lufs,
                true_peak: m.peak_dbtp,
            });
            let mut file = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&path.utf8)
                .map_err(|e| wav_failure(mradm_wav::Error::from(e), &path.display))?;
            mradm_wav::append_bext(
                &mut file,
                &payload,
                (m.output_layout == "hoa3").then_some(&mradm_wav::AMBI_HOA3),
            )
            .map_err(|e| wav_failure(e, &path.display))
        })()),
        _ => None,
    }
}

/// One post-processing pass (`run_output_pass`): cancellation before each step and, when the pass
/// asks, before installing; progress through the pass's range.
struct PassRun {
    pass: Option<Box<Pass>>,
    /// The `path=` text of a cancellation.
    path: String,
    install_check: bool,
    range: Range,
    pass_operation: u32,
    finishing: bool,
    done: bool,
}

enum PassStep {
    Event(Event),
    Done,
}

impl PassRun {
    fn start(
        begin: postprocess::Result<Begin>,
        path: &str,
        range: Range,
        pass_operation: u32,
    ) -> Result<(Self, Option<Event>), Error> {
        let mut run = Self {
            pass: None,
            path: path.to_owned(),
            install_check: false,
            range,
            pass_operation,
            finishing: false,
            done: false,
        };
        let first = match begin.map_err(pass_error)? {
            Begin::Skipped(p) => {
                run.done = true;
                p
            }
            Begin::Started(pass, p) => {
                run.install_check = pass.cancel_before_install();
                run.pass = Some(pass);
                p
            }
        };
        let event = run.emit(&first);
        Ok((run, event))
    }

    fn emit(&self, p: &postprocess::Progress) -> Option<Event> {
        p.emit.then(|| {
            let f = p.fraction.clamp(0., 1.);
            Event::Progress(self.range.map(&Progress {
                stage: stage::POST_PROCESSING,
                operation: self.pass_operation,
                fraction: f,
                stage_fraction: f,
                current: p.done,
                total: p.total,
                message: p.message,
            }))
        })
    }

    fn next(&mut self, cancelled: bool) -> Result<PassStep, Error> {
        if self.done {
            return Ok(PassStep::Done);
        }
        if self.finishing {
            if self.install_check && cancelled {
                return Err(cancelled_error(format!("path={}", self.path)));
            }
            let pass = *self.pass.take().expect("started");
            let p = pass.finish().map_err(pass_error)?;
            self.done = true;
            return Ok(match self.emit(&p) {
                Some(e) => PassStep::Event(e),
                None => PassStep::Done,
            });
        }
        if cancelled {
            return Err(cancelled_error(format!("path={}", self.path)));
        }
        let step = self
            .pass
            .as_mut()
            .expect("started")
            .step()
            .map_err(pass_error)?;
        self.finishing = step.complete;
        Ok(PassStep::Event(
            self.emit(&step.progress).unwrap_or(Event::Continue),
        ))
    }
}

/// The import's and policy's shared steps: the semantic policy of `options` applied to `scene`
/// (`resolve_and_apply_policy`), the in-memory JSON preferred over the file; with its warnings.
pub fn apply_policy(
    scene: &mut RenderScene,
    json: Option<&[u8]>,
    path: Option<&PathText>,
    input_display: &str,
) -> request::Outcome<()> {
    let mut logs = Vec::new();
    let warn = |logs: &mut Vec<Log>, message: String| {
        logs.push(Log {
            level: Level::Warning,
            module: "semantic-policy",
            message,
        })
    };
    let (text, label) = match (json, path) {
        (Some(json), path) => {
            if path.is_some() {
                warn(
                    &mut logs,
                    "in-memory semantic policy supplied; ignoring semantic_policy_path".into(),
                );
            }
            (json.to_vec(), "<memory>".to_owned())
        }
        (None, Some(path)) => match read_policy(path) {
            Ok(text) => (text, path.display.clone()),
            Err(e) => {
                return request::Outcome {
                    logs,
                    result: Err(e),
                };
            }
        },
        (None, None) => {
            return request::Outcome {
                logs,
                result: Ok(()),
            };
        }
    };
    let parsed = match policy::parse(&text) {
        Ok(p) => p,
        Err(e) => {
            return request::Outcome {
                logs,
                result: Err(scene_error(&e, input_display, &label)),
            };
        }
    };
    for w in scene.apply_policy(parsed).to_vec() {
        warn(&mut logs, w);
    }
    request::Outcome {
        logs,
        result: Ok(()),
    }
}

/// `io::read_policy_file`: an unopenable file fails; a read error keeps what was read (the C++
/// stream's behavior, e.g. for a directory).
fn read_policy(path: &PathText) -> Result<Vec<u8>, Error> {
    let mut file = std::fs::File::open(&path.utf8).map_err(|_| {
        Error::new(
            code::IO_ERROR,
            "semantic policy I/O error: cannot open file",
            path.display.clone(),
        )
    })?;
    let mut text = Vec::new();
    let _ = file.read_to_end(&mut text);
    Ok(text)
}

/// `io::write_policy_json`: the JSON and a newline.
fn write_policy_json(path: &PathText, json: &str, what: &str) -> Result<(), Error> {
    let mut file = std::fs::File::create(&path.utf8).map_err(|_| {
        Error::new(
            code::IO_ERROR,
            format!("semantic policy I/O error: cannot open {what} for writing"),
            path.display.clone(),
        )
    })?;
    file.write_all(json.as_bytes())
        .and_then(|()| file.write_all(b"\n"))
        .map_err(|_| {
            Error::new(
                code::IO_ERROR,
                format!("semantic policy I/O error: failed to write {what}"),
                path.display.clone(),
            )
        })
}

fn renderer_name(sel: Selection) -> &'static str {
    match sel {
        Selection::Automatic => "auto",
        Selection::Ear => "ear",
        Selection::TripleBalance => "triple-balance",
        Selection::Saf => "saf",
        Selection::Hoa => "hoa",
        Selection::Apple => "apple",
        Selection::Binaural => "binaural",
        Selection::SafBinaural => "saf-binaural",
        Selection::RoomBinaural => "room-binaural",
    }
}

fn preset_name(preset: u32) -> &'static str {
    match preset {
        0 => "off",
        1 => "headphone-default",
        2 => "headphone-movie",
        _ => "unknown",
    }
}

/// The capability report of a resolved backend.
fn capabilities(backend: Selection, platform: &Platform) -> Option<Backend> {
    Some(match backend {
        Selection::Ear => catalog::ear(),
        Selection::Saf => catalog::vbap(),
        Selection::TripleBalance => catalog::triple_balance(),
        Selection::Hoa => catalog::hoa(),
        Selection::SafBinaural => catalog::binaural(platform),
        Selection::RoomBinaural => catalog::room(),
        Selection::Apple => platform.apple.clone()?,
        _ => return None,
    })
}

/// `channel_count_for_layout`.
fn channel_count(caps: &Backend, layout: &str) -> Option<u16> {
    caps.layouts
        .iter()
        .find(|l| l.id == layout)
        .or_else(|| {
            (layout == "binaural")
                .then(|| caps.layouts.iter().find(|l| l.is_binaural))
                .flatten()
        })
        .map(|l| l.channel_count)
}

fn kind_of(backend: Selection) -> Option<Kind> {
    Some(match backend {
        Selection::Ear => Kind::Ear,
        Selection::Saf => Kind::Vbap,
        Selection::TripleBalance => Kind::TripleBalance,
        Selection::Hoa => Kind::Hoa,
        Selection::SafBinaural => Kind::Binaural,
        Selection::RoomBinaural => Kind::Room,
        _ => return None,
    })
}

/// The steps of `RenderService::render`, in order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Step {
    Validate,
    Import,
    ImportCancel,
    Resolve,
    Matrix,
    Report,
    Probe,
    Target,
    Plan,
    Prepare,
    Prepared,
    Render,
    Rendered,
    Measured,
    TrimCancel,
    GainCancel,
    BitdepthCancel,
    Encode,
    IamfEncode,
    IamfPackage,
    Install,
    Finalize,
    Metadata,
    Finish,
}

/// What a pending island's reply continues with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Awaiting {
    Packager,
    ApplePrepare,
    AppleRender,
    Flac,
    Opus,
    Apac,
    Iamf,
    Package,
    Metadata,
}

/// After a pass, the step that continues.
#[derive(Clone, Copy, Debug)]
struct Passing {
    run_then: Step,
}

/// One offline render, stepped by [`Render::next`].
pub struct Render {
    r: Request,
    platform: Platform,
    preimported: bool,
    apple_cached: bool,
    cached: Option<Arc<Prepared>>,
    new_prepared: Option<Arc<Prepared>>,
    queue: VecDeque<Event>,
    step: Step,
    awaiting: Option<Awaiting>,
    outcome: Option<Outcome>,
    scene: Option<RenderScene>,
    trim: Option<request::Trim>,
    output: PathText,
    sel: Selection,
    backend: Selection,
    layout: String,
    caps: Backend,
    needs_report: bool,
    report_ready: bool,
    report: Option<String>,
    matrix: Option<direct_matrix::Matrix>,
    final_ext: String,
    packager: Packager,
    ffmpeg_major: i32,
    target: request::OutputTarget,
    render_temp: Option<Temp>,
    render_path: PathText,
    output_temp: Option<Temp>,
    encoded: PathText,
    output_guard: Option<OutputGuard>,
    iamf_temp: Option<Temp>,
    iamf_path: PathText,
    window_render: bool,
    prepared: Option<Arc<Prepared>>,
    run: Option<Run>,
    range: Option<Range>,
    metrics: Metrics,
    gain_db: f64,
    pass: Option<(PassRun, Passing)>,
    pending: Option<PendingPass>,
    encode: u8,
}

/// A pass to begin on the next step: its own cancellation check and begin then see the caller's
/// stop token after the events before it.
#[derive(Clone, Copy, Debug)]
enum PassKind {
    Trim { start: u64, count: u64 },
    Gain(f32),
    Bitdepth(u16),
    Finalize(Profile),
}

#[derive(Clone, Debug)]
struct PendingPass {
    kind: PassKind,
    path: PathText,
    range: Range,
    then: Step,
}

impl Render {
    pub fn new(r: Request, platform: Platform, reuse: Reuse) -> Self {
        let preimported = reuse.scene.is_some();
        Self {
            r,
            platform,
            preimported,
            apple_cached: reuse.apple_prepared,
            cached: reuse.prepared,
            new_prepared: None,
            queue: VecDeque::new(),
            step: Step::Validate,
            awaiting: None,
            outcome: None,
            scene: reuse.scene,
            trim: None,
            output: PathText::default(),
            sel: Selection::Automatic,
            backend: Selection::Automatic,
            layout: String::new(),
            caps: Backend::default(),
            needs_report: false,
            report_ready: false,
            report: None,
            matrix: None,
            final_ext: String::new(),
            packager: Packager::None,
            ffmpeg_major: -1,
            target: request::OutputTarget::default(),
            render_temp: None,
            render_path: PathText::default(),
            output_temp: None,
            encoded: PathText::default(),
            output_guard: None,
            iamf_temp: None,
            iamf_path: PathText::default(),
            window_render: false,
            prepared: None,
            run: None,
            range: None,
            metrics: Metrics::default(),
            gain_db: 0.,
            pass: None,
            pending: None,
            encode: 0,
        }
    }

    /// The result, once [`Event::Done`] was returned.
    pub fn outcome(&self) -> Option<&Outcome> {
        self.outcome.as_ref()
    }

    /// The preparation this render made (the caller may cache it for later windows).
    pub fn take_prepared(&mut self) -> Option<Arc<Prepared>> {
        self.new_prepared.take()
    }

    /// The scene, for the Apple island.
    pub fn scene(&self) -> Option<&RenderScene> {
        self.scene.as_ref()
    }

    /// An island's raw progress event mapped into its range (identity outside one).
    pub fn map_progress(&self, e: &Progress) -> Progress {
        self.range.map_or(*e, |r| r.map(e))
    }

    fn finish_with(&mut self, outcome: Outcome) {
        // The temporary files and an armed partial output go before the result is read.
        self.run = None;
        self.pass = None;
        self.pending = None;
        self.iamf_temp = None;
        self.output_guard = None;
        self.output_temp = None;
        self.render_temp = None;
        self.outcome = Some(outcome);
        self.queue.push_back(Event::Done);
    }

    /// A failure with its result log line, and the semantic report once it may exist.
    fn fail(&mut self, e: Error, level: Level) {
        let message = e.message.clone();
        let semantic_report = if self.report_ready {
            self.report.clone()
        } else {
            None
        };
        self.finish_with(Outcome {
            error: Some(e),
            logs: vec![(level, message)],
            semantic_report,
            ..Outcome::default()
        });
    }

    fn fail_silent(&mut self, e: Error) {
        self.finish_with(Outcome {
            error: Some(e),
            ..Outcome::default()
        });
    }

    fn cancelled(&mut self) {
        self.fail(cancelled_error(""), Level::Info);
    }

    fn push(&mut self, e: Event) {
        self.queue.push_back(e);
    }

    fn push_logs(&mut self, logs: Vec<Log>, fallback: &'static str) {
        for mut l in logs {
            if l.module.is_empty() {
                l.module = fallback;
            }
            self.queue.push_back(Event::Log(l));
        }
    }

    /// The next event; `cancelled` is the caller's stop token now.
    pub fn next(&mut self, cancelled: bool, host: &dyn Host) -> Event {
        loop {
            if let Some(e) = self.queue.pop_front() {
                if let Event::Island(_) = e {
                    debug_assert!(self.awaiting.is_some());
                }
                return e;
            }
            if self.outcome.is_some() || self.awaiting.is_some() {
                // Spent, or the caller has not answered the island.
                return Event::Continue;
            }
            if let Some(e) = self.advance(cancelled, host) {
                return e;
            }
        }
    }

    /// Answers the pending island.
    pub fn resume(&mut self, reply: Reply) {
        let Some(awaiting) = self.awaiting.take() else {
            return;
        };
        self.range = None;
        match awaiting {
            Awaiting::Packager => {
                self.packager = reply.packager;
                self.ffmpeg_major = reply.ffmpeg_major;
                self.step = Step::Target;
            }
            Awaiting::ApplePrepare => self.prepared_done(None, reply.error, None),
            Awaiting::AppleRender => self.rendered(reply.error.map_or(Ok(reply.metrics), Err)),
            Awaiting::Flac | Awaiting::Opus | Awaiting::Apac => {
                if let Some(t) = self.render_temp.as_mut() {
                    t.remove_now();
                }
                if let Some(e) = reply.error {
                    return self.fail(e, Level::Error);
                }
                if awaiting == Awaiting::Opus {
                    self.push(progress(
                        stage::POST_PROCESSING,
                        operation::ENCODE_OPUS,
                        0.98,
                        1.0,
                        "Opus encoded",
                    ));
                }
                self.step = Step::Encode;
            }
            Awaiting::Iamf => {
                if let Some(t) = self.render_temp.as_mut() {
                    t.remove_now();
                }
                if let Some(e) = reply.error {
                    return self.fail(e, Level::Error);
                }
                if self.target.iamf_mp4 {
                    self.push(progress(
                        stage::POST_PROCESSING,
                        operation::ENCODE_IAMF,
                        0.96,
                        1.0,
                        "IAMF encoded",
                    ));
                    self.push(progress(
                        stage::POST_PROCESSING,
                        operation::PACKAGE_IAMF_MP4,
                        0.96,
                        0.0,
                        "packaging IAMF MP4",
                    ));
                    self.step = Step::IamfPackage;
                } else {
                    self.push(progress(
                        stage::POST_PROCESSING,
                        operation::ENCODE_IAMF,
                        0.98,
                        1.0,
                        "IAMF encoded",
                    ));
                    self.step = Step::Encode;
                }
            }
            Awaiting::Package => {
                if let Some(t) = self.iamf_temp.as_mut() {
                    t.remove_now();
                }
                if let Some(e) = reply.error {
                    return self.fail(e, Level::Error);
                }
                self.push(progress(
                    stage::POST_PROCESSING,
                    operation::PACKAGE_IAMF_MP4,
                    0.98,
                    1.0,
                    "IAMF MP4 packaged",
                ));
                self.step = Step::Encode;
            }
            Awaiting::Metadata => {
                if let Some(e) = reply.error {
                    let message = format!("metadata write failed: {}", e.message);
                    self.push(log(Level::Warning, "engine", message));
                }
                self.metadata_written();
            }
        }
    }

    fn island(&mut self, awaiting: Awaiting, island: Island, range: Option<Range>) {
        self.awaiting = Some(awaiting);
        self.range = range;
        self.queue.push_back(Event::Island(island));
    }
}

impl Render {
    /// Runs the current step; an event it returns directly bypasses the queue (the offline
    /// render's events).
    fn advance(&mut self, cancelled: bool, host: &dyn Host) -> Option<Event> {
        if let Some(pending) = self.pending.take() {
            return self.begin_pass(pending, cancelled);
        }
        if let Some((pass, passing)) = self.pass.as_mut() {
            let then = passing.run_then;
            match pass.next(cancelled) {
                Ok(PassStep::Event(e)) => return Some(e),
                Ok(PassStep::Done) => {
                    self.pass = None;
                    self.step = then;
                }
                Err(e) => {
                    self.pass = None;
                    self.fail(e, Level::Error);
                }
            }
            return None;
        }
        match self.step {
            Step::Validate => self.validate(),
            Step::Import => self.import(),
            Step::ImportCancel => {
                if cancelled {
                    // Before the report: the cancellation result carries none.
                    self.cancelled();
                } else {
                    self.step = Step::Resolve;
                    self.resolve();
                }
            }
            Step::Resolve => self.resolve(),
            Step::Matrix => self.matrix(),
            Step::Report => self.report_and_target(),
            Step::Probe => {
                self.island(Awaiting::Packager, Island::ProbeMp4Packager, None);
            }
            Step::Target => self.target(),
            Step::Plan => self.plan(),
            Step::Prepare => self.prepare(host),
            Step::Prepared => {
                if cancelled {
                    self.cancelled();
                } else {
                    self.start_render();
                }
            }
            Step::Render => return self.render_step(cancelled, host),
            Step::Rendered => {
                if cancelled {
                    self.cancelled();
                } else {
                    self.measured();
                }
            }
            Step::Measured => self.trim_pass(cancelled),
            Step::TrimCancel => {
                if cancelled {
                    self.cancelled();
                } else {
                    self.gain();
                }
            }
            Step::GainCancel => {
                if cancelled {
                    self.cancelled();
                } else {
                    self.bitdepth(cancelled);
                }
            }
            Step::BitdepthCancel => {
                if cancelled {
                    self.cancelled();
                } else {
                    self.encode = 0;
                    self.step = Step::Encode;
                }
            }
            Step::Encode => self.encode(cancelled),
            Step::IamfEncode => {
                if cancelled {
                    self.cancelled();
                } else {
                    self.iamf_encode();
                }
            }
            Step::IamfPackage => {
                let src = self.iamf_path.clone();
                let dst = self.encoded.clone();
                self.island(Awaiting::Package, Island::PackageMp4 { src, dst }, None);
            }
            Step::Install => {
                if cancelled {
                    self.cancelled();
                } else {
                    self.install();
                }
            }
            Step::Finalize => self.finalize(cancelled),
            Step::Metadata => self.metadata(),
            Step::Finish => self.finish(),
        }
        None
    }

    fn validate(&mut self) {
        self.push(progress(
            stage::VALIDATING,
            operation::VALIDATE_REQUEST,
            0.,
            0.,
            "validating request",
        ));
        let invalid = |m: &str| Error::new(code::INVALID_ARGUMENT, m, "");
        if self.r.input.utf8.is_empty() && self.r.input.display.is_empty() {
            return self.fail_silent(invalid("input path is required"));
        }
        if self.r.triple_balance_mode > 1 {
            return self.fail_silent(invalid("invalid Triple Balance mode"));
        }
        if self.r.triple_balance_mode == 1 && self.r.renderer != Selection::TripleBalance as u32 {
            return self.fail_silent(invalid("D mode requires the Triple Balance renderer"));
        }
        let message = format!("render request: {}", self.r.input.display);
        self.push(log(Level::Info, "engine", message));
        if self.r.peak_normalize_to_limit && !self.r.peak_limit {
            return self.fail(
                invalid("peak normalization to limit requires peak limiting to be enabled"),
                Level::Error,
            );
        }
        if !self.r.final_gain_db.is_finite() {
            return self.fail(invalid("final gain must be finite"), Level::Error);
        }
        self.push(progress(
            stage::PROBING,
            operation::PROBE_INPUT,
            0.03,
            0.,
            "probing input",
        ));
        self.push(progress(
            stage::IMPORTING_SCENE,
            operation::IMPORT_SCENE,
            0.06,
            0.,
            if self.preimported {
                "using cached scene"
            } else {
                "importing scene"
            },
        ));
        self.step = Step::Import;
    }

    fn import(&mut self) {
        if !self.preimported {
            let options = ImportOptions {
                input_layout: self.r.input_layout.as_deref(),
                input_channel_labels: &self.r.input_channel_labels,
            };
            match mradm_scene::import(self.r.input.utf8.as_bytes(), options) {
                Ok(scene) => {
                    let _ = mradm_scene::checkpoint::dump("05-import", &scene);
                    self.scene = Some(scene);
                }
                Err(e) => {
                    let e = scene_error(&e, &self.r.input.display, "");
                    return self.fail(e, Level::Error);
                }
            }
        }
        let scene = self.scene.as_ref().expect("imported");
        let s = &scene.snapshot;
        let message = format!(
            "scene: {} programmes, {} objects, {} ch @ {}Hz",
            s.programmes.len(),
            s.objects.len(),
            scene.info.channels,
            scene.info.sample_rate
        );
        let warnings: Vec<String> = s
            .warnings
            .iter()
            .map(|&i| s.strings[i as usize].clone())
            .collect();
        self.push(log(Level::Info, "engine", message));
        if !self.preimported {
            for w in warnings {
                self.push(log(Level::Warning, "importer", w));
            }
        }
        self.step = Step::ImportCancel;
    }

    fn resolve(&mut self) {
        let scene = self.scene.as_ref().expect("imported");
        let trim = request::trim_window(
            self.r.render_start_sec,
            self.r.render_end_sec,
            scene.info.sample_rate,
            scene.info.frames,
        );
        match trim.result {
            Ok(t) => {
                self.trim = t;
                self.push_logs(trim.logs, "engine");
            }
            Err(e) => return self.fail(e, Level::Error),
        }
        self.output = match &self.r.output {
            Some(o) if !(o.utf8.is_empty() && o.display.is_empty()) => o.clone(),
            _ => {
                let input = &self.r.input.utf8;
                let path = join(parent(input), &format!("{}_rendered.wav", stem(input)));
                PathText {
                    utf8: path.clone(),
                    display: path,
                }
            }
        };
        let requested = request::normalize_output_layout(if self.r.output_layout.is_empty() {
            "0+2+0"
        } else {
            &self.r.output_layout
        });
        let Some(selection) = Selection::from_u32(self.r.renderer) else {
            return self.fail(
                Error::new(6, "invalid renderer selection", ""),
                Level::Error,
            );
        };
        let resolved = match request::resolve_renderer(
            selection,
            &requested,
            self.r.internal_allow_speaker_stereo,
            self.platform.apple.is_some(),
        ) {
            Ok(r) => r,
            Err(e) => return self.fail(e, Level::Error),
        };
        self.sel = resolved.selected;
        self.backend = resolved.backend;
        self.layout = resolved.effective_output_layout;
        let routing = DsRouting::from_u32(self.r.routing).unwrap_or(DsRouting::Automatic);
        if let Err(e) =
            request::validate_direct_speakers_routing(self.backend, &self.layout, routing)
        {
            return self.fail(e, Level::Error);
        }
        let Some(caps) = capabilities(self.backend, &self.platform) else {
            return self.fail(Error::new(6, "unknown backend", ""), Level::Error);
        };
        self.caps = caps;
        let message = format!("backend: {} {}", self.caps.name, self.caps.version);
        self.push(log(Level::Info, "engine", message));
        for (level, message) in resolved.diagnostics {
            self.push(log(level, "engine", message));
        }
        if self.r.sofa.is_some() {
            if self.layout != "binaural" {
                let context = format!("layout={}", self.layout);
                return self.fail(
                    Error::new(
                        code::INVALID_ARGUMENT,
                        "--sofa is valid only for binaural output",
                        context,
                    ),
                    Level::Error,
                );
            }
            if !self.caps.hrtf_sources.iter().any(|s| s == "user-sofa") {
                let message = format!(
                    "renderer '{}' does not support user SOFA HRTFs",
                    renderer_name(self.sel)
                );
                return self.fail(Error::new(code::UNSUPPORTED, message, ""), Level::Error);
            }
        }
        self.needs_report = !self.preimported
            && (self.r.semantic_report_path.is_some() || self.r.capture_semantic_report);
        if !self.preimported {
            self.push(progress(
                stage::IMPORTING_SCENE,
                operation::APPLY_SEMANTIC_POLICY,
                0.10,
                0.,
                "applying semantic policy",
            ));
            let scene = self.scene.as_mut().expect("imported");
            let applied = apply_policy(
                scene,
                self.r.semantic_policy_json.as_deref(),
                self.r.semantic_policy_path.as_ref(),
                &self.r.input.display,
            );
            let failed = applied.result.err();
            self.push_logs(applied.logs, "semantic-policy");
            if let Some(e) = failed {
                return self.fail(e, Level::Error);
            }
            let _ =
                mradm_scene::checkpoint::dump("06-policy", self.scene.as_ref().expect("imported"));
        }
        // From here every failure carries the semantic report once captured.
        self.report_ready = true;
        self.step = Step::Matrix;
    }

    fn matrix(&mut self) {
        let routing = DsRouting::from_u32(self.r.routing).unwrap_or(DsRouting::Automatic);
        let label = self
            .r
            .matrix_path
            .as_ref()
            .map_or(String::new(), |p| p.display.clone());
        let source = request::MatrixSource {
            json: self.r.matrix_json.as_deref(),
            path: self.r.matrix_path.as_ref().map(|p| p.utf8.as_str()),
            path_label: &label,
        };
        let resolved = request::resolve_direct_speakers_matrix(
            routing,
            source,
            self.scene.as_ref().expect("imported"),
            &self.layout,
        );
        self.push_logs(resolved.logs, "direct-speakers-matrix");
        match resolved.result {
            Ok(m) => self.matrix = m,
            Err(e) => return self.fail(e, Level::Error),
        }
        let invalid = |m: String, c: String| Error::new(code::INVALID_ARGUMENT, m, c);
        let speaker_output = self.layout != "0+2+0" && self.layout != "binaural";
        if self.r.apple_spatial_preset != 0 {
            let name = preset_name(self.r.apple_spatial_preset);
            if self.sel != Selection::Apple {
                return self.fail(
                    invalid(
                        format!("--apple-spatial-preset {name} requires --renderer apple"),
                        String::new(),
                    ),
                    Level::Error,
                );
            }
            if speaker_output {
                let message = format!(
                    "--apple-spatial-preset {name} requires Apple binaural output; got '{}'",
                    self.layout
                );
                return self.fail(invalid(message, String::new()), Level::Error);
            }
        }
        if self.r.apple_speaker_rendering_flags {
            if self.sel != Selection::Apple {
                return self.fail(
                    invalid(
                        "--apple-speaker-rendering-flags requires --renderer apple".into(),
                        String::new(),
                    ),
                    Level::Error,
                );
            }
            if !speaker_output {
                let context = format!("layout={}", self.layout);
                return self.fail(
                    invalid(
                        "--apple-speaker-rendering-flags requires Apple speaker output".into(),
                        context,
                    ),
                    Level::Error,
                );
            }
        }
        if self.needs_report
            && let Err(e) = self.emit_report("")
        {
            return self.fail(e, Level::Error);
        }
        self.step = Step::Report;
    }

    /// `emit_semantic_report`: the in-memory copy and the file are the same bytes.
    fn emit_report(&mut self, renderer_effective: &str) -> Result<(), Error> {
        let Some(scene) = self.scene.as_ref().filter(|_| !self.preimported) else {
            return Ok(());
        };
        let options = report::ReportOptions {
            renderer: renderer_name(self.sel).into(),
            policy_path: self
                .r
                .semantic_policy_path
                .as_ref()
                .map_or(String::new(), |p| p.display.clone()),
            supports_channel_lock: self.caps.supports_channel_lock,
            supports_object_divergence: self.caps.supports_object_divergence,
            supports_diffuse: self.caps.supports_diffuse,
            hrtf_sources: self.caps.hrtf_sources.clone(),
            renderer_effective: renderer_effective.into(),
        };
        let text = report::report(scene, &options)
            .map_err(|e| Error::new(code_of(e.code), cut(&e.message, 1024), ""))?;
        if self.r.capture_semantic_report {
            self.report = Some(text.clone());
        }
        if let Some(path) = &self.r.semantic_report_path {
            write_policy_json(path, &text, "report")?;
        }
        Ok(())
    }

    fn report_and_target(&mut self) {
        self.final_ext = extension(&self.output.utf8).to_ascii_lowercase();
        if self.r.iamf_mp4 && self.platform.iamf {
            self.step = Step::Probe;
        } else {
            self.step = Step::Target;
        }
    }

    fn target(&mut self) {
        let channels = channel_count(&self.caps, &self.layout);
        let spelled = extension(&self.output.display).to_owned();
        let bit_depth = match self.r.output_bit_depth {
            1 => BitDepth::I24,
            2 => BitDepth::I16,
            _ => BitDepth::F32,
        };
        let outcome = request::output_target(&request::OutputRequest {
            extension: &spelled,
            layout: &self.layout,
            bit_depth,
            apac_caf: self.r.apac_caf,
            iamf_mp4: self.r.iamf_mp4,
            iamf_layers: &self.r.iamf_layers,
            channels,
            iamf_available: self.platform.iamf,
            packager: self.packager,
            ffmpeg_major: self.ffmpeg_major,
        });
        self.push_logs(outcome.logs, "engine");
        match outcome.result {
            Ok(t) => self.target = t,
            Err(e) => return self.fail(e, Level::Error),
        }
        let lossy = self.target.lossy();
        let final_path = self.output.utf8.clone();
        if lossy {
            let render_temp = sidecar(&final_path, "render_tmp", ".wav");
            self.render_path = PathText::built(&render_temp);
            self.render_temp = Some(Temp::new(render_temp));
            let ext = extension(&final_path);
            let output_temp = sidecar(
                &final_path,
                "output_tmp",
                if ext.is_empty() { ".tmp" } else { ext },
            );
            self.encoded = PathText::built(&output_temp);
            self.output_temp = Some(Temp::new(output_temp));
        } else {
            self.render_path = self.output.clone();
            self.encoded = self.output.clone();
        }
        self.output_guard = Some(OutputGuard {
            path: PathBuf::from(&final_path),
            active: false,
        });
        if self.target.iamf_mp4 {
            let r = &self.render_path.utf8;
            let iamf = PathBuf::from(join(parent(r), &format!("{}.iamf", stem(r))));
            self.iamf_path = PathText::built(&iamf);
            self.iamf_temp = Some(Temp::new(iamf));
        }
        self.push(progress(
            stage::PLANNING,
            operation::PLAN_RENDER,
            0.13,
            0.,
            "planning render",
        ));
        let s = &self.r.screens;
        if (s.has_reference || s.has_reproduction) && self.sel != Selection::RoomBinaural {
            return self.fail(
                Error::new(code::UNSUPPORTED, "屏幕配置当前仅支持 room-binaural", ""),
                Level::Error,
            );
        }
        self.window_render = self.trim.is_some() && self.caps.supports_render_window;
        self.step = Step::Plan;
    }

    fn windows(&self) -> (Option<Window>, Option<Window>) {
        match self.trim {
            Some(t) => {
                let w = Window {
                    start: t.start_frame,
                    count: t.frame_count,
                };
                if self.window_render {
                    (Some(w), None)
                } else {
                    (None, Some(w))
                }
            }
            None => (None, None),
        }
    }

    fn apple_plan(&self) -> ApplePlan {
        let (window, meter_window) = self.windows();
        ApplePlan {
            output_path: self.render_path.clone(),
            output_layout: self.layout.clone(),
            window,
            meter_window,
            matrix: self.matrix.clone(),
        }
    }

    fn plan(&mut self) {
        if self.backend == Selection::Apple {
            if self.apple_cached {
                self.step = Step::Prepared;
                return;
            }
            self.push(progress(
                stage::PLANNING,
                operation::PREPARE_BACKEND,
                0.15,
                0.,
                "preparing backend",
            ));
            let plan = self.apple_plan();
            return self.island(Awaiting::ApplePrepare, Island::ApplePrepare(plan), None);
        }
        if let Some(cached) = self.cached.clone() {
            self.prepared = Some(cached);
            self.step = Step::Prepared;
            return;
        }
        self.push(progress(
            stage::PLANNING,
            operation::PREPARE_BACKEND,
            0.15,
            0.,
            "preparing backend",
        ));
        self.step = Step::Prepare;
    }

    fn prepare(&mut self, host: &dyn Host) {
        let Some(kind) = kind_of(self.backend) else {
            return self.fail(Error::new(6, "unknown backend", ""), Level::Error);
        };
        let routes: Vec<mradm_render::routing::MatrixRoute> = self
            .matrix
            .as_ref()
            .map(|m| {
                m.routes
                    .iter()
                    .map(|r| mradm_render::routing::MatrixRoute {
                        source_label: r.source_label.as_bytes(),
                        mute: r.mute,
                        targets: r
                            .targets
                            .iter()
                            .map(|t| (t.label.as_bytes(), t.gain))
                            .collect(),
                    })
                    .collect()
            })
            .unwrap_or_default();
        let r = &self.r;
        let sofa = r.sofa.as_ref().map(|s| backend::Sofa {
            path: s.path.utf8.as_bytes(),
            file_name: &s.file_name,
        });
        let request = backend::Request {
            kind,
            layout_id: self.layout.as_bytes(),
            apple_geometry: r.apple_geometry,
            routing: match r.routing {
                1 => Routing::Label,
                2 => Routing::Position,
                3 => Routing::Matrix,
                _ => Routing::Automatic,
            },
            matrix: self.matrix.as_ref().map(|_| &routes[..]),
            speaker_spread: match r.speaker_spread {
                1 => SpeakerSpread::None,
                2 => SpeakerSpread::Mdap,
                _ => SpeakerSpread::Automatic,
            },
            binaural_spread: match r.binaural_spread {
                1 => BinauralSpreadMode::None,
                2 => BinauralSpreadMode::Cloud,
                3 => BinauralSpreadMode::SafSpreader,
                _ => BinauralSpreadMode::Automatic,
            },
            triple_balance_d: r.triple_balance_mode == 1,
            split_power: r.split_power,
            smoothing_frames: r.object_smoothing_frames,
            default_interp_ms: r.default_interp_ms,
            input_path: r.input.display.as_bytes(),
            sofa,
            screens: r.screens,
            room: backend::RoomRequest {
                package: r.room.package.clone(),
                hrir: &r.room.hrir,
                metres: r.room.metres,
                quality: r.room.quality,
            },
        };
        let preparation = backend::prepare(self.scene.as_ref().expect("imported"), &request, host);
        self.push_logs(preparation.outcome.logs, kind.module());
        match preparation.outcome.result {
            Ok(p) => self.prepared_done(preparation.report, None, Some(p)),
            Err(e) => self.prepared_done(preparation.report, Some(e), None),
        }
    }
}

impl Render {
    fn prepared_done(
        &mut self,
        backend_report: Option<String>,
        error: Option<Error>,
        prepared: Option<Arc<Prepared>>,
    ) {
        // A rejected preparation still reports the backend's interpretation.
        if let Some(r) = backend_report.filter(|r| !r.is_empty() && self.needs_report)
            && let Err(e) = self.emit_report(&r)
        {
            return self.fail(e, Level::Error);
        }
        if let Some(e) = error {
            return self.fail(e, Level::Error);
        }
        if let Some(p) = prepared {
            self.new_prepared = Some(Arc::clone(&p));
            self.prepared = Some(p);
        }
        self.step = Step::Prepared;
    }

    fn render_range() -> Range {
        Range::new(
            stage::RENDERING,
            operation::RENDER_AUDIO,
            0.15,
            0.88,
            "rendering audio",
        )
    }

    fn start_render(&mut self) {
        if self.backend == Selection::Apple {
            let plan = self.apple_plan();
            return self.island(
                Awaiting::AppleRender,
                Island::AppleRender(plan),
                Some(Self::render_range()),
            );
        }
        let (window, meter_window) = self.windows();
        let scene = self.scene.as_ref().expect("imported");
        let prepared = Arc::clone(self.prepared.as_ref().expect("prepared"));
        self.run = Some(Run::new(
            prepared,
            &offline::Settings {
                input_path: self.r.input.utf8.as_bytes(),
                output_path: self.render_path.utf8.as_bytes(),
                input_display: self.r.input.display.as_bytes(),
                output_display: self.render_path.display.as_bytes(),
                output_layout: self.layout.as_bytes(),
                inputs: usize::from(scene.info.channels),
                frames: scene.info.frames,
                sample_rate: scene.info.sample_rate,
                default_interp_ms: self.r.default_interp_ms,
                smoothing_frames: self.r.object_smoothing_frames,
                window,
                meter_window,
                pose: self.r.listener_orientation,
            },
        ));
        self.range = Some(Self::render_range());
        self.step = Step::Render;
    }

    fn render_step(&mut self, cancelled: bool, host: &dyn Host) -> Option<Event> {
        let run = self.run.as_mut().expect("render started");
        let range = Self::render_range();
        match run.next(cancelled, host) {
            Ok(RunEvent::Progress(p)) => Some(Event::Progress(range.map(&p))),
            Ok(RunEvent::Log(l)) => Some(Event::Log(l)),
            Ok(RunEvent::Continue) => Some(Event::Continue),
            Ok(RunEvent::Done(m)) => {
                self.run = None;
                self.range = None;
                self.rendered(Ok(m));
                None
            }
            Err(e) => {
                self.run = None;
                self.range = None;
                self.rendered(Err(e));
                None
            }
        }
    }

    fn rendered(&mut self, result: Result<Metrics, Error>) {
        let lossy = self.target.lossy();
        match result {
            Err(e) => {
                if !lossy && let Some(g) = self.output_guard.as_mut() {
                    g.active = true;
                    g.remove_now();
                }
                let level = if e.code == CANCELLED {
                    Level::Info
                } else {
                    Level::Error
                };
                self.fail(e, level);
            }
            Ok(m) => {
                self.metrics = m;
                if !lossy && let Some(g) = self.output_guard.as_mut() {
                    g.active = true;
                }
                self.step = Step::Rendered;
            }
        }
    }

    fn measured(&mut self) {
        if let Some(l) = self.metrics.lufs {
            let message = format!("measured loudness: {} LUFS", fixed(l, 1, false));
            self.push(log(Level::Info, "engine", message));
        }
        if let Some(p) = self.metrics.peak_dbtp {
            let message = format!("measured true peak: {} dBTP", fixed(p, 2, false));
            self.push(log(Level::Info, "engine", message));
        }
        self.step = Step::Measured;
    }

    fn trim_pass(&mut self, _cancelled: bool) {
        match self.trim.filter(|_| !self.window_render) {
            Some(t) => {
                self.pending = Some(PendingPass {
                    kind: PassKind::Trim {
                        start: t.start_frame,
                        count: t.frame_count,
                    },
                    path: self.render_path.clone(),
                    range: Range::new(
                        stage::POST_PROCESSING,
                        operation::TRIM_OUTPUT,
                        0.88,
                        0.885,
                        "trimming output",
                    ),
                    then: Step::TrimCancel,
                });
            }
            None => self.step = Step::TrimCancel,
        }
    }

    /// Begins a pending pass (`trim_file_frames`, `apply_gain_to_file`, `downconvert_to_int`,
    /// `finalize_rendered_wav`) with its C++ cancellation check.
    fn begin_pass(&mut self, p: PendingPass, cancelled: bool) -> Option<Event> {
        let path = p.path.utf8.as_str();
        let layout = self.layout.as_bytes();
        let precheck = matches!(p.kind, PassKind::Trim { .. } | PassKind::Bitdepth(_));
        if precheck && cancelled {
            self.fail(
                cancelled_error(format!("path={}", p.path.display)),
                Level::Error,
            );
            return None;
        }
        let (begin, op) = match p.kind {
            PassKind::Trim { start, count } => (
                postprocess::trim(path, start, count, layout),
                operation::TRIM_OUTPUT,
            ),
            PassKind::Gain(g) => (
                postprocess::gain(path, g, layout, cancelled),
                operation::APPLY_GAIN,
            ),
            PassKind::Bitdepth(bits) => (
                postprocess::bitdepth(path, bits),
                operation::CONVERT_BIT_DEPTH,
            ),
            PassKind::Finalize(profile) => (
                postprocess::finalize_rendered(path, &self.layout, profile, cancelled),
                operation::WRITE_METADATA,
            ),
        };
        match PassRun::start(begin, &p.path.display, p.range, op) {
            Ok((run, first)) => {
                self.pass = Some((run, Passing { run_then: p.then }));
                Some(first.unwrap_or(Event::Continue))
            }
            Err(e) => {
                self.fail(e, Level::Error);
                None
            }
        }
    }

    fn gain(&mut self) {
        let r = &self.r;
        let target_peak = f64::from(r.peak_limit_dbtp);
        let plan = post_gain::render_gain(&RenderGainRequest {
            measured_lufs: self.metrics.lufs,
            measured_peak_dbtp: self.metrics.peak_dbtp,
            loudness_target_lufs: f64::from(r.loudness_target_lufs),
            peak_limit_dbtp: target_peak,
            final_gain_db: r.final_gain_db,
            normalize_loudness: r.measure_loudness,
            peak_normalize_to_limit: r.peak_normalize_to_limit,
            peak_limit: r.peak_limit,
        });
        let mut lines = Vec::new();
        match plan.loudness {
            Stage::Applied => lines.push(format!(
                "loudness target {} LUFS → gain {} dB",
                fixed(f64::from(r.loudness_target_lufs), 1, false),
                fixed(plan.loudness_db, 2, false)
            )),
            Stage::Unchanged => {
                lines.push("integrated loudness within 0.1 LU of target — no adjustment".into())
            }
            Stage::Skipped => {}
        }
        match plan.makeup {
            Stage::Applied => lines.push(format!(
                "true peak after loudness {} dBTP, target {} dBTP -> makeup {} dB",
                fixed(plan.makeup_peak_after, 2, false),
                fixed(target_peak, 1, false),
                fixed(plan.makeup_db, 2, false)
            )),
            Stage::Unchanged => {
                lines.push("true peak already at/above normalization target; no makeup".into())
            }
            Stage::Skipped => {}
        }
        match plan.ceiling {
            Stage::Applied => lines.push(format!(
                "true peak after loudness {} dBTP, ceiling {} dBTP → clamp {} dB",
                fixed(plan.ceiling_peak_after, 2, false),
                fixed(target_peak, 1, false),
                fixed(plan.ceiling_db, 2, false)
            )),
            Stage::Unchanged => lines.push("true peak within target — no clamp".into()),
            Stage::Skipped => {}
        }
        if plan.final_applied {
            lines.push(format!(
                "final gain {} dB (unconstrained, bypasses peak limit)",
                fixed(r.final_gain_db, 2, true)
            ));
        }
        self.gain_db = plan.gain_db;
        if plan.apply {
            lines.push(format!(
                "applying total gain {} ({} dB)",
                fixed(f64::from(plan.gain), 4, false),
                fixed(plan.gain_db, 2, false)
            ));
        }
        for line in lines {
            self.push(log(Level::Info, "engine", line));
        }
        if plan.apply {
            self.pending = Some(PendingPass {
                kind: PassKind::Gain(plan.gain),
                path: self.render_path.clone(),
                range: Range::new(
                    stage::POST_PROCESSING,
                    operation::APPLY_GAIN,
                    0.885,
                    0.89,
                    "applying gain",
                ),
                then: Step::GainCancel,
            });
        } else {
            self.step = Step::GainCancel;
        }
    }

    fn bitdepth_range() -> Range {
        Range::new(
            stage::POST_PROCESSING,
            operation::CONVERT_BIT_DEPTH,
            0.89,
            0.90,
            "converting bit depth",
        )
    }

    fn bitdepth(&mut self, _cancelled: bool) {
        let depth = match self.r.output_bit_depth {
            1 => 24,
            2 => 16,
            _ => 0,
        };
        if self.target.lossy() || depth == 0 {
            self.step = Step::BitdepthCancel;
            return;
        }
        self.push(log(
            Level::Info,
            "engine",
            format!("converting to {depth}-bit integer PCM"),
        ));
        self.pending = Some(PendingPass {
            kind: PassKind::Bitdepth(depth),
            path: self.render_path.clone(),
            range: Self::bitdepth_range(),
            then: Step::BitdepthCancel,
        });
    }

    fn adjusted(&self) -> (Option<f64>, Option<f64>) {
        (
            self.metrics.lufs.map(|v| v + self.gain_db),
            self.metrics.peak_dbtp.map(|v| v + self.gain_db),
        )
    }

    fn encode(&mut self, _cancelled: bool) {
        loop {
            let stage = self.encode;
            self.encode += 1;
            let t = self.target.clone();
            match stage {
                0 if t.flac => {
                    self.push(log(
                        Level::Info,
                        "engine",
                        "encoding float32 render to FLAC (24-bit)",
                    ));
                    let island = Island::EncodeFlac {
                        src: self.render_path.clone(),
                        dst: self.encoded.clone(),
                    };
                    let range = Range::new(
                        stage::POST_PROCESSING,
                        operation::ENCODE_FLAC,
                        0.90,
                        0.98,
                        "encoding FLAC",
                    );
                    return self.island(Awaiting::Flac, island, Some(range));
                }
                1 if t.opus => {
                    self.push(log(
                        Level::Info,
                        "engine",
                        "encoding float32 render to Opus MKA (VBR)",
                    ));
                    self.push(progress(
                        stage::POST_PROCESSING,
                        operation::ENCODE_OPUS,
                        0.90,
                        0.0,
                        "encoding Opus",
                    ));
                    let island = Island::EncodeOpus {
                        src: self.render_path.clone(),
                        dst: self.encoded.clone(),
                        layout: self.layout.clone(),
                        bitrate: self.r.opus_bitrate_per_ch_kbps,
                    };
                    return self.island(Awaiting::Opus, island, None);
                }
                2 if t.apac() => {
                    let message = format!("encoding float32 render to APAC ({})", self.final_ext);
                    self.push(log(Level::Info, "engine", message));
                    let island = Island::EncodeApac {
                        src: self.render_path.clone(),
                        dst: self.encoded.clone(),
                        layout: self.layout.clone(),
                        bitrate: self.r.apac_bitrate_kbps,
                        drc_music: self.r.apac_drc_music,
                        caf: t.apac_caf,
                    };
                    let range = Range::new(
                        stage::POST_PROCESSING,
                        operation::ENCODE_APAC,
                        0.90,
                        0.98,
                        "encoding APAC",
                    );
                    return self.island(Awaiting::Apac, island, Some(range));
                }
                3 | 4 if (stage == 3 && t.iamf) || (stage == 4 && t.iamf_mp4) => {
                    let message = if stage == 3 {
                        "encoding 32-bit PCM staging to IAMF (Opus, raw OBU stream)"
                    } else {
                        "encoding 32-bit PCM staging to IAMF (Opus) and packaging to MP4"
                    };
                    self.push(log(Level::Info, "engine", message));
                    self.pending = Some(PendingPass {
                        kind: PassKind::Bitdepth(32),
                        path: self.render_path.clone(),
                        range: Self::bitdepth_range(),
                        then: Step::IamfEncode,
                    });
                    return;
                }
                0..=4 => {}
                _ => {
                    self.step = Step::Install;
                    return;
                }
            }
        }
    }

    fn iamf_encode(&mut self) {
        self.push(progress(
            stage::POST_PROCESSING,
            operation::ENCODE_IAMF,
            0.90,
            0.0,
            "encoding IAMF",
        ));
        let (lufs, peak_dbtp) = self.adjusted();
        let island = Island::EncodeIamf {
            src: self.render_path.clone(),
            dst: if self.target.iamf_mp4 {
                self.iamf_path.clone()
            } else {
                self.encoded.clone()
            },
            layout: self.layout.clone(),
            bitrate: self.r.opus_bitrate_per_ch_kbps,
            lufs,
            peak_dbtp,
            layers: self.target.iamf_layers.clone(),
        };
        self.island(Awaiting::Iamf, island, None);
    }

    fn install(&mut self) {
        if self.target.lossy() {
            let final_path = PathBuf::from(&self.output.utf8);
            let temp = self.output_temp.as_ref().expect("lossy").path.clone();
            if let Err(e) = replace_output(&temp, &final_path, &self.output.display) {
                return self.fail(e, Level::Error);
            }
            if let Some(t) = self.output_temp.as_mut() {
                t.dismiss();
            }
        }
        self.step = Step::Finalize;
    }

    fn finalize(&mut self, _cancelled: bool) {
        if self.final_ext != ".wav" {
            self.step = Step::Metadata;
            return;
        }
        let layout = self.layout.as_str();
        let rerender =
            self.sel == Selection::TripleBalance && (layout == "4+7+0" || layout == "9.1.6");
        let adm = matches!(layout, "binaural" | "4+5+4" | "9.1.6" | "9+10+3" | "hoa3");
        let mut lines = Vec::new();
        if adm && !rerender && self.r.output_bit_depth == 0 {
            lines.push((
                Level::Warning,
                "float32 spatial WAV stores exact ADM AXML/CHNA semantics in RF64; use --output-bit-depth i24 \
                 for normative PCM BW64 interoperability",
            ));
        }
        if rerender && layout == "9.1.6" {
            lines.push((
                Level::Info,
                "triple-balance 9.1.6 WAV: native order, WAVEFORMATEXTENSIBLE mask 0, no ADM; \
                 CAF provides an explicit CoreAudio 9.1.6 layout",
            ));
        }
        lines.push((
            Level::Info,
            "finalizing machine-readable WAV channel layout",
        ));
        for (level, line) in lines {
            self.push(log(level, "engine", line));
        }
        self.pending = Some(PendingPass {
            kind: PassKind::Finalize(if rerender {
                Profile::SpeakerRerender
            } else {
                Profile::AdmSemantics
            }),
            path: self.output.clone(),
            range: Range::new(
                stage::POST_PROCESSING,
                operation::WRITE_METADATA,
                0.98,
                0.99,
                "finalizing WAV channel layout",
            ),
            then: Step::Metadata,
        });
    }

    fn metadata(&mut self) {
        self.push(progress(
            stage::POST_PROCESSING,
            operation::WRITE_METADATA,
            0.99,
            0.0,
            "writing metadata",
        ));
        let (lufs, peak_dbtp) = self.adjusted();
        let fields = MetadataFields {
            encoder: ENCODER.into(),
            date_utc: utc_date(now()),
            renderer: self.caps.name.clone(),
            output_layout: self.layout.clone(),
            lufs,
            peak_dbtp,
        };
        let ext = self.final_ext.clone();
        match write_own_metadata(&ext, &self.output, &fields) {
            Some(result) => {
                if let Err(e) = result {
                    let message = format!("metadata write failed: {}", e.message);
                    self.push(log(Level::Warning, "engine", message));
                }
                self.metadata_written();
            }
            None if matches!(ext.as_str(), ".flac" | ".mka" | ".m4a" | ".mp4") => {
                let island = Island::WriteMetadata {
                    path: self.output.clone(),
                    fields,
                };
                self.island(Awaiting::Metadata, island, None);
            }
            None => self.metadata_written(),
        }
    }

    fn metadata_written(&mut self) {
        self.push(progress(
            stage::POST_PROCESSING,
            operation::WRITE_METADATA,
            0.995,
            1.0,
            "metadata written",
        ));
        if let Some(g) = self.output_guard.as_mut() {
            g.active = false;
        }
        self.step = Step::Finish;
    }

    fn finish(&mut self) {
        let (lufs, peak_dbtp) = self.adjusted();
        self.push(progress(stage::FINISHED, operation::FINISH, 1., 1., "done"));
        let output_path = Some(self.output.display.clone());
        let semantic_report = self.report.clone();
        self.finish_with(Outcome {
            error: None,
            output_path,
            metrics: Some(Metrics { lufs, peak_dbtp }),
            logs: vec![(Level::Info, "render completed".into())],
            semantic_report,
        });
    }
}
