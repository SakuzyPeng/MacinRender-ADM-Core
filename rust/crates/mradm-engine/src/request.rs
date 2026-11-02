//! Render request resolution (PCM consistency 5.2 B), moved from the C++ `renderer_factory` and
//! `RenderService::render` with the same order, messages and locators: the output layout aliases,
//! the backend selection, DirectSpeakers routing and matrix validation, the output time-range
//! trim and the output container decisions.
use crate::fmt::general;
use mradm_render::routing::{self, LfeTarget};
use mradm_scene::{RenderScene, direct_matrix};
use std::collections::{BTreeSet, HashMap};

/// The C++ `ErrorCode` values.
pub mod code {
    pub const INVALID_ARGUMENT: i32 = 1;
    pub const UNSUPPORTED: i32 = 2;
    pub const IO_ERROR: i32 = 3;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    pub code: i32,
    pub message: String,
    pub context: String,
}

impl Error {
    pub fn new(code: i32, message: impl Into<String>, context: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
            context: context.into(),
        }
    }
}

/// The C++ `LogLevel` values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Level {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Log {
    pub level: Level,
    pub module: &'static str,
    pub message: String,
}

/// A result with the log lines the caller emits first, in order, whether it succeeded or not.
#[derive(Clone, Debug, PartialEq)]
pub struct Outcome<T> {
    pub logs: Vec<Log>,
    pub result: Result<T, Error>,
}

impl<T> Outcome<T> {
    fn new(logs: Vec<Log>, result: Result<T, Error>) -> Self {
        Self { logs, result }
    }
}

/// The C++ `RendererSelection` values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Selection {
    Automatic = 0,
    Ear = 1,
    Saf = 2,
    Hoa = 3,
    Apple = 4,
    Binaural = 5,
    SafBinaural = 6,
    TripleBalance = 7,
    RoomBinaural = 8,
}

impl Selection {
    pub fn from_u32(value: u32) -> Option<Self> {
        use Selection::*;
        [
            Automatic,
            Ear,
            Saf,
            Hoa,
            Apple,
            Binaural,
            SafBinaural,
            TripleBalance,
            RoomBinaural,
        ]
        .get(value as usize)
        .copied()
    }
}

/// The C++ `DirectSpeakersRoutingMode` values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum DsRouting {
    Automatic = 0,
    Label = 1,
    Position = 2,
    Matrix = 3,
}

impl DsRouting {
    pub fn from_u32(value: u32) -> Option<Self> {
        use DsRouting::*;
        [Automatic, Label, Position, Matrix]
            .get(value as usize)
            .copied()
    }
}

/// The canonical id of a requested output layout (`5.1` -> `0+5+0`, ...), ASCII case-insensitive;
/// empty means binaural, and `None` means the value is not an alias and passes through unchanged.
pub fn canonical_layout(layout: &[u8]) -> Option<&'static str> {
    Some(match layout.to_ascii_lowercase().as_slice() {
        b"" | b"binaural" => "binaural",
        b"stereo" | b"2.0" | b"0+2+0" => "0+2+0",
        b"5.1" | b"0+5+0" => "0+5+0",
        b"5.1.2" | b"2+5+0" => "2+5+0",
        b"7.1" | b"wav71" | b"wave_7_1" | b"wave-7.1" | b"0+7+0" => "wav71",
        b"5.1.4" | b"atmos514" | b"4+5+0" => "4+5+0",
        b"9.1.4" | b"4+5+4" => "4+5+4",
        b"7.1.4" | b"atmos714" | b"4+7+0" => "4+7+0",
        b"9.1.6" | b"atmos916" => "9.1.6",
        b"22.2" | b"9+10+3" => "9+10+3",
        b"hoa3" => "hoa3",
        _ => return None,
    })
}

/// [`canonical_layout`], or the input itself.
pub fn normalize_output_layout(layout: &str) -> String {
    canonical_layout(layout.as_bytes()).map_or_else(|| layout.to_owned(), str::to_owned)
}

