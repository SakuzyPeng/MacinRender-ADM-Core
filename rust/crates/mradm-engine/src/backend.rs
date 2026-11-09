//! Backend preparation (PCM consistency 5.2 C1): one entry for the six Rust backends, moved from
//! the C++ `IRenderer::prepare` shells with the same check order, messages, locators and log
//! modules, ahead of the `mradm-render` preparations. The file streams and the offline driver
//! ([`crate::offline`]) read the result.
//!
//! What only the platform layer can do (the room floating-point scope, the room resources with
//! their CUDA selection) arrives through [`Host`].
use crate::request::{Error, Level, Log, Outcome, code};
use mradm_dsp::room::{
    loading::Package,
    screen::{RoomScreenGeometry, RoomScreens},
};
use mradm_render::room_runtime::{Config as RoomConfig, Prepare, Resources, Scope};
use mradm_render::routing::{self, MatrixRoute};
use mradm_render::{
    binaural::Spread as BinauralSpread, binaural_hrtf, binaural_render, ear, hoa, offline, room,
    triple_balance, vbap,
};
use mradm_scene::RenderScene;
use mradm_scene::layouts::{self, Geometry};
use std::path::Path;
use std::sync::Arc;

/// The backends with a Rust preparation; the values are the C++ `render_common::EngineBackend`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Kind {
    Ear = 0,
    Vbap = 1,
    TripleBalance = 2,
    Hoa = 3,
    Binaural = 4,
    Room = 5,
}

impl Kind {
    pub fn from_u32(value: u32) -> Option<Self> {
        Some(match value {
            0 => Self::Ear,
            1 => Self::Vbap,
            2 => Self::TripleBalance,
            3 => Self::Hoa,
            4 => Self::Binaural,
            5 => Self::Room,
            _ => return None,
        })
    }

    /// The backend's log module.
    pub fn module(self) -> &'static str {
        match self {
            Self::Ear => "ear",
            Self::Vbap => "saf-vbap",
            Self::TripleBalance => "triple-balance",
            Self::Hoa => "hoa-encode",
            Self::Binaural => "binaural",
            Self::Room => "room-binaural",
        }
    }
}

/// `DirectSpeakersRoutingMode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum Routing {
    Automatic = 0,
    Label = 1,
    Position = 2,
    Matrix = 3,
}

/// `SpeakerSpreadMode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum SpeakerSpread {
    Automatic = 0,
    None = 1,
    Mdap = 2,
}

/// `BinauralSpreadMode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum BinauralSpreadMode {
    Automatic = 0,
    None = 1,
    Cloud = 2,
    SafSpreader = 3,
}

impl BinauralSpreadMode {
    /// The binaural backend's numerical spread (the C++ `numerical_spread`).
    pub fn numerical(self) -> BinauralSpread {
        match self {
            Self::None => BinauralSpread::None,
            Self::SafSpreader => BinauralSpread::SafSpreader,
            _ => BinauralSpread::Cloud,
        }
    }
}

/// A user SOFA file: its UTF-8 path (also the display text) and file name.
#[derive(Clone, Copy, Debug)]
pub struct Sofa<'a> {
    pub path: &'a [u8],
    pub file_name: &'a [u8],
}

/// The screen configuration: `reference` holds the default geometry when absent.
#[derive(Clone, Copy, Debug)]
pub struct Screens {
    pub has_reference: bool,
    pub reference: RoomScreenGeometry,
    pub has_reproduction: bool,
    pub reproduction: RoomScreenGeometry,
}

/// `RoomConfig`: the profile's package (none when no profile), the UTF-8 personal HRIR path, the
/// scale and the quality.
#[derive(Clone)]
pub struct RoomRequest<'a> {
    pub package: Option<Arc<Package>>,
    pub hrir: &'a [u8],
    pub metres: f64,
    pub quality: u32,
}

