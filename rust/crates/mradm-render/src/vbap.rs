//! The VBAP loudspeaker backend (`saf-vbap`, PCM consistency 4.5): the per-channel gain tables
//! of Objects (VBAP / MDAP after channel lock and divergence) and DirectSpeakers (label,
//! position or matrix routing and the 22.2 LFE routing), compiled into one `PcmMixer` plan.
//! Moved from the C++ `vbap_renderer` with the same block precedence, arithmetic, checkpoints,
//! warnings and errors; the layout registry and capabilities stay with the C++ adapter.
use crate::routing::{self, Error, Level, LfeRouting, MatrixRoute, ResolvedRoute, Target};
use crate::scene::{self, in_effect, labels, span};
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::pcm_mix::{Block, Coefficients, Plan, Row};
use mradm_dsp::scene_math::{self as math, Speaker as SceneSpeaker};
use mradm_dsp::vbap::Panner;
use mradm_scene::RenderScene;
use std::collections::BTreeMap;
use std::sync::Arc;

/// One output speaker of the layout.
#[derive(Clone, Copy, Debug)]
pub struct Speaker<'a> {
    pub label: &'a [u8],
    pub azimuth: f32,
    pub elevation: f32,
    pub is_lfe: bool,
}

/// DirectSpeakers routing after `auto` resolved to `label`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Routing {
    Label,
    Position,
    Matrix,
}

pub struct Request<'a> {
    pub scene: &'a RenderScene,
    pub speakers: &'a [Speaker<'a>],
    pub layout_id: &'a [u8],
    /// The `input=` context of a channel outside the input.
    pub input_path: &'a [u8],
    /// MDAP spread for Objects extent (3D layouts only).
    pub spread: bool,
    pub routing: Routing,
    /// The parsed matrix; matrix routing without one fails.
    pub matrix: Option<&'a [MatrixRoute<'a>]>,
    pub split_power: bool,
}

/// The live-override key of one input channel: its owning object and, for DirectSpeakers, the
/// canonical label of its first in-effect block.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Binding {
    pub input_channel: u16,
    pub object_id: Vec<u8>,
    pub label_key: Vec<u8>,
}

pub struct Prepared {
    pub plan: Arc<Plan>,
    pub bindings: Vec<Binding>,
}

pub use crate::scene::Log;

fn error(code: i32, message: impl Into<Vec<u8>>, context: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: context.into(),
    }
}

fn dsp_error(e: mradm_dsp::Error, context: &[u8]) -> Error {
    error(crate::offline::dsp_code(&e), e.to_string(), context)
}

/// LFE speakers sit below the horizon but are not panned, so they do not make a layout 3D.
fn is_2d(speakers: &[Speaker]) -> bool {
    speakers
        .iter()
        .all(|s| s.is_lfe || s.elevation.abs() < 1.0e-6)
}

/// The VBAP gains of the non-LFE speakers, expanded to every output (LFE outputs stay silent).
struct Gains<'a> {
    speakers: &'a [Speaker<'a>],
    directions: Vec<f32>,
    is_3d: bool,
    panner: Option<Panner>,
}

impl<'a> Gains<'a> {
    fn new(speakers: &'a [Speaker<'a>]) -> Self {
        Self {
            speakers,
            directions: speakers
                .iter()
                .filter(|s| !s.is_lfe)
                .flat_map(|s| [s.azimuth, s.elevation])
                .collect(),
            is_3d: !is_2d(speakers),
            panner: None,
        }
    }

    /// A failure is `render_failed` with the C++ `Rust DSP: ` wording; the caller adds context.
    fn point(
        &mut self,
        azimuth: f32,
        elevation: f32,
        gain: f32,
        spread: f32,
    ) -> Result<Vec<f32>, Error> {
        let failed = |e: mradm_dsp::Error| error(4, format!("Rust DSP: {e}"), Vec::new());
        if ENABLED {
            let _ = process(
                "vbap.01-source.f32",
                Words::F32(&[azimuth, elevation, gain, spread]),
            );
            let _ = process("vbap.02-speakers.f32", Words::F32(&self.directions));
        }
        if self.panner.is_none() {
            if !(4..=200_000).contains(&self.directions.len()) {
                return Err(failed(mradm_dsp::Error::InvalidArgument(
                    "Invalid speaker array",
                )));
            }
            self.panner = Some(Panner::new(&self.directions, self.is_3d).map_err(failed)?);
        }
        let panner = self.panner.as_ref().expect("created above");
        let mut table = vec![0.; self.directions.len() / 2];
        panner
            .gains(azimuth, elevation, spread, &mut table)
            .map_err(failed)?;
        if ENABLED {
            let _ = process("vbap.03-gains.f32", Words::F32(&table));
        }
        let mut panned = table.into_iter();
        Ok(self
            .speakers
            .iter()
            .map(|s| {
                if s.is_lfe {
                    0.
                } else {
                    panned.next().unwrap_or(0.) * gain
                }
            })
            .collect())
    }
}