/// The backend serving a request. `selected` is the selection the caller reports (automatic
/// two-channel output is promoted to SAF binaural; other automatic requests and the legacy
/// `binaural` alias stay as requested); `backend` is the concrete backend.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Resolved {
    pub selected: Selection,
    pub backend: Selection,
    pub effective_output_layout: String,
    pub diagnostics: Vec<(Level, String)>,
}

/// Resolves a renderer for an already normalized output layout; `apple` says whether this build
/// has the Apple backend.
pub fn resolve_renderer(
    requested: Selection,
    layout: &str,
    allow_speaker_stereo: bool,
    apple: bool,
) -> Result<Resolved, Error> {
    let speaker_stereo = layout == "0+2+0";
    let mut selected = requested;
    if selected == Selection::Automatic && (speaker_stereo || layout == "binaural") {
        selected = Selection::SafBinaural;
    }
    if matches!(selected, Selection::Ear | Selection::Saf)
        && speaker_stereo
        && !allow_speaker_stereo
    {
        return Err(Error::new(
            code::UNSUPPORTED,
            "two-channel loudspeaker rendering is unavailable; use binaural output instead",
            "",
        ));
    }
    let backend = match selected {
        Selection::Ear | Selection::Automatic => Selection::Ear,
        Selection::Binaural | Selection::SafBinaural => Selection::SafBinaural,
        Selection::Apple if !apple => {
            return Err(Error::new(
                code::UNSUPPORTED,
                format!(
                    "renderer '{}' is not available in this build",
                    selected as u32
                ),
                "",
            ));
        }
        other => other,
    };
    let mut resolved = Resolved {
        selected,
        backend,
        effective_output_layout: layout.to_owned(),
        diagnostics: Vec::new(),
    };
    if selected == Selection::Binaural {
        resolved.diagnostics.push((
            Level::Warning,
            "--renderer binaural is a legacy alias for --renderer saf-binaural; prefer saf-binaural".into(),
        ));
    }
    if backend == Selection::SafBinaural {
        if layout != "0+2+0" && layout != "binaural" {
            return Err(Error::new(
                code::UNSUPPORTED,
                format!("SAF binaural backend does not support output layout '{layout}'"),
                format!("layout={layout}"),
            ));
        }
        resolved.effective_output_layout = "binaural".into();
    }
    Ok(resolved)
}

/// Validates an explicit DirectSpeakers routing request after backend and layout resolution.
pub fn validate_direct_speakers_routing(
    backend: Selection,
    layout: &str,
    mode: DsRouting,
) -> Result<(), Error> {
    if mode == DsRouting::Automatic {
        return Ok(());
    }
    let binaural = layout == "0+2+0" || layout == "binaural";
    if mode == DsRouting::Matrix {
        // EAR and SAF are loudspeaker backends; their internal 0+2+0 test layout is not binaural.
        if matches!(backend, Selection::Ear | Selection::Saf)
            || (backend == Selection::Apple && !binaural)
        {
            return Ok(());
        }
        return Err(Error::new(
            code::UNSUPPORTED,
            "DirectSpeakers matrix routing is supported only by EAR, SAF, and Apple speaker outputs",
            "",
        ));
    }
    match backend {
        Selection::Saf => Ok(()),
        Selection::Apple if binaural && mode == DsRouting::Label => Err(Error::new(
            code::UNSUPPORTED,
            "Apple binaural output does not support DirectSpeakers label routing; use position",
            "",
        )),
        Selection::Apple => Ok(()),
        _ => Err(Error::new(
            code::UNSUPPORTED,
            "explicit DirectSpeakers routing is supported only by SAF and Apple renderers",
            "",
        )),
    }
}

