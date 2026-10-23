//! The EAR loudspeaker backend (`ear`, PCM consistency 4.5): the EAR output layout (standard,
//! project-defined and Apple geometry), the per-channel direct and diffuse gain tables of Objects
//! (after channel lock and divergence), DirectSpeakers (EAR, matrix and 22.2 LFE routing) and HOA
//! packs (AllRAD decode), compiled into one `PcmMixer` plan, and the decorrelation filter bank.
//! Moved from the C++ `ear_renderer` with the same block precedence, double-precision
//! arithmetic, checkpoints, warnings and errors; the capabilities stay with the C++ adapter.
use crate::offline::dsp_code;
use crate::routing::{
    self, Error, Level, LfeRouting, LfeTarget, MatrixRoute, ResolvedRoute, Target,
};
use crate::scene::{self, Log, in_effect, labels, span};
use crate::vbap::Binding;
use mradm_adm::records::DirectBlock;
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::ear_post::FilterBank;
use mradm_dsp::pcm_mix::{Block, Coefficients, Plan, Row};
use mradm_dsp::scene_math::Speaker as SceneSpeaker;
use mradm_ear::{Calculator, Channel, DirectSpeakers, Layout, Normalization, Object};
use mradm_scene::RenderScene;
use mradm_scene::layouts::{self, Geometry, Speaker};
use std::collections::BTreeMap;
use std::sync::Arc;

/// The EAR post-processing compensation delay of the direct path, in frames.
pub const DELAY: usize = 255;

pub struct Request<'a> {
    pub scene: &'a RenderScene,
    pub layout_id: &'a [u8],
    pub geometry: Geometry,
    /// DirectSpeakers matrix routing (otherwise automatic); it needs a parsed `matrix`.
    pub matrix_routing: bool,
    pub matrix: Option<&'a [MatrixRoute<'a>]>,
    /// The `input=` context of a channel outside the input.
    pub input_path: &'a [u8],
    pub split_power: bool,
}

pub struct Prepared {
    pub plan: Arc<Plan>,
    pub filters: Arc<FilterBank>,
    pub bindings: Vec<Binding>,
    pub outputs: usize,
}

fn error(code: i32, message: impl Into<Vec<u8>>, context: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: context.into(),
    }
}

fn ear_code(e: mradm_ear::Error) -> i32 {
    match e {
        mradm_ear::Error::InvalidArgument(_) => 1,
        mradm_ear::Error::Unsupported(_) => 2,
        mradm_ear::Error::RenderFailed(_) => 4,
    }
}

/// How the C++ adapter reported a failure: an EAR error with its own code or a layout the
/// geometry tables cannot build (both at `layout=`), or a routing error as it is.
enum Failure {
    Ear(mradm_ear::Error),
    Unsupported(Vec<u8>),
    Routing(Error),
}

impl From<mradm_ear::Error> for Failure {
    fn from(e: mradm_ear::Error) -> Self {
        Self::Ear(e)
    }
}

impl Failure {
    fn at(self, layout_id: &[u8]) -> Error {
        let context = [b"layout=", layout_id].concat();
        match self {
            Self::Ear(e) => error(ear_code(e), e.to_string(), context),
            Self::Unsupported(what) => error(
                2,
                [b"unsupported output layout '", layout_id, b"': ", &what].concat(),
                context,
            ),
            Self::Routing(e) => e,
        }
    }
}

fn standard(name: &str) -> Result<Layout, Failure> {
    Ok(Layout {
        name: name.to_owned(),
        channels: mradm_ear::standard_layout(name)?.channels,
    })
}