/// The gains of one Objects source; the MDAP spread follows its extent on 3D layouts.
fn source_gains(
    gains: &mut Gains,
    source: &mradm_adm::records::ObjectBlock,
    spread: bool,
) -> Result<Vec<f32>, Error> {
    let p = &source.position;
    let [azimuth, elevation, _] = scene::polar(p);
    let spread = if gains.is_3d && spread {
        let distance = if p.cartesian != 0 {
            math::length([p.x, p.y, p.z])
        } else {
            p.distance
        };
        math::spread(
            [source.width, source.height, source.depth],
            distance,
            source.divergence,
            source.azimuth_range,
            false,
        )
    } else {
        0.
    };
    gains.point(azimuth, elevation, source.gain, spread)
}

struct GainBlock {
    gains: Vec<f32>,
    start: u64,
    end: u64,
    jump: bool,
    smoothable: bool,
    interpolation: Option<u64>,
}

#[derive(Default)]
struct Channel {
    object_id: Vec<u8>,
    label_key: Vec<u8>,
    blocks: Vec<GainBlock>,
}

const MODULE_WARNING: Level = Level::Warning;

fn concat(parts: &[&[u8]]) -> Vec<u8> {
    parts.concat()
}

/// Whether a non-muted object has an in-effect block off the horizontal plane.
fn has_elevated_sources(scene: &RenderScene) -> bool {
    let s = &scene.snapshot;
    s.objects.iter().enumerate().any(|(i, object)| {
        !scene.overlay.objects[i].mute
            && span(&s.tracks, object.tracks).iter().any(|track| {
                let blocks = span(&s.object_blocks, track.blocks);
                in_effect(blocks, |b| b.start).into_iter().any(|b| {
                    let position = scene::offset(blocks[b].position, object);
                    scene::polar(&position)[1].abs() > 1.0e-3
                })
            })
    })
}