/// Where a DirectSpeakers matrix comes from. `path` is the UTF-8 file path; `path_label` is the
/// spelling the error context quotes.
#[derive(Clone, Copy, Debug, Default)]
pub struct MatrixSource<'a> {
    pub json: Option<&'a [u8]>,
    pub path: Option<&'a str>,
    pub path_label: &'a str,
}

fn load_matrix(source: MatrixSource<'_>) -> Result<direct_matrix::Matrix, Error> {
    let (bytes, label) = match (source.json, source.path) {
        (Some(json), _) => (json.to_vec(), "<memory>".to_owned()),
        (None, Some(path)) => {
            let io = |message: &str| {
                Error::new(
                    code::IO_ERROR,
                    format!("DirectSpeakers matrix I/O error: {message}"),
                    source.path_label,
                )
            };
            let mut file = std::fs::File::open(path).map_err(|_| io("cannot open file"))?;
            let mut bytes = Vec::new();
            std::io::Read::read_to_end(&mut file, &mut bytes)
                .map_err(|_| io("failed while reading file"))?;
            (bytes, source.path_label.to_owned())
        }
        (None, None) => unreachable!("checked by the caller"),
    };
    direct_matrix::parse(&bytes).map_err(|e| Error::new(e.code, e.message, label))
}

/// Resolves the path / JSON precedence and validates the matrix against the (policy-applied)
/// scene and the effective output layout. `Ok(None)` means no matrix was requested.
pub fn resolve_direct_speakers_matrix(
    mode: DsRouting,
    source: MatrixSource<'_>,
    scene: &RenderScene,
    effective_layout: &str,
) -> Outcome<Option<direct_matrix::Matrix>> {
    let mut logs = Vec::new();
    let result = resolve_matrix(mode, source, scene, effective_layout, &mut logs);
    Outcome::new(logs, result)
}

fn resolve_matrix(
    mode: DsRouting,
    source: MatrixSource<'_>,
    scene: &RenderScene,
    effective_layout: &str,
    logs: &mut Vec<Log>,
) -> Result<Option<direct_matrix::Matrix>, Error> {
    let configured = source.json.is_some() || source.path.is_some();
    if mode != DsRouting::Matrix {
        if configured {
            return Err(Error::new(
                code::INVALID_ARGUMENT,
                "DirectSpeakers matrix configuration requires routing mode 'matrix'",
                "",
            ));
        }
        return Ok(None);
    }
    if !configured {
        return Err(Error::new(
            code::INVALID_ARGUMENT,
            "DirectSpeakers matrix routing requires a matrix path or in-memory JSON",
            "",
        ));
    }
    let loaded = load_matrix(source);
    if source.json.is_some() && source.path.is_some() {
        logs.push(Log {
            level: Level::Warning,
            module: "direct-speakers-matrix",
            message:
                "in-memory DirectSpeakers matrix supplied; ignoring direct_speakers_matrix_path"
                    .into(),
        });
    }
    let matrix = loaded?;
    if normalize_output_layout(&matrix.output_layout) != effective_layout {
        return Err(Error::new(
            code::INVALID_ARGUMENT,
            format!(
                "DirectSpeakers matrix layout '{}' does not match output layout '{effective_layout}'",
                matrix.output_layout
            ),
            "",
        ));
    }
    let rows = route_rows(&matrix)?;
    scene_coverage(&matrix, &rows, scene, logs)?;
    Ok(Some(matrix))
}

