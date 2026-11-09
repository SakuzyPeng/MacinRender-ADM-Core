//! The Scene stream renderers (`mradm-render` `live_vbap`, `live_triple_balance`, `live_binaural`
//! and `room_stream::Live`) behind one type, and their creation from the stream configuration
//! (PCM consistency 5.4 A, formerly the C++ `create_live_renderer` and the four
//! `create_live_*_renderer` shells): the selection, the checks, their order and texts unchanged.
use crate::backend::{BinauralSpreadMode, Host, RoomRequest, Screens, Sofa, SpeakerSpread, native};
use crate::request::{Selection, code};
use mradm_dsp::room::quality::Quality;
use mradm_dsp::room::screen::RoomScreens;
use mradm_render::room_runtime::{Config as RoomConfig, Control};
use mradm_render::room_stream::Live;
use mradm_render::routing::Error;
use mradm_render::{binaural_hrtf, live, live_binaural, live_triple_balance, live_vbap};
use mradm_scene::layouts::Geometry;
use std::sync::Arc;

/// A Scene stream renderer.
pub enum Renderer {
    Vbap(Box<live_vbap::Renderer>),
    TripleBalance(Box<live_triple_balance::Renderer>),
    Room(Box<Live>),
    Binaural(Box<live_binaural::Renderer>),
}

impl Renderer {
    /// The name in boundary texts and the mixer context of rejected buffers.
    pub fn names(&self) -> (&'static str, &'static [u8]) {
        match self {
            Self::Vbap(_) => ("Live VBAP", b"Live VBAP DSP"),
            Self::TripleBalance(_) => ("Live Triple Balance", b"Live Triple Balance DSP"),
            Self::Room(_) => ("room-binaural", b"room-binaural"),
            Self::Binaural(_) => ("Live binaural", b"Live binaural DSP"),
        }
    }
    pub fn output_channels(&self) -> u32 {
        match self {
            Self::Vbap(r) => r.output_channels(),
            Self::TripleBalance(r) => r.output_channels(),
            Self::Room(_) => 2,
            Self::Binaural(r) => r.output_channels(),
        }
    }
    /// The checkpoint name suffix after the Scene renderer key (`<key><suffix>-*`); room writes none.
    pub fn trace_suffix(&self) -> String {
        match self {
            Self::Vbap(r) => format!(".30-vbap{}", r.output_channels()),
            Self::TripleBalance(_) => ".30-triple".into(),
            Self::Room(_) => String::new(),
            Self::Binaural(_) => ".30-binaural".into(),
        }
    }
    pub fn configure(&mut self, generation: u64, elements: &[live::Element]) -> Result<(), Error> {
        match self {
            Self::Vbap(r) => r.configure(generation, elements),
            Self::TripleBalance(r) => r.configure(generation, elements),
            Self::Room(r) => r.configure(generation, elements),
            Self::Binaural(r) => r.configure(generation, elements),
        }
    }
    pub fn reset(&mut self) {
        match self {
            Self::Vbap(r) => r.reset(),
            Self::TripleBalance(r) => r.reset(),
            Self::Room(r) => r.reset(),
            Self::Binaural(r) => r.reset(),
        }
    }
    pub fn render<P: live::PlaneSource + ?Sized>(
        &mut self,
        frame: &live::FrameView<'_, P>,
        output: &mut [f32],
    ) -> Result<(), Error> {
        match self {
            Self::Vbap(r) => r.render(frame, output),
            Self::TripleBalance(r) => r.render(frame, output),
            Self::Room(r) => r.render(frame, output),
            Self::Binaural(r) => r.render(frame, output),
        }
    }
    pub fn diagnostics(&self) -> &[live::Diagnostic] {
        match self {
            Self::Vbap(r) => r.diagnostics(),
            Self::TripleBalance(r) => r.diagnostics(),
            Self::Room(r) => r.diagnostics(),
            Self::Binaural(r) => r.diagnostics(),
        }
    }
    /// The warnings of the last configure: only room raises any.
    pub fn configured(&self) -> &[live::Diagnostic] {
        match self {
            Self::Room(r) => r.diagnostics(),
            _ => &[],
        }
    }
    pub fn end_input(&mut self) -> Result<u64, Error> {
        match self {
            Self::Room(r) => r.end_input(),
            _ => Ok(0),
        }
    }
    pub fn tail(&self) -> u64 {
        match self {
            Self::Room(r) => r.tail_frames(),
            Self::Binaural(r) => r.tail_frames(),
            _ => 0,
        }
    }
    pub fn pose(&mut self, pose: [f32; 3]) {
        match self {
            Self::Room(r) => r.set_pose(pose),
            Self::Binaural(r) => r.set_pose(pose),
            _ => {}
        }
    }
    /// The room control of a room renderer.
    pub fn control(&self) -> Option<Control> {
        match self {
            Self::Room(r) => Some(r.control()),
            _ => None,
        }
    }
}

/// `live_scene::RendererConfig`. `sofa` is set for a non-empty SOFA path; `sink` is whether the
/// caller publishes diagnostics.
pub struct Config<'a> {
    pub selection: Selection,
    pub layout_id: &'a [u8],
    pub apple_geometry: bool,
    pub speaker_spread: SpeakerSpread,
    pub binaural_spread: BinauralSpreadMode,
    pub split_power: bool,
    pub smoothing_frames: u32,
    pub sample_rate: u32,
    pub sofa: Option<Sofa<'a>>,
    pub screens: Screens,
    pub room: RoomRequest<'a>,
    pub sink: bool,
}