// A linear port of the C++ gain-table preparation keeps the block precedence visible.
fn gain_matrix(
    r: &Request,
    lfe: &LfeRouting,
    log: &mut Log,
) -> Result<BTreeMap<u16, Channel>, Error> {
    let scene = r.scene;
    let s = &scene.snapshot;
    let layout_id = r.layout_id;
    if is_2d(r.speakers) && has_elevated_sources(scene) {
        log(
            MODULE_WARNING,
            concat(&[
                b"output layout '",
                layout_id,
                "' is 2D but scene contains Objects with non-zero elevation — height information \
                 will be projected to the horizontal plane"
                    .as_bytes(),
            ]),
        );
    }
    let outputs = r.speakers.len();
    let object_speakers: Vec<SceneSpeaker> = r
        .speakers
        .iter()
        .map(|s| SceneSpeaker {
            azimuth: s.azimuth,
            elevation: s.elevation,
            is_lfe: u32::from(s.is_lfe),
        })
        .collect();
    let targets: Vec<Target> = r
        .speakers
        .iter()
        .map(|s| Target {
            label: s.label,
            is_lfe: s.is_lfe,
        })
        .collect();
    let matrix: Option<Vec<ResolvedRoute>> = match (r.routing, r.matrix) {
        (Routing::Matrix, None) => {
            return Err(error(
                1,
                "DirectSpeakers matrix routing requires a parsed matrix",
                Vec::new(),
            ));
        }
        (Routing::Matrix, Some(m)) => Some(routing::resolve_matrix(m, &targets, layout_id)?),
        _ => None,
    };
    let mut gains = Gains::new(r.speakers);
    let mut screen_ref_warned = false;
    let mut by_channel: BTreeMap<u16, Channel> = BTreeMap::new();
    for (oi, object) in s.objects.iter().enumerate() {
        if scene.overlay.objects[oi].mute {
            continue;
        }
        let scale = |g: &mut Vec<f32>| {
            if object.gain != 1. {
                g.iter_mut().for_each(|v| *v *= object.gain);
            }
        };
        for track in span(&s.tracks, object.tracks) {
            if track.channel < 0 {
                continue;
            }
            let input = track.channel as u16;
            let uid = s.strings[track.uid as usize].as_bytes();
            let track_context = || concat(&[b"track_uid=", uid]);
            let channel = by_channel.entry(input).or_default();
            channel.object_id = s.strings[object.id as usize].as_bytes().to_vec();
            let direct = span(&s.direct_blocks, track.ds_blocks);
            let direct_states = span(&scene.overlay.direct_blocks, track.ds_blocks);
            let in_effect_direct = in_effect(direct, |b| b.start);
            // Live controls bind to the same in-effect blocks as the gains: an overwritten block
            // must not leave its speaker label behind.
            if let Some(&first) = in_effect_direct.first()
                && let Some(label) = labels(scene, direct[first].labels).first()
            {
                channel.label_key = routing::canonical_label(label);
            }

            let blocks = span(&s.object_blocks, track.blocks);
            for b in in_effect(blocks, |b| b.start) {
                let prepared = scene::prepare_object_block(
                    &blocks[b],
                    object,
                    &object_speakers,
                    &mut screen_ref_warned,
                    &mut |text| log(MODULE_WARNING, text.as_bytes().to_vec()),
                );
                let mut sum = vec![0.; outputs];
                for source in &prepared.sources {
                    let source = source_gains(&mut gains, source, r.spread)
                        .map_err(|e| error(e.code, e.message, track_context()))?;
                    sum.iter_mut().zip(&source).for_each(|(a, b)| *a += b);
                }
                scale(&mut sum);
                channel.blocks.push(GainBlock {
                    gains: sum,
                    start: prepared.start,
                    end: prepared.end,
                    jump: prepared.jump,
                    smoothable: true,
                    interpolation: prepared.interpolation,
                });
            }

            for b in in_effect_direct {
                let ds = &direct[b];
                let ds_labels = labels(scene, ds.labels);
                let mut out = vec![0.; outputs];
                let position_or_front = |log: &mut Log| {
                    if direct_states[b].has_position {
                        [ds.azimuth, ds.elevation]
                    } else {
                        log(
                            MODULE_WARNING,
                            b"DirectSpeakers block has no nominal position; using front-centre \
                              (azimuth 0, elevation 0)"
                                .to_vec(),
                        );
                        [0., 0.]
                    }
                };
                let target = routing::lfe_target(&ds_labels, ds.low_pass_present != 0);
                if lfe.applies_to_22_2 && target != routing::LfeTarget::None {
                    out[routing::LFE1_22_2] = ds.gain * lfe.gain(target, routing::LfeTarget::Lfe1);
                    out[routing::LFE2_22_2] = ds.gain * lfe.gain(target, routing::LfeTarget::Lfe2);
                } else if target != routing::LfeTarget::None {
                    match routing::target_index(&targets, &ds_labels) {
                        Some(t) if targets[t].is_lfe => out[t] = ds.gain,
                        _ => log(
                            MODULE_WARNING,
                            concat(&[
                                b"DirectSpeakers LFE channel has no matching LFE output in layout '",
                                layout_id,
                                "' — channel dropped".as_bytes(),
                            ]),
                        ),
                    }
                } else if r.routing == Routing::Matrix {
                    let resolved = matrix.as_deref().unwrap_or_default();
                    let route = &resolved[routing::matrix_route(resolved, &ds_labels)?];
                    for t in &route.targets {
                        out[t.output] = ds.gain * t.gain;
                    }
                } else if r.routing == Routing::Position {
                    let [azimuth, elevation] = position_or_front(log);
                    out = gains
                        .point(azimuth, elevation, ds.gain, 0.)
                        .map_err(|e| error(e.code, e.message, track_context()))?;
                } else {
                    match routing::target_index(&targets, &ds_labels) {
                        Some(t) if !targets[t].is_lfe => out[t] = ds.gain,
                        _ => {
                            let from_label = routing::label_position(&ds_labels);
                            let [azimuth, elevation] = match from_label {
                                Some(p) => p,
                                None => position_or_front(log),
                            };
                            let label = ds_labels.first().copied().unwrap_or(b"<missing>");
                            log(
                                MODULE_WARNING,
                                concat(&[
                                    b"DirectSpeakers label '",
                                    label,
                                    "' not in output layout — spatializing from ".as_bytes(),
                                    if from_label.is_some() {
                                        b"label direction"
                                    } else {
                                        b"nominal position"
                                    },
                                ]),
                            );
                            out = gains
                                .point(azimuth, elevation, ds.gain, 0.)
                                .map_err(|e| error(e.code, e.message, track_context()))?;
                        }
                    }
                }
                scale(&mut out);
                channel.blocks.push(GainBlock {
                    gains: out,
                    start: ds.start,
                    end: ds.end.min(object.end),
                    jump: true,
                    smoothable: false,
                    interpolation: None,
                });
            }
        }
    }
    // Blocks sharing a start keep their scene (ADM document) order; the last one is in effect.
    for channel in by_channel.values_mut() {
        channel.blocks.sort_by_key(|b| b.start);
    }
    Ok(by_channel)
}