/// A channel at the speaker's position, with its installation ranges (the position without one).
fn channel(speaker: &Speaker) -> Result<Channel, Failure> {
    if speaker.label.len() >= 32 {
        return Err(mradm_ear::Error::InvalidArgument("EAR string too long").into());
    }
    let position = [
        f64::from(speaker.azimuth),
        f64::from(speaker.elevation),
        1.0,
    ];
    let range = |r: Option<[f32; 2]>, v: f32| r.unwrap_or([v, v]).map(f64::from);
    Ok(Channel {
        name: speaker.label.to_owned(),
        real: position,
        nominal: position,
        azimuth_range: range(speaker.azimuth_range, speaker.azimuth),
        elevation_range: range(speaker.elevation_range, speaker.elevation),
        lfe: speaker.is_lfe,
    })
}

fn custom(spec: &layouts::Layout) -> Result<Layout, Failure> {
    Ok(Layout {
        name: spec.id.to_owned(),
        channels: spec
            .speakers
            .iter()
            .map(channel)
            .collect::<Result<_, _>>()?,
    })
}

/// Keeps EAR's nominal positions and triangulation and replaces the real coordinates and ranges.
fn effective_positions(mut layout: Layout, effective: &layouts::Layout) -> Result<Layout, Failure> {
    for ch in &mut layout.channels {
        let Some(speaker) = effective.speakers.iter().find(|s| s.label == ch.name) else {
            return Err(Failure::Unsupported(
                format!(
                    "speaker '{}' is missing from geometry profile '{}'",
                    ch.name, effective.id
                )
                .into_bytes(),
            ));
        };
        let real = channel(speaker)?;
        ch.real = real.real;
        ch.azimuth_range = real.azimuth_range;
        ch.elevation_range = real.elevation_range;
    }
    Ok(layout)
}

/// 4+5+4 and 9.1.6 are project layouts, not EAR's.
fn project_layout(id: &str) -> bool {
    id == "4+5+4" || id == "9.1.6"
}

/// The EAR layout of an output layout id. `wav71` renders as `0+7+0` and is reordered after.
pub fn layout(layout_id: &[u8], geometry: Geometry) -> Result<Layout, Error> {
    let build = || -> Result<Layout, Failure> {
        let id = std::str::from_utf8(layout_id).ok();
        if geometry == Geometry::Apple {
            let Some(effective) = id.and_then(|id| layouts::find(id, Geometry::Apple)) else {
                return Err(Failure::Unsupported(
                    [
                        b"Apple speaker geometry is unavailable for layout '",
                        layout_id,
                        b"'",
                    ]
                    .concat(),
                ));
            };
            let id = effective.id;
            let nominal = if id == "wav71" {
                standard("0+7+0")?
            } else if project_layout(id) {
                let Some(spec) = layouts::find(id, Geometry::Standard) else {
                    return Err(Failure::Unsupported(
                        format!("standard nominal layout '{id}' is unavailable").into_bytes(),
                    ));
                };
                custom(spec)?
            } else {
                standard(id)?
            };
            return effective_positions(nominal, effective);
        }
        let Some(id) = id else {
            return Err(mradm_ear::Error::InvalidArgument("Invalid EAR UTF-8").into());
        };
        if id == "wav71" {
            return standard("0+7+0");
        }
        if project_layout(id)
            && let Some(spec) = layouts::find(id, Geometry::Standard)
        {
            return custom(spec);
        }
        standard(id)
    };
    build().map_err(|f| f.at(layout_id))
}

struct GainBlock {
    gains: Vec<f64>,
    diffuse: Vec<f64>,
    start: u64,
    end: u64,
    jump: bool,
    smoothable: bool,
    interpolation: Option<u64>,
}

#[derive(Default)]
struct ChannelGains {
    object_id: Vec<u8>,
    label_key: Vec<u8>,
    blocks: Vec<GainBlock>,
}

fn is_lfe_key(key: &[u8]) -> bool {
    matches!(key, b"LFE" | b"LFE1" | b"LFE2" | b"LFEL" | b"LFER")
}