/// A created renderer and the rate it renders at (the room package's for room-binaural).
pub struct Created {
    pub renderer: Renderer,
    pub sample_rate: u32,
}

fn error(code: i32, message: impl Into<Vec<u8>>, context: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: context.into(),
    }
}

/// The creation of the speaker and binaural renderers reports no context (the C++ shells passed
/// no context buffer).
fn without_context(mut e: Error) -> Error {
    e.context.clear();
    e
}

/// Creates the renderer of `config.selection`.
pub fn create(config: &Config, host: &dyn Host) -> Result<Created, Error> {
    let renderer = match config.selection {
        Selection::RoomBinaural => return room(config, host),
        Selection::Saf => Renderer::Vbap(Box::new(
            live_vbap::Renderer::new(&live_vbap::Config {
                layout_id: config.layout_id,
                geometry: if config.apple_geometry {
                    Geometry::Apple
                } else {
                    Geometry::Standard
                },
                spread: config.speaker_spread != SpeakerSpread::None,
                split_power: config.split_power,
                smoothing_frames: config.smoothing_frames,
            })
            .map_err(without_context)?,
        )),
        Selection::SafBinaural => Renderer::Binaural(Box::new(binaural(config)?)),
        Selection::TripleBalance => Renderer::TripleBalance(Box::new(
            live_triple_balance::Renderer::new(&live_triple_balance::Config {
                layout_id: config.layout_id,
                standard_geometry: !config.apple_geometry,
                spread: match config.speaker_spread {
                    SpeakerSpread::None => live_triple_balance::Spread::None,
                    SpeakerSpread::Mdap => live_triple_balance::Spread::Mdap,
                    SpeakerSpread::Automatic => live_triple_balance::Spread::Automatic,
                },
                has_sofa: config.sofa.is_some(),
                sample_rate: config.sample_rate,
                split_power: config.split_power,
                smoothing_frames: config.smoothing_frames,
            })
            .map_err(without_context)?,
        )),
        _ => {
            return Err(error(
                code::UNSUPPORTED,
                "Scene streams support SAF VBAP, SAF binaural, room-binaural and Triple Balance",
                "",
            ));
        }
    };
    Ok(Created {
        renderer,
        sample_rate: config.sample_rate,
    })
}

/// The live HRTF state (built-in set, or the SOFA file at the stream rate), then the renderer.
fn binaural(config: &Config) -> Result<live_binaural::Renderer, Error> {
    let state = match config.sofa {
        Some(sofa) => {
            let path = native(sofa.path).ok_or_else(|| {
                error(
                    code::IO_ERROR,
                    "SOFA file could not be opened",
                    [b"path=".as_slice(), sofa.path].concat(),
                )
            })?;
            binaural_hrtf::prepare_live(
                Some(&binaural_hrtf::Sofa {
                    path,
                    display: sofa.path,
                    file_name: sofa.file_name,
                }),
                config.sample_rate,
            )?
        }
        None => binaural_hrtf::prepare_live(None, config.sample_rate)?,
    };
    live_binaural::Renderer::new(
        state,
        &live_binaural::Config {
            sample_rate: config.sample_rate,
            spread: match config.binaural_spread {
                BinauralSpreadMode::None => 1,
                BinauralSpreadMode::SafSpreader => 2,
                BinauralSpreadMode::Automatic | BinauralSpreadMode::Cloud => 0,
            },
            smoothing_frames: config.smoothing_frames,
        },
    )
    .map_err(without_context)
}

/// The screens, the room resources, the output and spread checks, then the room renderer.
fn room(config: &Config, host: &dyn Host) -> Result<Created, Error> {
    let s = config.screens;
    let reproduction = if s.has_reproduction {
        s.reproduction
    } else {
        s.reference
    };
    host.validate_screens(&RoomScreens {
        enabled: 1,
        reserved: 0,
        reference: s.reference,
        reproduction,
    })
    .map_err(|(code, message)| error(code, message, "screen"))?;
    let Some(package) = config.room.package.clone() else {
        return Err(error(
            code::INVALID_ARGUMENT,
            "room-binaural 需要房间包",
            "",
        ));
    };
    let sofa = config.sofa.map_or(&b""[..], |s| s.path);
    let resources = host
        .prepare_room(
            &package,
            config.room.hrir,
            sofa,
            config.room.quality,
            config.room.metres,
        )
        .map_err(|(code, message)| error(code, message, "room-binaural"))?;
    let sample_rate = resources.set.room.profile.sample_rate;
    if config.sample_rate != sample_rate || config.layout_id != b"binaural" {
        return Err(error(
            code::UNSUPPORTED,
            "room-binaural 需要与房间包采样率一致的双耳输出",
            "",
        ));
    }
    if config.binaural_spread == BinauralSpreadMode::SafSpreader {
        return Err(error(
            code::UNSUPPORTED,
            "room-binaural 不支持 SAF covariance spreader",
            "",
        ));
    }
    let quality = Quality::try_from(config.room.quality)
        .map_err(|_| error(code::INVALID_ARGUMENT, "无效房间档位", ""))?;
    let live = Live::new(
        Arc::new(resources),
        RoomConfig {
            package,
            hrir: String::from_utf8_lossy(config.room.hrir).into_owned(),
            metres: config.room.metres,
            quality,
        },
        config.binaural_spread != BinauralSpreadMode::None,
        RoomScreens {
            enabled: u32::from(s.has_reproduction),
            reserved: 0,
            reference: s.reference,
            reproduction,
        },
        config.sink,
        host.room_scope(),
        host.room_switch(),
    )?;
    Ok(Created {
        renderer: Renderer::Room(Box::new(live)),
        sample_rate,
    })
}