/// The `RenderPlan` fields a backend preparation reads.
#[derive(Clone)]
pub struct Request<'a> {
    pub kind: Kind,
    pub layout_id: &'a [u8],
    pub apple_geometry: bool,
    pub routing: Routing,
    /// The parsed DirectSpeakers matrix, when the request has one.
    pub matrix: Option<&'a [MatrixRoute<'a>]>,
    pub speaker_spread: SpeakerSpread,
    pub binaural_spread: BinauralSpreadMode,
    pub triple_balance_d: bool,
    pub split_power: bool,
    pub smoothing_frames: u32,
    pub default_interp_ms: u32,
    pub input_path: &'a [u8],
    pub sofa: Option<Sofa<'a>>,
    pub screens: Screens,
    pub room: RoomRequest<'a>,
}

/// The services of the platform layer; errors are a code and its message.
pub trait Host {
    /// `mradm_room_validate_screens` of one record, in the room scope.
    fn validate_screens(&self, screens: &RoomScreens) -> Result<(), (i32, String)>;
    /// The room resources (`room_internal::prepare` without reuse), in the room scope.
    fn prepare_room(
        &self,
        package: &Package,
        hrir: &[u8],
        sofa: &[u8],
        quality: u32,
        metres: f64,
    ) -> Result<Resources, (i32, String)>;
    /// The polar conversion the room event table runs in the room scope.
    fn room_polar(&self) -> room::PolarConversion;
    fn room_scope(&self) -> Scope;
    fn room_switch(&self) -> Prepare;
    /// Whether the calling Scene worker renders a room: Windows registers it with MMCSS.
    fn room_worker(&self, active: bool) {
        let _ = active;
    }
    /// The Scene worker's wait for output ring space while a room renders: Windows uses a
    /// high-resolution waitable timer.
    fn room_wait(&self, duration: std::time::Duration) {
        std::thread::sleep(duration);
    }
}

/// What the Triple Balance file stream reads besides the offline kernel.
pub struct TripleBalance {
    pub sizes: Arc<[triple_balance::SizeTrack]>,
    pub spread_none: bool,
}

/// The room file stream's table, resources and configuration.
pub struct RoomPrepared {
    pub table: Arc<room::Prepared>,
    pub resources: Arc<Resources>,
    pub config: RoomConfig,
    pub sample_rate: u32,
}

/// An immutable backend preparation, reused across offline windows and file streams.
pub struct Prepared {
    pub kind: Kind,
    /// The loudspeaker driver's kernel (VBAP, HOA, EAR, Triple Balance).
    pub kernel: Option<offline::Kernel>,
    pub bindings: Arc<[vbap::Binding]>,
    pub inputs: usize,
    pub outputs: usize,
    pub frames: u64,
    pub sample_rate: u32,
    pub triple_balance: Option<TripleBalance>,
    pub binaural: Option<Arc<binaural_render::Prepared>>,
    pub room: Option<RoomPrepared>,
}

/// The preparation, its log lines, and the Triple Balance report (also of a rejection).
pub struct Preparation {
    pub outcome: Outcome<Arc<Prepared>>,
    pub report: Option<String>,
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

fn render_error(e: routing::Error) -> Error {
    Error::new(e.code, text(&e.message), text(&e.context))
}

/// Collects the `mradm-render` log lines under `module`.
struct Lines {
    module: &'static str,
    logs: Vec<Log>,
}

impl Lines {
    fn push(&mut self, level: Level, message: String) {
        self.logs.push(Log {
            level,
            module: self.module,
            message,
        });
    }
    fn render(&mut self, level: routing::Level, message: Vec<u8>) {
        let level = match level {
            routing::Level::Info => Level::Info,
            routing::Level::Warning => Level::Warning,
        };
        self.push(level, text(&message));
    }
}

/// Prepares `r` on the policy-applied scene.
pub fn prepare(scene: &RenderScene, r: &Request, host: &dyn Host) -> Preparation {
    let mut lines = Lines {
        module: r.kind.module(),
        logs: Vec::new(),
    };
    let mut report = None;
    let result = match r.kind {
        Kind::Vbap => prepare_vbap(scene, r, &mut lines),
        Kind::Hoa => prepare_hoa(scene, r, &mut lines),
        Kind::Ear => prepare_ear(scene, r, &mut lines),
        Kind::TripleBalance => prepare_triple_balance(scene, r, &mut lines, &mut report),
        Kind::Binaural => prepare_binaural(scene, r, &mut lines),
        Kind::Room => prepare_room(scene, r, &mut lines, host),
    };
    Preparation {
        outcome: Outcome {
            logs: lines.logs,
            result: result.map(Arc::new),
        },
        report,
    }
}

fn base(kind: Kind, scene: &RenderScene, outputs: usize) -> Prepared {
    Prepared {
        kind,
        kernel: None,
        bindings: Arc::from([]),
        inputs: usize::from(scene.info.channels),
        outputs,
        frames: scene.info.frames,
        sample_rate: scene.info.sample_rate,
        triple_balance: None,
        binaural: None,
        room: None,
    }
}

fn with_kernel(
    mut prepared: Prepared,
    kernel: offline::Kernel,
    bindings: Vec<vbap::Binding>,
) -> Prepared {
    prepared.kernel = Some(kernel);
    prepared.bindings = Arc::from(bindings);
    prepared
}

fn geometry_name(apple: bool) -> &'static str {
    if apple { "apple" } else { "standard" }
}