/// The DirectSpeakers metadata EAR reads: an LFE block names only its LFE, `LFE2` or `LFE1`.
fn direct_metadata(
    scene: &RenderScene,
    ds: &DirectBlock,
    has_position: bool,
    lfe: bool,
) -> DirectSpeakers {
    let s = &scene.snapshot;
    let ds_labels = labels(scene, ds.labels);
    let labels: Vec<String> = if lfe {
        let second = ds_labels.iter().any(|l| {
            let key = routing::label_key(l);
            key.windows(4).any(|w| w == b"LFE2") || key == b"LFER"
        });
        vec![if second { "LFE2" } else { "LFE1" }.to_owned()]
    } else {
        ds_labels
            .iter()
            .map(|l| String::from_utf8_lossy(l).into_owned())
            .collect()
    };
    // Label-less packs use positional routing, including custom pack IDs.
    let pack = &s.strings[ds.pack_id as usize];
    let mut meta = DirectSpeakers {
        labels,
        pack: (!pack.is_empty() && !ds_labels.is_empty()).then(|| pack.clone()),
        position: [0., 0., 1.],
        ..DirectSpeakers::default()
    };
    if has_position {
        meta.position = [ds.azimuth, ds.elevation, ds.distance].map(f64::from);
        let bounds = [
            ds.azimuth_min,
            ds.azimuth_max,
            ds.elevation_min,
            ds.elevation_max,
            ds.distance_min,
            ds.distance_max,
        ];
        for (i, value) in bounds.into_iter().enumerate() {
            if ds.bounds_present & (1 << i) != 0 {
                meta.bounds[i] = Some(f64::from(value));
            }
        }
    }
    if ds.low_pass_present != 0 || lfe {
        meta.low_pass = Some(if ds.low_pass_present != 0 {
            f64::from(ds.low_pass)
        } else {
            120.
        });
    }
    meta
}

fn direct_speakers(
    calculator: &Calculator,
    meta: &DirectSpeakers,
    log: &mut Log,
) -> Result<Vec<f64>, mradm_ear::Error> {
    let frequency_lfe = meta.low_pass.is_some_and(|f| f <= 200.);
    let label_lfe = meta
        .labels
        .iter()
        .any(|l| is_lfe_key(&routing::label_key(l.as_bytes())));
    if meta.low_pass.is_some() && !frequency_lfe {
        log(
            Level::Warning,
            b"frequency indication present but does not indicate an LFE channel".to_vec(),
        );
    }
    if !meta.labels.is_empty() && frequency_lfe != label_lfe {
        log(
            Level::Warning,
            b"LFE indication from frequency element does not match speakerLabel".to_vec(),
        );
    }
    calculator.direct_speakers(meta)
}

