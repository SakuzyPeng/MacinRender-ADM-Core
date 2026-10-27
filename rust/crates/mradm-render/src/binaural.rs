//! The binaural source plan (PCM consistency 4.7 B), moved from the C++ `build_sources`,
//! `build_spreader_tracks` and `build_spreader_groups`: the 3 × 17 divergence × extent slots of
//! the HRTF convolution sources, the LFE bypass, the DirectSpeakers positions, and the SAF
//! spreader tracks and their adapter groups, with the same order, warnings and checkpoints.
//! Records are read as the C++ `AdmScene` copied them (record gains; the effective object mute
//! and DirectSpeakers position from the overlay).
use crate::routing::{self, Level, LfeTarget};
use crate::scene::{self, in_effect, labels, span};
use mradm_adm::records::{Object, ObjectBlock};
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::hrtf_filters::grid_index;
use mradm_dsp::scene_math::{self as math, CloudPoint, Speaker};
use mradm_dsp::stream_dsp::extent_spread_degrees;
use mradm_scene::RenderScene;
use std::collections::BTreeMap;

pub const MODULE: &str = "binaural";
/// Minimum extent cone (degrees) routed through the SAF spreader.
const SPREADER_THRESHOLD_DEG: f32 = 1.0;
const DIVERGENCE_SLOTS: usize = 3;
const CENTER_SLOT: usize = 1;
const EXTENT_SLOTS: usize = 17;
const SLOTS: usize = DIVERGENCE_SLOTS * EXTENT_SLOTS;
/// Spreader tracks pack into adapters by a fixed budget, not the core count (ADR 0017).
pub const GROUP_BUDGET: usize = 8;
/// Lanes of one OM spreader instance.
pub const MAX_LANES: usize = 8;

/// The binaural extent spread mode (`BinauralSpreadMode`; automatic is the cloud).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Spread {
    Cloud,
    None,
    SafSpreader,
}

/// Live topology scales of one object: diffuse, width, height, depth and divergence.
pub type Scales = [f32; 5];

/// A DirectSpeakers block with its labels and effective position flag.
#[derive(Clone, Debug)]
pub struct InputDirect {
    pub block: mradm_adm::records::DirectBlock,
    pub labels: Vec<Vec<u8>>,
    pub has_position: bool,
}

#[derive(Clone, Debug)]
pub struct InputTrack {
    pub channel: Option<u16>,
    pub uid: Vec<u8>,
    pub blocks: Vec<ObjectBlock>,
    pub direct: Vec<InputDirect>,
}

#[derive(Clone, Debug)]
pub struct InputObject {
    pub id: Vec<u8>,
    pub mute: bool,
    pub object: Object,
    pub tracks: Vec<InputTrack>,
}

/// What the plan reads of the scene, owned so the file stream can rebuild it with scales.
#[derive(Clone, Debug, Default)]
pub struct Input {
    pub objects: Vec<InputObject>,
    pub has_hoa: bool,
}