fn route_rows(matrix: &direct_matrix::Matrix) -> Result<HashMap<Vec<u8>, usize>, Error> {
    let mut rows = HashMap::new();
    for (index, route) in matrix.routes.iter().enumerate() {
        let source = routing::direct_label(route.source_label.as_bytes());
        let context = format!("source_label={}", route.source_label);
        if source.is_empty() {
            return Err(Error::new(
                code::INVALID_ARGUMENT,
                "DirectSpeakers matrix source_label resolves to an empty label",
                context,
            ));
        }
        if routing::is_lfe(&source) {
            return Err(Error::new(
                code::INVALID_ARGUMENT,
                format!(
                    "DirectSpeakers matrix source '{}' is LFE; LFE uses dedicated routing",
                    route.source_label
                ),
                "",
            ));
        }
        if rows.insert(source, index).is_some() {
            return Err(Error::new(
                code::INVALID_ARGUMENT,
                format!(
                    "duplicate DirectSpeakers matrix source '{}' after alias resolution",
                    route.source_label
                ),
                "",
            ));
        }
        let mut targets = BTreeSet::new();
        for target in &route.targets {
            let key = routing::direct_label(target.label.as_bytes());
            if key.is_empty() {
                return Err(Error::new(
                    code::INVALID_ARGUMENT,
                    "DirectSpeakers matrix target label resolves to an empty label",
                    context,
                ));
            }
            if routing::is_lfe(&key) {
                return Err(Error::new(
                    code::INVALID_ARGUMENT,
                    format!(
                        "DirectSpeakers matrix target '{}' is LFE; LFE uses dedicated routing",
                        target.label
                    ),
                    context,
                ));
            }
            if !targets.insert(key) {
                return Err(Error::new(
                    code::INVALID_ARGUMENT,
                    format!(
                        "duplicate DirectSpeakers matrix target '{}' after alias resolution",
                        target.label
                    ),
                    context,
                ));
            }
        }
    }
    Ok(rows)
}

fn scene_coverage(
    matrix: &direct_matrix::Matrix,
    rows: &HashMap<Vec<u8>, usize>,
    scene: &RenderScene,
    logs: &mut Vec<Log>,
) -> Result<(), Error> {
    let s = &scene.snapshot;
    let mut used = vec![false; matrix.routes.len()];
    for object in &s.objects {
        for track in &s.tracks[object.tracks.offset..object.tracks.offset + object.tracks.len] {
            for block in &s.direct_blocks
                [track.ds_blocks.offset..track.ds_blocks.offset + track.ds_blocks.len]
            {
                let labels: Vec<&str> = s.indices
                    [block.labels.offset..block.labels.offset + block.labels.len]
                    .iter()
                    .map(|&id| s.strings[id as usize].as_str())
                    .collect();
                let refs: Vec<&[u8]> = labels.iter().map(|l| l.as_bytes()).collect();
                if routing::lfe_target(&refs, block.low_pass_present != 0) != LfeTarget::None {
                    continue;
                }
                let uid = &s.strings[track.uid as usize];
                let mut matched: Option<usize> = None;
                for label in &labels {
                    let Some(&row) = rows.get(&routing::direct_label(label.as_bytes())) else {
                        continue;
                    };
                    if matched.is_some_and(|m| m != row) {
                        return Err(Error::new(
                            code::INVALID_ARGUMENT,
                            "DirectSpeakers block matches multiple matrix source rows",
                            format!("track_uid={uid} labels={}", labels.join(",")),
                        ));
                    }
                    matched = Some(row);
                }
                let Some(row) = matched else {
                    let labels = if labels.is_empty() {
                        "<missing>".to_owned()
                    } else {
                        labels.join(",")
                    };
                    return Err(Error::new(
                        code::INVALID_ARGUMENT,
                        format!(
                            "DirectSpeakers block label '{labels}' is not covered by the routing matrix"
                        ),
                        format!("track_uid={uid}"),
                    ));
                };
                used[row] = true;
            }
        }
    }
    for (route, used) in matrix.routes.iter().zip(used) {
        if !used {
            logs.push(Log {
                level: Level::Warning,
                module: "direct-speakers-matrix",
                message: format!(
                    "matrix source '{}' is not present in this scene",
                    route.source_label
                ),
            });
        }
    }
    Ok(())
}

/// The output time-range trim: `Ok(None)` renders the whole timeline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Trim {
    pub start_frame: u64,
    pub frame_count: u64,
}