fn prepare_vbap(scene: &RenderScene, r: &Request, lines: &mut Lines) -> Result<Prepared, Error> {
    let geometry = if r.apple_geometry {
        Geometry::Apple
    } else {
        Geometry::Standard
    };
    let Some(layout) = layouts::layouts(geometry)
        .iter()
        .find(|l| l.id.as_bytes() == r.layout_id)
    else {
        return Err(Error::new(
            code::UNSUPPORTED,
            format!(
                "unsupported VBAP output layout '{}' for speaker geometry '{}'",
                text(r.layout_id),
                geometry_name(r.apple_geometry)
            ),
            "",
        ));
    };
    lines.push(
        Level::Info,
        format!("speaker geometry: {}", geometry_name(r.apple_geometry)),
    );
    let routing = match r.routing {
        Routing::Automatic | Routing::Label => vbap::Routing::Label,
        Routing::Position => vbap::Routing::Position,
        Routing::Matrix => vbap::Routing::Matrix,
    };
    let name = match routing {
        vbap::Routing::Label => "label",
        vbap::Routing::Position => "position",
        vbap::Routing::Matrix => "matrix",
    };
    lines.push(Level::Info, format!("DirectSpeakers routing: {name}"));
    let speakers: Vec<_> = layout
        .speakers
        .iter()
        .map(|s| vbap::Speaker {
            label: s.label.as_bytes(),
            azimuth: s.azimuth,
            elevation: s.elevation,
            is_lfe: s.is_lfe,
        })
        .collect();
    let request = vbap::Request {
        scene,
        speakers: &speakers,
        layout_id: r.layout_id,
        input_path: r.input_path,
        spread: r.speaker_spread != SpeakerSpread::None,
        routing,
        matrix: r.matrix,
        split_power: r.split_power,
    };
    let p = vbap::prepare(&request, &mut |level, message| lines.render(level, message))
        .map_err(render_error)?;
    Ok(with_kernel(
        base(Kind::Vbap, scene, speakers.len()),
        offline::Kernel::Mix(p.plan),
        p.bindings,
    ))
}

fn prepare_hoa(scene: &RenderScene, r: &Request, lines: &mut Lines) -> Result<Prepared, Error> {
    if r.routing != Routing::Automatic {
        return Err(Error::new(
            code::UNSUPPORTED,
            "HOA renderer does not support explicit DirectSpeakers routing; use automatic",
            "",
        ));
    }
    let request = hoa::Request {
        scene,
        layout_id: r.layout_id,
        split_power: r.split_power,
    };
    let p = hoa::prepare(&request, &mut |level, message| lines.render(level, message))
        .map_err(render_error)?;
    Ok(with_kernel(
        base(Kind::Hoa, scene, mradm_dsp::hoa::CHANNELS),
        offline::Kernel::Hoa(p.plan),
        p.bindings,
    ))
}