/// A linear port of the C++ gain-table preparation keeps the block precedence visible.
fn gain_matrix(
    r: &Request,
    layout: Layout,
    matrix: Option<&[ResolvedRoute]>,
    lfe: &LfeRouting,
    log: &mut Log,
) -> Result<(Calculator, BTreeMap<u16, ChannelGains>), Failure> {
    let scene = r.scene;
    let s = &scene.snapshot;
    let speakers: Vec<SceneSpeaker> = layout
        .channels
        .iter()
        .map(|c| SceneSpeaker {
            azimuth: c.real[0] as f32,
            elevation: c.real[1] as f32,
            is_lfe: u32::from(c.lfe),
        })
        .collect();
    let mut calculator = Calculator::new(layout)?;
    let outputs = speakers.len();
    let mut screen_ref_warned = false;
    let mut by_channel: BTreeMap<u16, ChannelGains> = BTreeMap::new();
    for (oi, object) in s.objects.iter().enumerate() {
        if scene.overlay.objects[oi].mute {
            continue;
        }
        let object_gain = f64::from(object.gain);
        for track in span(&s.tracks, object.tracks) {
            if track.channel < 0 {
                continue;
            }
            let channel = by_channel.entry(track.channel as u16).or_default();
            channel.object_id = s.strings[object.id as usize].as_bytes().to_vec();
            let direct = span(&s.direct_blocks, track.ds_blocks);
            let direct_states = span(&scene.overlay.direct_blocks, track.ds_blocks);
            let in_effect_direct = in_effect(direct, |b| b.start);
            // Bind live controls from the same resolved blocks used to build the channel's gains.
            // An overwritten block must not leave its speaker label behind.
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
                    &speakers,
                    &mut screen_ref_warned,
                    &mut |text| log(Level::Warning, text.as_bytes().to_vec()),
                );
                let mut gains = vec![0.; outputs];
                let mut diffuse = vec![0.; outputs];
                for source in &prepared.sources {
                    let [azimuth, elevation, distance] = scene::polar(&source.position);
                    let (d, f) = calculator.objects(Object {
                        position: [azimuth, elevation, distance].map(f64::from),
                        width: f64::from(source.width),
                        height: f64::from(source.height),
                        depth: f64::from(source.depth),
                        gain: f64::from(source.gain) * object_gain,
                        diffuse: f64::from(source.diffuse),
                    })?;
                    if ENABLED {
                        let p = &source.position;
                        let input = [
                            p.azimuth,
                            p.elevation,
                            p.distance,
                            source.width,
                            source.height,
                            source.depth,
                            source.gain,
                            source.diffuse,
                        ];
                        let _ = process("ear.01-object-input.f32", Words::F32(&input));
                        let _ = process("ear.02-direct.f64", Words::F64(&d));
                        let _ = process("ear.03-diffuse.f64", Words::F64(&f));
                    }
                    for c in 0..outputs {
                        gains[c] += d[c];
                        diffuse[c] += f[c];
                    }
                }
                channel.blocks.push(GainBlock {
                    gains,
                    diffuse,
                    start: prepared.start,
                    end: prepared.end,
                    jump: prepared.jump,
                    smoothable: true,
                    interpolation: prepared.interpolation,
                });
            }

            for b in in_effect_direct {
                let ds = &direct[b];
                let target =
                    routing::lfe_target(&labels(scene, ds.labels), ds.low_pass_present != 0);
                let has_position = direct_states[b].has_position;
                let mut gains = vec![0.; outputs];
                if lfe.applies_to_22_2 && target != LfeTarget::None {
                    gains[routing::LFE1_22_2] = f64::from(lfe.gain(target, LfeTarget::Lfe1));
                    gains[routing::LFE2_22_2] = f64::from(lfe.gain(target, LfeTarget::Lfe2));
                } else if target != LfeTarget::None {
                    let meta = direct_metadata(scene, ds, has_position, true);
                    gains = direct_speakers(&calculator, &meta, log)?;
                } else if let Some(resolved) = matrix {
                    let route = routing::matrix_route(resolved, &labels(scene, ds.labels))
                        .map_err(Failure::Routing)?;
                    for t in &resolved[route].targets {
                        gains[t.output] = f64::from(t.gain);
                    }
                } else {
                    let meta = direct_metadata(scene, ds, has_position, false);
                    gains = direct_speakers(&calculator, &meta, log)?;
                }
                let ds_gain = f64::from(ds.gain) * object_gain;
                gains.iter_mut().for_each(|g| *g *= ds_gain);
                channel.blocks.push(GainBlock {
                    gains,
                    diffuse: vec![0.; outputs],
                    start: ds.start,
                    end: ds.end.min(object.end),
                    jump: true,
                    smoothable: false,
                    interpolation: None,
                });
            }
        }
    }

    for pack in &s.hoa {
        if pack.mute != 0 || pack.channels.len == 0 {
            continue;
        }
        if pack.screen_ref != 0 {
            log(
                Level::Warning,
                b"screenRef for HOA is not implemented; ignoring".to_vec(),
            );
        }
        if pack.nfc_ref_dist != 0. {
            log(
                Level::Warning,
                b"nfcRefDist is not implemented; ignoring".to_vec(),
            );
        }
        let channels = span(&s.hoa_channels, pack.channels);
        let orders: Vec<i32> = channels.iter().map(|c| c.order).collect();
        let degrees: Vec<i32> = channels.iter().map(|c| c.degree).collect();
        let norm = match s.strings[pack.normalization as usize].as_str() {
            "N3D" => Normalization::N3d,
            "SN3D" => Normalization::Sn3d,
            "FuMa" => Normalization::Fuma,
            _ => {
                return Err(mradm_ear::Error::Unsupported("Unknown HOA normalization").into());
            }
        };
        let decode = calculator.hoa(&orders, &degrees, norm)?;
        if ENABLED {
            for (row, gains) in decode.chunks(outputs).enumerate() {
                let _ = process(&format!("ear.hoa-decode-row-{row}.f64"), Words::F64(gains));
            }
        }
        let pack_gain = f64::from(pack.gain);
        for (i, ch) in channels.iter().enumerate() {
            if ch.channel < 0 {
                continue;
            }
            let channel = by_channel.entry(ch.channel as u16).or_default();
            let row = &decode[i * outputs..(i + 1) * outputs];
            let blocks = span(&s.hoa_blocks, ch.blocks);
            for b in in_effect(blocks, |b| b.start) {
                let block = &blocks[b];
                let gain = f64::from(block.gain) * pack_gain;
                channel.blocks.push(GainBlock {
                    gains: row.iter().map(|g| g * gain).collect(),
                    diffuse: vec![0.; outputs],
                    start: block.start,
                    end: block.end,
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
    Ok((calculator, by_channel))
}

/// Prepares the gain plan and filters; `log` receives the `ear` lines in order, also before an
/// error.
pub fn prepare(r: &Request, log: &mut Log) -> Result<Prepared, Error> {
    let lfe = scene::lfe_routing(r.scene, r.split_power, r.layout_id, log)?;
    let layout = layout(r.layout_id, r.geometry)?;
    log(
        Level::Info,
        format!(
            "speaker geometry: {}",
            if r.geometry == Geometry::Apple {
                "apple"
            } else {
                "standard"
            }
        )
        .into_bytes(),
    );
    let mut matrix = None;
    if r.matrix_routing {
        let Some(m) = r.matrix else {
            return Err(error(
                1,
                "DirectSpeakers matrix routing requires a parsed matrix",
                Vec::new(),
            ));
        };
        let targets: Vec<Target> = layout
            .channels
            .iter()
            .map(|c| Target {
                label: c.name.as_bytes(),
                is_lfe: c.lfe,
            })
            .collect();
        matrix = Some(routing::resolve_matrix(m, &targets, r.layout_id)?);
        log(Level::Info, b"DirectSpeakers routing: matrix".to_vec());
    }
    let (calculator, channels) =
        gain_matrix(r, layout, matrix.as_deref(), &lfe, log).map_err(|f| f.at(r.layout_id))?;
    if channels.is_empty() {
        log(
            Level::Warning,
            b"no renderable tracks found (all muted?), writing silence".to_vec(),
        );
    }
    let inputs = r.scene.info.channels;
    if let Some(&channel) = channels.keys().find(|&&c| c >= inputs) {
        return Err(error(
            4,
            format!("track channel index {channel} is outside input channel count {inputs}"),
            [b"input=", r.input_path].concat(),
        ));
    }
    let outputs = calculator.layout().channels.len();
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
            coefficients.extend_from_slice(&b.diffuse);
        }
        bindings.push(Binding {
            input_channel,
            object_id: channel.object_id,
            label_key: channel.label_key,
        });
    }
    let dsp = |e: mradm_dsp::Error, context: &[u8]| error(dsp_code(&e), e.to_string(), context);
    let plan = Plan::new(
        usize::from(inputs),
        outputs,
        rows,
        blocks,
        Coefficients::Ear(coefficients),
    )
    .map_err(|e| dsp(e, b"PCM mix preparation"))?;
    let filters = FilterBank::new(outputs, &calculator.filters(), DELAY)
        .map_err(|e| dsp(e, b"EAR filter preparation"))?;
    Ok(Prepared {
        plan: Arc::new(plan),
        filters: Arc::new(filters),
        bindings,
        outputs,
    })
}