/// Resolves `--start` / `--end` against the input timeline (nearest frame, ties away from zero,
/// as `mradm-dsp` `timeline`); logs the kept frame range.
pub fn trim_window(
    start_sec: f64,
    end_sec: Option<f64>,
    sample_rate: u32,
    frames: u64,
) -> Outcome<Option<Trim>> {
    let fail = |message: String| {
        Outcome::new(
            Vec::new(),
            Err(Error::new(code::INVALID_ARGUMENT, message, "")),
        )
    };
    // NaN and negative starts must also enter validation when no end was supplied.
    if start_sec == 0.0 && end_sec.is_none() {
        return Outcome::new(Vec::new(), Ok(None));
    }
    if sample_rate == 0 {
        return fail("cannot apply time-range trim: input sample rate is unknown".into());
    }
    let Some(start_frame) = mradm_dsp::timeline::seconds_to_frame(start_sec, sample_rate) else {
        return fail(format!(
            "--start must be finite and >= 0 (got {}s)",
            general(start_sec, 6)
        ));
    };
    let end = end_sec.unwrap_or(0.0);
    let requested_end = match end_sec {
        Some(e) => mradm_dsp::timeline::seconds_to_frame(e, sample_rate),
        None => Some(frames),
    };
    let Some(requested_end) = requested_end else {
        return fail(format!(
            "--end must be finite and >= 0 (got {}s)",
            general(end, 6)
        ));
    };
    if end_sec.is_some() && end <= start_sec {
        return fail(format!(
            "--end ({}s) must be greater than --start ({}s)",
            general(end, 6),
            general(start_sec, 6)
        ));
    }
    let rate = f64::from(sample_rate);
    if start_frame >= frames {
        return fail(format!(
            "--start ({}s) is at or beyond the input duration ({}s)",
            general(start_sec, 6),
            general(frames as f64 / rate, 6)
        ));
    }
    let end_frame = requested_end.min(frames);
    if end_frame <= start_frame {
        return fail(format!(
            "trim window [{}s, {}s) is shorter than one sample frame at {sample_rate}Hz",
            general(start_sec, 6),
            general(end, 6)
        ));
    }
    let count = end_frame - start_frame;
    let log = Log {
        level: Level::Info,
        module: "engine",
        message: format!(
            "output trim: frames [{start_frame}, {}) of {frames} @ {sample_rate}Hz",
            start_frame + count
        ),
    };
    Outcome::new(
        vec![log],
        Ok(Some(Trim {
            start_frame,
            frame_count: count,
        })),
    )
}

/// The C++ `OutputBitDepth` values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum BitDepth {
    F32 = 0,
    I24 = 1,
    I16 = 2,
}

/// The MP4 packager in PATH (`audio::IamfMp4PackagerKind`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum Packager {
    #[default]
    None = 0,
    Mp4box = 1,
    Ffmpeg = 2,
}

/// The output request and the platform facts its validation reads.
#[derive(Clone, Debug)]
pub struct OutputRequest<'a> {
    /// The final output path's extension, as spelled (`.FLAC`, `""`, ...).
    pub extension: &'a str,
    pub layout: &'a str,
    pub bit_depth: BitDepth,
    pub apac_caf: bool,
    pub iamf_mp4: bool,
    pub iamf_layers: &'a [String],
    /// The backend's channel count for the layout, when it lists it.
    pub channels: Option<u16>,
    pub iamf_available: bool,
    /// Probed only for `--iamf-container mp4` in IAMF builds.
    pub packager: Packager,
    pub ffmpeg_major: i32,
}

/// What the final output needs.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct OutputTarget {
    pub flac: bool,
    pub opus: bool,
    pub apac_mpeg4: bool,
    pub apac_caf: bool,
    pub iamf: bool,
    pub iamf_mp4: bool,
    pub iamf_layers: Vec<String>,
}

