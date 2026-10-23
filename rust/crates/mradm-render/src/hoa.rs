//! The HOA3 encoder backend (`hoa-encode`, PCM consistency 4.5): the per-channel encoder blocks
//! of Objects (position offset and divergence; extent and diffuse in the coefficients) and
//! DirectSpeakers (nominal position, or the position its BS.2051 label names; LFE W-only),
//! compiled into one HOA plan. Moved from the C++ `hoa_renderer` with the same block precedence,
//! checkpoints, warnings and errors; the capabilities stay with the C++ adapter.
use crate::offline::dsp_code;
use crate::routing::{self, Error, Level};
use crate::scene::{self, Log, in_effect, labels, span};
use crate::vbap::Binding;
use mradm_adm::records::ObjectBlock;
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::hoa::{BlockInput, HAS_INTERPOLATION, JUMP, Plan, RowInput, Source};
use mradm_scene::RenderScene;
use std::collections::BTreeMap;
use std::sync::Arc;

pub struct Request<'a> {
    pub scene: &'a RenderScene,
    pub layout_id: &'a [u8],
    pub split_power: bool,
}

pub struct Prepared {
    pub plan: Arc<Plan>,
    pub bindings: Vec<Binding>,
}

fn error(code: i32, message: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: Vec::new(),
    }
}

/// A BS.2051 positional label (`M+030`, `U-045`, `T+000`, `B+000`) as azimuth and elevation in
/// degrees; trailing characters after the azimuth digits are rejected.
pub fn label_direction(label: &[u8]) -> Option<[f32; 2]> {
    if label.len() < 5 {
        return None;
    }
    let elevation = match label[0] {
        b'M' => 0.,
        b'U' => 30.,
        b'T' => 90.,
        b'B' => -30.,
        _ => return None,
    };
    let sign = match label[1] {
        b'+' => 1.,
        b'-' => -1.,
        _ => return None,
    };
    let azimuth = mradm_dsp::decimal::parse_f32(std::str::from_utf8(&label[2..]).ok()?)?;
    Some([azimuth * sign, elevation])
}

fn source(block: &ObjectBlock) -> Source {
    Source {
        position: scene::coordinates(&block.position),
        cartesian: u32::from(block.position.cartesian != 0),
        width: block.width,
        height: block.height,
        depth: block.depth,
        gain: block.gain,
        diffuse: block.diffuse,
    }
}

#[derive(Default)]
struct Channel {
    binding: Binding,
    /// Indices into the blocks in scene traversal order, with their starts.
    order: Vec<(usize, u64)>,
}