fn prepare_ear(scene: &RenderScene, r: &Request, lines: &mut Lines) -> Result<Prepared, Error> {
    let matrix_routing = r.routing == Routing::Matrix;
    if r.routing != Routing::Automatic && !matrix_routing {
        return Err(Error::new(
            code::UNSUPPORTED,
            "EAR renderer supports only automatic or matrix DirectSpeakers routing",
            "",
        ));
    }
    let request = ear::Request {
        scene,
        layout_id: r.layout_id,
        geometry: if r.apple_geometry {
            Geometry::Apple
        } else {
            Geometry::Standard
        },
        matrix_routing,
        matrix: r.matrix,
        input_path: r.input_path,
        split_power: r.split_power,
    };
    let p = ear::prepare(&request, &mut |level, message| lines.render(level, message))
        .map_err(render_error)?;
    let outputs = p.outputs;
    let plan = offline::EarPlan {
        mix: p.plan,
        filters: p.filters,
        wav71: r.layout_id == b"wav71",
    };
    Ok(with_kernel(
        base(Kind::Ear, scene, outputs),
        offline::Kernel::Ear(Arc::new(plan)),
        p.bindings,
    ))
}

fn prepare_triple_balance(
    scene: &RenderScene,
    r: &Request,
    lines: &mut Lines,
    report: &mut Option<String>,
) -> Result<Prepared, Error> {
    let spread = match r.speaker_spread {
        SpeakerSpread::Automatic => triple_balance::Spread::Automatic,
        SpeakerSpread::None => triple_balance::Spread::None,
        SpeakerSpread::Mdap => triple_balance::Spread::Mdap,
    };
    let request = triple_balance::Request {
        scene,
        layout_id: r.layout_id,
        mode: if r.triple_balance_d {
            triple_balance::Mode::D
        } else {
            triple_balance::Mode::Standard
        },
        spread,
        split_power: r.split_power,
        standard_geometry: !r.apple_geometry,
        smoothing_frames: r.smoothing_frames,
        routing: match r.routing {
            Routing::Automatic => triple_balance::Routing::Automatic,
            Routing::Label => triple_balance::Routing::Label,
            Routing::Position => triple_balance::Routing::Position,
            Routing::Matrix => triple_balance::Routing::Matrix,
        },
        has_matrix: r.matrix.is_some(),
        input_path: r.input_path,
    };
    let outcome =
        triple_balance::prepare(&request, &mut |level, message| lines.render(level, message));
    *report = Some(outcome.report);
    let p = outcome.result.map_err(render_error)?;
    let mut prepared = with_kernel(
        base(Kind::TripleBalance, scene, p.outputs),
        offline::Kernel::TripleBalance(p.plan),
        p.bindings,
    );
    prepared.triple_balance = Some(TripleBalance {
        sizes: Arc::from(p.size_tracks),
        spread_none: spread == triple_balance::Spread::None,
    });
    Ok(prepared)
}

#[cfg(unix)]
pub(crate) fn native(bytes: &[u8]) -> Option<&Path> {
    use std::os::unix::ffi::OsStrExt;
    Some(Path::new(std::ffi::OsStr::from_bytes(bytes)))
}
#[cfg(not(unix))]
pub(crate) fn native(bytes: &[u8]) -> Option<&Path> {
    std::str::from_utf8(bytes).ok().map(Path::new)
}

const MIN_BINAURAL_BLOCK: u64 = 1024;