impl OutputTarget {
    pub fn apac(&self) -> bool {
        self.apac_mpeg4 || self.apac_caf
    }
    /// Every encoded final output renders to a temporary float WAVE first.
    pub fn lossy(&self) -> bool {
        self.flac || self.opus || self.apac() || self.iamf || self.iamf_mp4
    }
}

struct IamfLayer {
    id: &'static str,
    display: &'static str,
    bed: u32,
    lfe: u32,
    height: u32,
}

const IAMF_LAYERS: [IamfLayer; 5] = [
    IamfLayer {
        id: "0+5+0",
        display: "5.1",
        bed: 5,
        lfe: 1,
        height: 0,
    },
    IamfLayer {
        id: "2+5+0",
        display: "5.1.2",
        bed: 5,
        lfe: 1,
        height: 2,
    },
    IamfLayer {
        id: "4+5+0",
        display: "5.1.4",
        bed: 5,
        lfe: 1,
        height: 4,
    },
    IamfLayer {
        id: "wav71",
        display: "7.1",
        bed: 7,
        lfe: 1,
        height: 0,
    },
    IamfLayer {
        id: "4+7+0",
        display: "7.1.4",
        bed: 7,
        lfe: 1,
        height: 4,
    },
];

const IAMF_SUPPORTED: &str = "supported: 5.1, 5.1.2, 5.1.4, 7.1, 7.1.4";

fn iamf_layer(id: &str) -> Option<&'static IamfLayer> {
    IAMF_LAYERS.iter().find(|l| l.id == id)
}

fn invalid(message: impl Into<String>) -> Error {
    Error::new(code::INVALID_ARGUMENT, message, "")
}

fn unsupported(message: impl Into<String>) -> Error {
    Error::new(code::UNSUPPORTED, message, "")
}

fn iamf_scalable_layers(
    raw: &[String],
    layout: &str,
    logs: &mut Vec<Log>,
) -> Result<Vec<String>, Error> {
    if raw.is_empty() {
        return Ok(Vec::new());
    }
    if raw.len() > 7 {
        return Err(invalid("IAMF scalable layers support at most 7 layers"));
    }
    let Some(last) = iamf_layer(layout) else {
        return Err(invalid(format!(
            "IAMF scalable layers are not supported for output layout '{layout}'; {IAMF_SUPPORTED}"
        )));
    };
    let mut ids = Vec::with_capacity(raw.len());
    let mut infos = Vec::with_capacity(raw.len());
    for value in raw {
        let id = normalize_output_layout(value);
        let Some(info) = iamf_layer(&id) else {
            return Err(invalid(format!(
                "IAMF scalable layer '{value}' is not supported; {IAMF_SUPPORTED}"
            )));
        };
        ids.push(id);
        infos.push(info);
    }
    if ids.last().map(String::as_str) != Some(layout) {
        return Err(invalid(format!(
            "last IAMF scalable layer must match output layout '{}'; got '{}'",
            last.display,
            infos.last().unwrap().display
        )));
    }
    for pair in infos.windows(2) {
        let (prev, cur) = (pair[0], pair[1]);
        if cur.bed < prev.bed || cur.lfe < prev.lfe || cur.height < prev.height {
            return Err(invalid(format!(
                "IAMF scalable layers must be monotonic; '{}' cannot follow '{}'",
                cur.display, prev.display
            )));
        }
    }
    if last.height > 0 {
        for info in infos.iter().filter(|i| i.height == 0) {
            logs.push(Log {
                level: Level::Warning,
                module: "engine",
                message: format!(
                    "IAMF scalable layers include flat layer '{}' for height output '{}'; \
                     official guidance recommends height layouts, continuing",
                    info.display, last.display
                ),
            });
        }
    }
    Ok(ids)
}

fn flac_supports_layout(layout: &str) -> bool {
    matches!(layout, "binaural" | "0+2+0" | "0+5+0" | "wav71")
}