/// Prepares the HOA plan; `log` receives the `hoa-encode` lines in order, also before an error.
pub fn prepare(r: &Request, log: &mut Log) -> Result<Prepared, Error> {
    let scene = r.scene;
    scene::lfe_routing(scene, r.split_power, r.layout_id, log)?;
    if r.layout_id != b"hoa3" {
        return Err(error(
            2,
            [
                b"unsupported HOA output layout '",
                r.layout_id,
                b"'; supported: hoa3",
            ]
            .concat(),
        ));
    }
    if scene.info.sample_rate == 0 {
        return Err(error(1, "HOA requires a nonzero sample rate"));
    }
    let s = &scene.snapshot;
    let inputs = scene.info.channels;
    let mut by_channel: BTreeMap<u16, Channel> = BTreeMap::new();
    let mut blocks = Vec::new();
    let mut sources = Vec::new();
    for (oi, object) in s.objects.iter().enumerate() {
        if scene.overlay.objects[oi].mute {
            continue;
        }
        for track in span(&s.tracks, object.tracks) {
            if track.channel < 0 {
                continue;
            }
            let input = track.channel as u16;
            if input >= inputs {
                return Err(error(
                    4,
                    format!("track channel index {input} is outside input channel count {inputs}"),
                ));
            }
            let channel = by_channel.entry(input).or_default();
            // Each track rebinds the channel; a later track's blocks still add to it.
            channel.binding = Binding {
                input_channel: input,
                object_id: s.strings[object.id as usize].as_bytes().to_vec(),
                label_key: Vec::new(),
            };
            let direct = span(&s.direct_blocks, track.ds_blocks);
            let direct_states = span(&scene.overlay.direct_blocks, track.ds_blocks);
            let in_effect_direct = in_effect(direct, |b| b.start);
            // Bind live controls after resolving overwritten labels, just like the channel's gains.
            if let Some(&first) = in_effect_direct.first()
                && let Some(label) = labels(scene, direct[first].labels).first()
            {
                channel.binding.label_key = routing::canonical_label(label);
            }
            let object_blocks = span(&s.object_blocks, track.blocks);
            for b in in_effect(object_blocks, |b| b.start) {
                let raw = &object_blocks[b];
                let mut base = *raw;
                base.position = scene::offset(base.position, object);
                let expanded = scene::divergence(&base);
                let offset = sources.len();
                sources.extend(expanded.iter().map(source));
                channel.order.push((blocks.len(), raw.start));
                let interpolation = raw.interpolation_present != 0;
                blocks.push(BlockInput {
                    start: raw.start,
                    end: raw.end.min(object.end),
                    interpolation: if interpolation { raw.interpolation } else { 0 },
                    source_offset: offset,
                    source_count: expanded.len(),
                    object_gain: object.gain,
                    kind: 0,
                    flags: if raw.jump != 0 { JUMP } else { 0 }
                        | if interpolation { HAS_INTERPOLATION } else { 0 },
                });
            }
            for b in in_effect_direct {
                let ds = &direct[b];
                let ds_labels = labels(scene, ds.labels);
                let lfe = routing::lfe_target(&ds_labels, ds.low_pass_present != 0)
                    != routing::LfeTarget::None;
                let mut direction = [ds.azimuth, ds.elevation];
                if !lfe && !direct_states[b].has_position {
                    let Some(found) = ds_labels.iter().find_map(|l| label_direction(l)) else {
                        log(
                            Level::Warning,
                            format!(
                                "DirectSpeakers channel {input} has no position and no parseable \
                                 label; skipping"
                            )
                            .into_bytes(),
                        );
                        continue;
                    };
                    direction = found;
                }
                let offset = sources.len();
                sources.push(Source {
                    position: [direction[0], direction[1], 1.],
                    gain: ds.gain,
                    ..Source::default()
                });
                channel.order.push((blocks.len(), ds.start));
                blocks.push(BlockInput {
                    start: ds.start,
                    end: ds.end.min(object.end),
                    interpolation: 0,
                    source_offset: offset,
                    source_count: 1,
                    object_gain: object.gain,
                    kind: if lfe { 2 } else { 1 },
                    flags: JUMP,
                });
            }
        }
    }
    let mut rows = Vec::with_capacity(by_channel.len());
    let mut order = Vec::with_capacity(blocks.len());
    let mut bindings = Vec::with_capacity(by_channel.len());
    for (input, mut channel) in by_channel {
        // Blocks sharing a start keep their scene (ADM document) order; the last one is in effect.
        channel.order.sort_by_key(|&(_, start)| start);
        rows.push(RowInput {
            input: usize::from(input),
            block_offset: order.len(),
            block_count: channel.order.len(),
        });
        order.extend(channel.order.iter().map(|&(index, _)| index));
        bindings.push(channel.binding);
    }
    let (plan, trace) =
        Plan::new(usize::from(inputs), &rows, &blocks, &order, &sources).map_err(|e| Error {
            code: dsp_code(&e),
            message: e.to_string().into_bytes(),
            context: b"HOA DSP".to_vec(),
        })?;
    if ENABLED {
        if trace.flags & 1 != 0 {
            let _ = process("hoa.01-polar.f32", Words::F32(&trace.polar));
        }
        if trace.flags & 2 != 0 {
            let _ = process("hoa.02-direction.f32", Words::F32(&trace.direction));
            let _ = process("hoa.03-normalized.f32", Words::F32(&trace.normalized));
            let _ = process("hoa.04-coefficients.f32", Words::F32(&trace.coefficients));
        }
    }
    if bindings.is_empty() {
        log(
            Level::Warning,
            b"no renderable tracks found (all muted?), writing silence".to_vec(),
        );
    }
    Ok(Prepared {
        plan: Arc::new(plan),
        bindings,
    })
}