impl Input {
    pub fn of(scene: &RenderScene) -> Self {
        let s = &scene.snapshot;
        let objects = s
            .objects
            .iter()
            .enumerate()
            .map(|(oi, object)| InputObject {
                id: s.strings[object.id as usize].as_bytes().to_vec(),
                mute: scene.overlay.objects[oi].mute,
                object: *object,
                tracks: span(&s.tracks, object.tracks)
                    .iter()
                    .map(|track| {
                        let states = span(&scene.overlay.direct_blocks, track.ds_blocks);
                        InputTrack {
                            channel: u16::try_from(track.channel).ok(),
                            uid: s.strings[track.uid as usize].as_bytes().to_vec(),
                            blocks: span(&s.object_blocks, track.blocks).to_vec(),
                            direct: span(&s.direct_blocks, track.ds_blocks)
                                .iter()
                                .zip(states)
                                .map(|(block, state)| InputDirect {
                                    block: *block,
                                    labels: labels(scene, block.labels)
                                        .into_iter()
                                        .map(<[u8]>::to_vec)
                                        .collect(),
                                    has_position: state.has_position,
                                })
                                .collect(),
                        }
                    })
                    .collect(),
            })
            .collect();
        Self {
            objects,
            has_hoa: !s.hoa.is_empty(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Block {
    pub az: f32,
    pub el: f32,
    pub gain: f32,
    pub start: u64,
    pub end: u64,
    pub jump: bool,
    pub interpolation: Option<u64>,
    pub head_locked: bool,
}

/// One HRTF convolution (or LFE bypass) source.
#[derive(Clone, Debug, PartialEq)]
pub struct Source {
    pub channel: u16,
    /// The object gain.
    pub gain: f32,
    pub object_id: Vec<u8>,
    /// The canonical DirectSpeakers label (empty for Objects): the per-channel live gain key.
    pub label_key: Vec<u8>,
    pub bypass_lfe: bool,
    pub smoothable: bool,
    pub diffuse_bus: bool,
    pub blocks: Vec<Block>,
}

impl Source {
    fn new(channel: u16, gain: f32, object_id: &[u8]) -> Self {
        Self {
            channel,
            gain,
            object_id: object_id.to_vec(),
            label_key: Vec::new(),
            bypass_lfe: false,
            smoothable: false,
            diffuse_bus: false,
            blocks: Vec::new(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SubSource {
    pub az: f32,
    pub el: f32,
    pub spread: f32,
    pub gain: f32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SpreaderBlock {
    pub sources: Vec<SubSource>,
    pub start: u64,
    pub end: u64,
    pub head_locked: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Track {
    pub seed: u64,
    pub channel: u16,
    pub object_gain: f32,
    pub blocks: Vec<SpreaderBlock>,
    pub sources: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Lane {
    pub track_slot: usize,
    pub source: usize,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Group {
    pub tracks: Vec<usize>,
    pub lanes: Vec<Lane>,
}

pub type Log<'a> = dyn FnMut(Level, Vec<u8>) + 'a;

/// `std::clamp`: NaN passes through.
fn clamp(v: f32, lo: f32, hi: f32) -> f32 {
    if v < lo {
        lo
    } else if hi < v {
        hi
    } else {
        v
    }
}

/// channelLock needs a reproduction reference without loudspeakers: the ±30° stereo pair.
const LOCK_SPEAKERS: [Speaker; 2] = [
    Speaker {
        azimuth: 30.,
        elevation: 0.,
        is_lfe: 0,
    },
    Speaker {
        azimuth: -30.,
        elevation: 0.,
        is_lfe: 0,
    },
];

/// BS.2051 labels → (az, el); +az is left. Matched on the label as written.
fn label_position(label: &[u8]) -> Option<(f32, f32)> {
    const TABLE: [(&[u8], f32, f32); 25] = [
        (b"M+000", 0., 0.),
        (b"M+030", 30., 0.),
        (b"M-030", -30., 0.),
        (b"M+060", 60., 0.),
        (b"M-060", -60., 0.),
        (b"M+090", 90., 0.),
        (b"M-090", -90., 0.),
        (b"M+110", 110., 0.),
        (b"M-110", -110., 0.),
        (b"M+135", 135., 0.),
        (b"M-135", -135., 0.),
        (b"M+180", 180., 0.),
        (b"U+030", 30., 30.),
        (b"U-030", -30., 30.),
        (b"U+000", 0., 30.),
        (b"U+045", 45., 30.),
        (b"U-045", -45., 30.),
        (b"U+110", 110., 30.),
        (b"U-110", -110., 30.),
        (b"U+135", 135., 30.),
        (b"U-135", -135., 30.),
        (b"U+180", 180., 30.),
        (b"T+000", 0., 90.),
        (b"B+045", 45., -30.),
        (b"B-045", -45., -30.),
    ];
    TABLE
        .iter()
        .find(|(l, _, _)| *l == label)
        .map(|&(_, az, el)| (az, el))
}

fn block_position(block: &ObjectBlock) -> (f32, f32) {
    let p = scene::polar(&block.position);
    (p[0], p[1])
}

/// The object blocks scaled by a live topology edit, clamped to the ADM ranges.
fn scaled(blocks: &[ObjectBlock], scales: Option<&Scales>) -> Vec<ObjectBlock> {
    let mut blocks = blocks.to_vec();
    if let Some(s) = scales {
        for b in &mut blocks {
            let angular = if b.position.cartesian != 0 { 1. } else { 360. };
            b.diffuse = clamp(b.diffuse * s[0], 0., 1.);
            b.width = clamp(b.width * s[1], 0., angular);
            b.height = clamp(b.height * s[2], 0., angular);
            b.depth = clamp(b.depth * s[3], 0., 1.);
            b.divergence = clamp(b.divergence * s[4], 0., 1.);
        }
    }
    blocks
}

/// The extent cloud of a source block (one point without a cloud spread) as (az, el, gain, slot).
fn extent(block: &ObjectBlock, gain: f32, cloud: bool) -> Vec<(f32, f32, f32, usize)> {
    if !cloud {
        let (az, el) = block_position(block);
        return vec![(az, el, gain, 0)];
    }
    let p = &block.position;
    let mut points = [CloudPoint::default(); 17];
    let mut trace = [[0f32; 14]; 17];
    let count = math::cloud(
        scene::coordinates(p),
        p.cartesian != 0,
        [block.width, block.height, block.depth],
        gain,
        true,
        false,
        &mut points,
        ENABLED.then_some(&mut trace),
    );
    points[..count]
        .iter()
        .enumerate()
        .map(|(index, point)| {
            if ENABLED && count > 1 {
                let _ = process(
                    &format!("cloud.01-slot-{}.f32", point.slot),
                    Words::F32(&trace[index]),
                );
                let grid = grid_index(point.azimuth, point.elevation).unwrap_or(0) as i32;
                let _ = process(
                    &format!("cloud.02-grid-{}.i32", point.slot),
                    Words::I32(&[grid]),
                );
            }
            (
                point.azimuth,
                point.elevation,
                point.weight,
                point.slot as usize,
            )
        })
        .collect()
}

fn append(bank: Vec<Source>, sources: &mut Vec<Source>) {
    for mut source in bank {
        if !source.blocks.is_empty() {
            // Blocks sharing a start keep their scene order; the last one is in effect.
            source.blocks.sort_by_key(|b| b.start);
            sources.push(source);
        }
    }
}

fn warn(log: &mut Log, text: &str) {
    log(Level::Warning, text.as_bytes().to_vec());
}

#[allow(clippy::too_many_arguments)]
fn object_track_sources(
    object: &InputObject,
    blocks: &[ObjectBlock],
    channel: u16,
    screen_ref_warned: &mut bool,
    log: &mut Log,
    sources: &mut Vec<Source>,
    spread: Spread,
) {
    let bank = |diffuse_bus| {
        (0..SLOTS)
            .map(|_| Source {
                smoothable: true,
                diffuse_bus,
                ..Source::new(channel, object.object.gain, &object.id)
            })
            .collect::<Vec<_>>()
    };
    let (mut direct, mut diffuse) = (bank(false), bank(true));
    for b in in_effect(blocks, |b| b.start) {
        let prepared = scene::prepare_object_block(
            &blocks[b],
            &object.object,
            &LOCK_SPEAKERS,
            screen_ref_warned,
            &mut |text| warn(log, text),
        );
        let count = prepared.sources.len();
        for (index, source) in prepared.sources.iter().enumerate() {
            let divergence_slot = if count == DIVERGENCE_SLOTS {
                index
            } else {
                CENTER_SLOT
            };
            let d = clamp(source.diffuse, 0., 1.);
            let direct_scale = (1. - d).sqrt();
            let diffuse_scale = d.sqrt();
            let has_extent = extent_spread_degrees(source.width, source.height, source.depth)
                >= SPREADER_THRESHOLD_DEG;
            let cloud = spread == Spread::Cloud;
            let block = |az, el, gain| Block {
                az,
                el,
                gain,
                start: prepared.start,
                end: prepared.end,
                jump: prepared.jump,
                interpolation: prepared.interpolation,
                head_locked: source.head_locked != 0,
            };
            if direct_scale > 1.0e-4 && (spread != Spread::SafSpreader || !has_extent) {
                for (az, el, gain, slot) in extent(source, source.gain * direct_scale, cloud) {
                    direct[divergence_slot * EXTENT_SLOTS + slot]
                        .blocks
                        .push(block(az, el, gain));
                }
            }
            if diffuse_scale <= 1.0e-4 {
                continue;
            }
            for (az, el, gain, slot) in extent(source, source.gain * diffuse_scale, cloud) {
                diffuse[divergence_slot * EXTENT_SLOTS + slot]
                    .blocks
                    .push(block(az, el, gain));
            }
        }
    }
    append(direct, sources);
    append(diffuse, sources);
}

/// The HRTF convolution sources, in object, track, Objects-then-DirectSpeakers order.
pub fn sources(
    input: &Input,
    spread: Spread,
    scales: &BTreeMap<Vec<u8>, Scales>,
    log: &mut Log,
) -> Vec<Source> {
    let mut sources = Vec::new();
    let mut screen_ref_warned = false;
    for object in &input.objects {
        if object.mute {
            continue;
        }
        for track in &object.tracks {
            let Some(channel) = track.channel else {
                continue;
            };
            if !track.blocks.is_empty() {
                let blocks = scaled(&track.blocks, scales.get(&object.id));
                object_track_sources(
                    object,
                    &blocks,
                    channel,
                    &mut screen_ref_warned,
                    log,
                    &mut sources,
                    spread,
                );
            }
            for b in in_effect(&track.direct, |d| d.block.start) {
                let ds = &track.direct[b];
                let label_key = ds
                    .labels
                    .first()
                    .map_or_else(Vec::new, |l| routing::canonical_label(l));
                let refs: Vec<&[u8]> = ds.labels.iter().map(Vec::as_slice).collect();
                let lfe =
                    routing::lfe_target(&refs, ds.block.low_pass_present != 0) != LfeTarget::None;
                let end = ds.block.end.min(object.object.end);
                let mut source = Source {
                    label_key,
                    ..Source::new(channel, object.object.gain, &object.id)
                };
                let position = if lfe {
                    source.bypass_lfe = true;
                    Some((0., 0.))
                } else if ds.has_position {
                    Some((ds.block.azimuth, ds.block.elevation))
                } else {
                    ds.labels.iter().find_map(|l| label_position(l))
                };
                let Some((az, el)) = position else {
                    warn(
                        log,
                        &format!(
                            "DirectSpeakers channel {channel} has no resolvable position, skipping"
                        ),
                    );
                    continue;
                };
                source.blocks.push(Block {
                    az,
                    el,
                    gain: ds.block.gain,
                    start: ds.block.start,
                    end,
                    jump: true,
                    interpolation: None,
                    head_locked: ds.block.head_locked != 0,
                });
                sources.push(source);
            }
        }
    }
    if input.has_hoa {
        warn(
            log,
            "HOA tracks are not supported by the binaural renderer and will be skipped",
        );
    }
    sources
}

/// The SAF spreader tracks: extended direct sources per Objects track.
pub fn tracks(input: &Input, log: &mut Log) -> Vec<Track> {
    let mut tracks = Vec::new();
    let mut screen_ref_warned = false;
    for object in &input.objects {
        if object.mute {
            continue;
        }
        for track in &object.tracks {
            let Some(channel) = track.channel else {
                continue;
            };
            if track.blocks.is_empty() {
                continue;
            }
            let mut seed = 14695981039346656037u64;
            let identity = [
                object.id.as_slice(),
                b":",
                &track.uid,
                b":",
                channel.to_string().as_bytes(),
            ]
            .concat();
            for byte in identity {
                seed ^= u64::from(byte);
                seed = seed.wrapping_mul(1099511628211);
            }
            let mut st = Track {
                seed,
                channel,
                object_gain: object.object.gain,
                blocks: Vec::new(),
                sources: 1,
            };
            for b in in_effect(&track.blocks, |b| b.start) {
                let raw = &track.blocks[b];
                let prepared = scene::prepare_object_block(
                    raw,
                    &object.object,
                    &LOCK_SPEAKERS,
                    &mut screen_ref_warned,
                    &mut |text| warn(log, text),
                );
                let mut block = SpreaderBlock {
                    sources: Vec::new(),
                    start: prepared.start,
                    end: prepared.end,
                    head_locked: raw.head_locked != 0,
                };
                for source in &prepared.sources {
                    let d = clamp(source.diffuse, 0., 1.);
                    let direct_scale = (1. - d).sqrt();
                    let spread = extent_spread_degrees(source.width, source.height, source.depth);
                    if direct_scale > 1.0e-4 && spread >= SPREADER_THRESHOLD_DEG {
                        let (az, el) = block_position(source);
                        block.sources.push(SubSource {
                            az,
                            el,
                            spread,
                            gain: source.gain * direct_scale,
                        });
                    }
                }
                if !block.sources.is_empty() {
                    st.sources = st.sources.max(block.sources.len());
                    st.blocks.push(block);
                }
            }
            if !st.blocks.is_empty() {
                st.blocks.sort_by_key(|b| b.start);
                tracks.push(st);
            }
        }
    }
    tracks
}

/// Tracks packed into adapter groups: one per track unless the lanes far exceed the budget.
pub fn groups(tracks: &[Track], budget: usize) -> Vec<Group> {
    if tracks.is_empty() {
        return Vec::new();
    }
    let budget = budget.max(1);
    let lanes = |t: &Track| t.sources.min(MAX_LANES);
    let total: usize = tracks.iter().map(lanes).sum();
    let target = if total > budget * 2 {
        MAX_LANES.min(total.div_ceil(budget)).max(1)
    } else {
        1
    };
    let mut groups = Vec::new();
    let mut current = Group::default();
    for (index, track) in tracks.iter().enumerate() {
        let count = lanes(track);
        if count == 0 {
            continue;
        }
        if !current.lanes.is_empty() && current.lanes.len() + count > target {
            groups.push(std::mem::take(&mut current));
        }
        let track_slot = current.tracks.len();
        current.tracks.push(index);
        current
            .lanes
            .extend((0..count).map(|source| Lane { track_slot, source }));
        if current.lanes.len() >= target {
            groups.push(std::mem::take(&mut current));
        }
    }
    if !current.lanes.is_empty() {
        groups.push(current);
    }
    groups
}

/// The group budget: `MR_ADM_DIAGNOSTIC_GROUP_BUDGET` in diagnostic builds (1..=256).
pub fn group_budget() -> Result<usize, String> {
    diagnostic_count("MR_ADM_DIAGNOSTIC_GROUP_BUDGET", GROUP_BUDGET)
}

/// A diagnostic count override, as the C++ `consistency::count_override`.
pub fn diagnostic_count(variable: &str, fallback: usize) -> Result<usize, String> {
    if !ENABLED {
        return Ok(fallback);
    }
    match std::env::var_os(variable) {
        None => Ok(fallback),
        // `std::from_chars`: decimal digits only (no sign or space), the whole text.
        Some(value) => value
            .to_str()
            .filter(|v| !v.is_empty() && v.bytes().all(|b| b.is_ascii_digit()))
            .and_then(|v| v.parse::<usize>().ok())
            .filter(|v| (1..=256).contains(v))
            .ok_or_else(|| format!("{variable} must be an integer in [1, 256]")),
    }
}