fn prepare_binaural(
    scene: &RenderScene,
    r: &Request,
    lines: &mut Lines,
) -> Result<Prepared, Error> {
    if r.routing != Routing::Automatic {
        return Err(Error::new(
            code::UNSUPPORTED,
            "SAF binaural renderer does not support explicit DirectSpeakers routing; use automatic",
            "",
        ));
    }
    mradm_render::scene::lfe_routing(scene, r.split_power, r.layout_id, &mut |level, message| {
        lines.render(level, message)
    })
    .map_err(render_error)?;
    let rate = scene.info.sample_rate;
    if rate != 48000 {
        return Err(Error::new(
            code::INVALID_ARGUMENT,
            format!("binaural renderer requires 48000 Hz input, got {rate}"),
            format!("input={}", text(r.input_path)),
        ));
    }
    let block = MIN_BINAURAL_BLOCK.max(u64::from(r.smoothing_frames));
    let hrtf = match r.sofa {
        Some(sofa) => {
            let path = native(sofa.path).ok_or_else(|| {
                Error::new(
                    code::IO_ERROR,
                    "SOFA file could not be opened",
                    format!("path={}", text(sofa.path)),
                )
            })?;
            let sofa = binaural_hrtf::Sofa {
                path,
                display: sofa.path,
                file_name: sofa.file_name,
            };
            binaural_hrtf::load(Some(&sofa), rate)
        }
        None => binaural_hrtf::load(None, rate),
    }
    .and_then(|dataset| binaural_hrtf::build(dataset, block, false))
    .map_err(render_error)?;
    lines.push(
        Level::Info,
        format!(
            "HRTF source: {} ({} dirs, {} taps @ {} Hz)",
            hrtf.name, hrtf.num_dirs, hrtf.hrir_len, rate
        ),
    );
    let p = binaural_render::prepare(
        scene,
        Arc::new(hrtf),
        r.binaural_spread.numerical(),
        r.smoothing_frames,
        &mut |level, message| lines.render(level, message),
    )
    .map_err(render_error)?;
    let mut prepared = base(Kind::Binaural, scene, 2);
    prepared.binaural = Some(Arc::new(p));
    Ok(prepared)
}

fn prepare_room(
    scene: &RenderScene,
    r: &Request,
    lines: &mut Lines,
    host: &dyn Host,
) -> Result<Prepared, Error> {
    let s = r.screens;
    let validation = RoomScreens {
        enabled: 1,
        reserved: 0,
        reference: s.reference,
        reproduction: if s.has_reproduction {
            s.reproduction
        } else {
            s.reference
        },
    };
    host.validate_screens(&validation)
        .map_err(|(code, message)| Error::new(code, message, "screen"))?;
    let Some(package) = r.room.package.clone() else {
        return Err(Error::new(
            code::INVALID_ARGUMENT,
            "room-binaural 需要房间包",
            "",
        ));
    };
    let sofa = r.sofa.map_or(&b""[..], |s| s.path);
    let resources = host
        .prepare_room(&package, r.room.hrir, sofa, r.room.quality, r.room.metres)
        .map_err(|(code, message)| Error::new(code, message, "room-binaural"))?;
    let sample_rate = resources.set.room.profile.sample_rate;
    let request = room::Request {
        scene,
        sample_rate,
        layout_id: r.layout_id,
        spread: match r.binaural_spread {
            BinauralSpreadMode::None => room::Spread::None,
            BinauralSpreadMode::SafSpreader => room::Spread::SafSpreader,
            _ => room::Spread::Automatic,
        },
        routing: match r.routing {
            Routing::Automatic => room::Routing::Automatic,
            Routing::Label => room::Routing::Label,
            Routing::Position => room::Routing::Position,
            Routing::Matrix => room::Routing::Matrix,
        },
        has_matrix: r.matrix.is_some(),
        default_interp_ms: r.default_interp_ms,
        screens: room::Screens {
            reference: s.has_reference.then_some(s.reference),
            reproduction: s.has_reproduction.then_some(s.reproduction),
        },
    };
    let table = room::prepare(
        &request,
        &mut |level, message| lines.render(level, message),
        host.room_polar(),
    )
    .map_err(render_error)?;
    let quality = mradm_dsp::room::quality::Quality::try_from(r.room.quality)
        .map_err(|_| Error::new(code::INVALID_ARGUMENT, "无效房间档位", ""))?;
    let mut prepared = base(Kind::Room, scene, 2);
    prepared.room = Some(RoomPrepared {
        table: Arc::new(table),
        resources: Arc::new(resources),
        config: RoomConfig {
            package,
            hrir: text(r.room.hrir),
            metres: r.room.metres,
            quality,
        },
        sample_rate,
    });
    Ok(prepared)
}