/// The final output's container decisions and their checks, in the C++ order.
pub fn output_target(request: &OutputRequest<'_>) -> Outcome<OutputTarget> {
    let mut logs = Vec::new();
    let result = target(request, &mut logs);
    Outcome::new(logs, result)
}

fn target(r: &OutputRequest<'_>, logs: &mut Vec<Log>) -> Result<OutputTarget, Error> {
    let ext = r.extension.to_ascii_lowercase();
    let mut t = OutputTarget {
        flac: ext == ".flac",
        opus: ext == ".mka",
        ..OutputTarget::default()
    };
    if r.apac_caf && ext != ".caf" {
        let shown = if ext.is_empty() { "<none>" } else { &ext };
        return Err(invalid(format!(
            "--apac-container caf requires output path with .caf extension; got '{shown}'"
        )));
    }
    t.apac_mpeg4 = (ext == ".m4a" || ext == ".mp4") && !r.iamf_mp4 && !r.apac_caf;
    t.apac_caf = r.apac_caf && ext == ".caf";
    if ext == ".caf" && !t.apac_caf && r.bit_depth != BitDepth::F32 {
        let depth = if r.bit_depth == BitDepth::I16 {
            "i16"
        } else {
            "i24"
        };
        return Err(invalid(format!(
            "--output-bit-depth {depth} is not supported for .caf output (CAF is float32 only)"
        )));
    }
    t.iamf = ext == ".iamf";
    t.iamf_mp4 = r.iamf_mp4 && r.iamf_available;
    if t.iamf || r.iamf_mp4 {
        t.iamf_layers = iamf_scalable_layers(r.iamf_layers, r.layout, logs)?;
    } else if !r.iamf_layers.is_empty() {
        logs.push(Log {
            level: Level::Warning,
            module: "engine",
            message: "ignoring IAMF scalable layers for non-IAMF output".into(),
        });
    }
    if t.flac {
        if !flac_supports_layout(r.layout) {
            return Err(unsupported(format!(
                "FLAC output supports only non-height layouts (binaural, 5.1, 7.1); layout '{}' is not supported",
                r.layout
            )));
        }
        if let Some(channels) = r.channels.filter(|&c| c > 8) {
            return Err(unsupported(format!(
                "FLAC supports 1-8 channels; layout '{}' renders {channels} channels",
                r.layout
            )));
        }
    }
    if t.iamf && !r.iamf_available {
        return Err(unsupported(
            "IAMF output requires a build configured with MR_ADM_ENABLE_IAMF=ON and the official AOM iamf-tools \
             bridge",
        ));
    }
    if (t.iamf || r.iamf_mp4) && r.layout == "9.1.6" {
        return Err(unsupported(
            "IAMF 9.1.6 output is disabled because expanded/Base-Enhanced IAMF is not currently compatible enough \
             for release output",
        ));
    }
    if r.iamf_mp4 {
        if ext != ".mp4" {
            return Err(invalid(format!(
                "--iamf-container mp4 requires output path with .mp4 extension; got '{ext}'"
            )));
        }
        if !r.iamf_available {
            return Err(unsupported(
                "--iamf-container mp4 requires a build configured with MR_ADM_ENABLE_IAMF=ON",
            ));
        }
        match r.packager {
            Packager::None => {
                return Err(unsupported(
                    "--iamf-container mp4 requires mp4box (GPAC) or ffmpeg in PATH; install GPAC: https://gpac.io",
                ));
            }
            Packager::Ffmpeg if r.ffmpeg_major < 7 => logs.push(Log {
                level: Level::Warning,
                module: "engine",
                message: format!(
                    "ffmpeg {} detected; IAMF-in-MP4 ialb loudness box is unreliable below version 7.0 — consider \
                     installing mp4box (GPAC)",
                    r.ffmpeg_major
                ),
            }),
            _ => {}
        }
    }
    Ok(t)
}