/// Prepares the gain plan; `log` receives the `saf-vbap` lines in order, also before an error.
pub fn prepare(r: &Request, log: &mut Log) -> Result<Prepared, Error> {
    let lfe = scene::lfe_routing(r.scene, r.split_power, r.layout_id, log)?;
    let channels = gain_matrix(r, &lfe, log)?;
    if channels.is_empty() {
        log(
            MODULE_WARNING,
            b"no renderable tracks found (all muted?), writing silence".to_vec(),
        );
    }
    let inputs = r.scene.info.channels;
    if let Some(&channel) = channels.keys().find(|&&c| c >= inputs) {
        return Err(error(
            4,
            format!("track channel index {channel} is outside input channel count {inputs}"),
            concat(&[b"input=", r.input_path]),
        ));
    }
    let outputs = r.speakers.len();
    let mut rows = Vec::with_capacity(channels.len());
    let mut blocks = Vec::new();
    let mut coefficients = Vec::new();
    let mut bindings = Vec::with_capacity(channels.len());
    for (input_channel, channel) in channels {
        rows.push(Row {
            input_channel: usize::from(input_channel),
            block_offset: blocks.len(),
            block_count: channel.blocks.len(),
            output_gain: 1.,
        });
        for b in channel.blocks {
            // Object-duration clipping can put the end before the start. Keep the empty block
            // (and its gains) so later blocks retain the interpolation predecessor.
            blocks.push(Block {
                start: b.start,
                end: b.start.max(b.end),
                interpolation: b.interpolation.unwrap_or(0),
                flags: u32::from(b.jump)
                    | (u32::from(b.smoothable) << 1)
                    | (u32::from(b.interpolation.is_some()) << 2),
            });
            coefficients.extend_from_slice(&b.gains);
        }
        bindings.push(Binding {
            input_channel,
            object_id: channel.object_id,
            label_key: channel.label_key,
        });
    }
    let plan = Plan::new(
        usize::from(inputs),
        outputs,
        rows,
        blocks,
        Coefficients::Speaker(coefficients),
    )
    .map_err(|e| dsp_error(e, b"PCM mix preparation"))?;
    Ok(Prepared {
        plan: Arc::new(plan),
        bindings,
    })
}
