//! Per-emitter short paths and one shared world-space late field. Input gain and
//! ADM semantic expansion belong to the session adapter, not the room model.
use super::{
    BLOCK,
    geometry::{Position, Rotation, WeightedPosition},
    partition::{Dynamic, InputHistory, Long, LongReader, SharedLongHistory},
    prepared::{PreparedRoom, ResponseWork},
};
use crate::{Error, Result};
use rayon::prelude::*;
use std::sync::{Arc, Mutex};

pub struct Emitter {
    room: Arc<PreparedRoom>,
    direct_eq: Long,
    wet_eq: Long,
    eq_shared: bool,
    direct_history: InputHistory,
    wet_history: InputHistory,
    direct: Dynamic,
    early: Dynamic,
    send: Dynamic,
    send_impulse: Vec<f32>,
    direct_input: [f32; BLOCK],
    wet_input: [f32; BLOCK],
    scaled: [f32; BLOCK],
    pub position: Position,
    initialized: bool,
    early_initialized: bool,
    render_direct: bool,
    orientation: [f32; 3],
}

impl Emitter {
    pub fn new(room: Arc<PreparedRoom>, position: Position) -> Result<Self> {
        Ok(Self {
            direct_eq: Long::new(room.eq.clone())?,
            wet_eq: Long::new(room.eq.clone())?,
            eq_shared: true,
            direct_history: InputHistory::new(room.short_taps.max(room.send_taps))?,
            wet_history: InputHistory::new(room.short_taps.max(room.send_taps))?,
            direct: Dynamic::reader(2, room.short_taps)?,
            early: Dynamic::reader(2, room.short_taps)?,
            send: Dynamic::reader(1, room.send_taps)?,
            send_impulse: vec![0.; room.send_taps],
            direct_input: [0.; BLOCK],
            wet_input: [0.; BLOCK],
            scaled: [0.; BLOCK],
            room,
            position,
            initialized: false,
            early_initialized: false,
            render_direct: true,
            orientation: [0.; 3],
        })
    }

    pub fn reset(&mut self) {
        self.direct_eq.reset();
        self.wet_eq.reset();
        self.direct_history.reset();
        self.wet_history.reset();
        self.direct.reset();
        self.early.reset();
        self.send.reset();
        self.eq_shared = true;
        self.initialized = false;
        self.early_initialized = false;
        self.render_direct = true;
    }

    pub fn update(
        &mut self,
        position: Position,
        orientation: [f32; 3],
        work: &mut ResponseWork,
    ) -> Result<()> {
        self.update_sources(
            position,
            &[WeightedPosition {
                position,
                weight: 1.,
            }],
            orientation,
            false,
            true,
            work,
        )
    }
    pub fn update_sources(
        &mut self,
        position: Position,
        sources: &[WeightedPosition],
        orientation: [f32; 3],
        force: bool,
        early: bool,
        work: &mut ResponseWork,
    ) -> Result<()> {
        if self.initialized
            && !force
            && position.point == self.position.point
            && orientation == self.orientation
            && (!early || self.early_initialized)
        {
            return Ok(());
        }
        let moved = force || !self.initialized || position.point != self.position.point;
        work.build_branches(
            &self.room,
            sources,
            &Rotation::new(orientation)?,
            self.render_direct,
            early,
        )?;
        let taps = self.room.short_taps;
        if self.render_direct {
            self.direct
                .set_target_support(&work.impulse[..2 * taps], work.direct_support)?;
        }
        if early {
            self.early
                .set_target_support(&work.impulse[2 * taps..], work.early_support)?;
            self.early_initialized = true;
        }
        if moved {
            self.room.late_send_many(sources, &mut self.send_impulse)?;
            self.send.set_target(&self.send_impulse)?;
        }
        self.position = position;
        self.orientation = orientation;
        self.initialized = true;
        Ok(())
    }

    /// Overwrite direct / early stereo planes and mono late send. The wet send
    /// scales excitation before EQ, so a room switch retains the old EQ tail.
    pub fn process(
        &mut self,
        input: &[f32],
        wet_gain: &[f32],
        direct: &mut [f32],
        early: &mut [f32],
        send: &mut [f32],
    ) -> Result<()> {
        if !self.initialized {
            return Err(Error::InvalidArgument("Unprepared room emitter"));
        }
        self.begin(input, wet_gain)?;
        self.render_range(0, BLOCK, direct, early, send)
    }
    pub fn begin(&mut self, input: &[f32], wet_gain: &[f32]) -> Result<()> {
        self.begin_branches(input, wet_gain, true)
    }
    /// Once an outgoing room has completed its direct fade, that branch cannot
    /// become audible again. Wet EQ and all reflected/send history still run.
    pub fn begin_branches(&mut self, input: &[f32], wet_gain: &[f32], direct: bool) -> Result<()> {
        let _profile = super::profile::Guard::new(super::profile::Stage::EmitterBegin);
        if input.len() != BLOCK || wet_gain.len() != BLOCK {
            return Err(Error::InvalidArgument(
                "Unprepared room emitter or invalid block",
            ));
        }
        let unity = wet_gain.iter().all(|v| *v == 1.);
        if self.eq_shared && (!unity || !direct) {
            self.wet_eq.copy_history_from(&self.direct_eq)?;
            self.wet_history.copy_history_from(&self.direct_history)?;
            self.eq_shared = false;
        }
        self.render_direct = direct;
        if direct {
            self.direct_eq.process(input, &mut self.direct_input)?;
            self.direct
                .begin_with_history(&mut self.direct_history, &self.direct_input)?;
        }
        if !self.eq_shared {
            for i in 0..BLOCK {
                self.scaled[i] = input[i] * wet_gain[i];
            }
            self.wet_eq.process(&self.scaled, &mut self.wet_input)?;
            self.early
                .begin_with_history(&mut self.wet_history, &self.wet_input)?;
        } else {
            self.early.begin_shared();
        }
        self.send.begin_shared();
        Ok(())
    }
    pub fn render_range(
        &mut self,
        offset: usize,
        frames: usize,
        direct: &mut [f32],
        early: &mut [f32],
        send: &mut [f32],
    ) -> Result<()> {
        let _profile = super::profile::Guard::new(super::profile::Stage::EmitterConvolution);
        if self.render_direct {
            self.direct
                .render_shared_range(&self.direct_history, offset, frames, direct)?;
        } else {
            for ear in 0..2 {
                direct[ear * BLOCK + offset..ear * BLOCK + offset + frames].fill(0.);
            }
        }
        let history = if self.eq_shared {
            &self.direct_history
        } else {
            &self.wet_history
        };
        self.early
            .render_shared_range(history, offset, frames, early)?;
        self.send.render_shared_range(history, offset, frames, send)
    }
}

struct Decoder {
    convolution: Option<Dynamic>,
    query: Option<super::hrir::TimeQuery>,
    face: usize,
    filter: Vec<f32>,
    field: Option<LongReader>,
    input: [f32; BLOCK],
    output: [f32; BLOCK * 2],
}
#[derive(Clone, Copy, PartialEq, Eq)]
enum TailKind {
    Combined,
    Field,
    Decoder,
}
/// A decoder subrange with one head pose. Splitting only where the pose
/// changes is bit-identical to splitting at every control mark: an unchanged
/// pose never retargets, and subranges keep Dynamic's per-sample order.
#[derive(Clone, Copy)]
struct Segment {
    begin: usize,
    end: usize,
    changed: bool,
    rotation: Rotation,
}
struct Decoders {
    room: Arc<PreparedRoom>,
    decoders: Vec<Decoder>,
    orientation: [f32; 3],
    initialized: bool,
    changed: bool,
    begin: bool,
    offset: usize,
    frames: usize,
    source: [f32; BLOCK],
    history: Option<SharedLongHistory>,
    parallel: bool,
    kind: TailKind,
    segments: Vec<Segment>,
}
impl Decoders {
    fn render(&mut self) -> Result<()> {
        let _profile = super::profile::Guard::new(if self.kind == TailKind::Field {
            super::profile::Stage::LateField
        } else {
            super::profile::Stage::LateDecoder
        });
        if self.kind == TailKind::Field || (self.kind == TailKind::Combined && self.begin) {
            self.history.as_mut().unwrap().begin(&self.source)?;
        }
        if self.kind == TailKind::Field {
            let history = self.history.as_ref().unwrap();
            let process = |decoder: &mut Decoder| {
                decoder
                    .field
                    .as_mut()
                    .unwrap()
                    .render(history, &mut decoder.input)
            };
            if self.parallel && self.decoders.len() >= 8 {
                self.decoders.par_iter_mut().try_for_each(process)?;
            } else {
                self.decoders.iter_mut().try_for_each(process)?;
            }
            return Ok(());
        }
        let rotation = Rotation::new(self.orientation)?;
        let changed = self.changed;
        let begin = self.begin;
        let offset = self.offset;
        let frames = self.frames;
        let history = self.history.as_ref();
        let combined = self.kind == TailKind::Combined;
        let render = |(decoder, direction): (&mut Decoder, &[f64; 3])| -> Result<()> {
            if begin {
                if combined {
                    decoder
                        .field
                        .as_mut()
                        .unwrap()
                        .render(history.unwrap(), &mut decoder.input)?;
                }
                decoder
                    .convolution
                    .as_mut()
                    .unwrap()
                    .begin(&decoder.input)?;
            }
            if changed {
                decoder.query.as_mut().unwrap().query(
                    rotation.apply(*direction),
                    &mut decoder.face,
                    &mut decoder.filter,
                )?;
                decoder
                    .convolution
                    .as_mut()
                    .unwrap()
                    .set_target(&decoder.filter)?;
            }
            decoder
                .convolution
                .as_mut()
                .unwrap()
                .render_range(offset, frames, &mut decoder.output)
        };
        if self.parallel && self.decoders.len() >= 8 {
            self.decoders
                .par_iter_mut()
                .zip(self.room.profile.late_directions.par_iter())
                .try_for_each(render)?;
        } else {
            self.decoders
                .iter_mut()
                .zip(&self.room.profile.late_directions)
                .try_for_each(render)?;
        }
        self.begin = false;
        self.changed = false;
        Ok(())
    }
}
impl super::workers::Work for Mutex<Decoders> {
    fn run(&self) -> Result<()> {
        self.lock().unwrap().render()
    }
}

/// One world direction for one canonical block: the current and incoming room
/// fields, the serial drain/switch merge, and every head-pose segment of its
/// decoder. The per-direction arithmetic matches the former three-stage path.
fn render_direction(
    decoder: &mut Decoder,
    direction: &[f64; 3],
    current: (&mut Decoder, &SharedLongHistory, usize),
    mut incoming: Option<(&mut Decoder, &SharedLongHistory)>,
    segments: &[Segment],
) -> Result<()> {
    let (current, current_history, current_frames) = current;
    {
        let _profile = super::profile::Guard::new(super::profile::Stage::LateField);
        current
            .field
            .as_mut()
            .unwrap()
            .render(current_history, &mut current.input)?;
        if let Some((next, history)) = incoming.as_mut() {
            next.field
                .as_mut()
                .unwrap()
                .render(history, &mut next.input)?;
        }
    }
    for (i, value) in decoder.input.iter_mut().enumerate() {
        // A draining room contributes an exact zero after its final sample.
        *value = if i >= current_frames {
            0.
        } else {
            current.input[i]
        };
        if let Some((next, _)) = &incoming {
            *value += next.input[i];
        }
    }
    let Decoder {
        convolution,
        query,
        face,
        filter,
        input,
        output,
        ..
    } = decoder;
    let convolution = convolution.as_mut().unwrap();
    convolution.begin(input)?;
    for segment in segments {
        if segment.changed {
            query
                .as_mut()
                .unwrap()
                .query(segment.rotation.apply(*direction), face, filter)?;
            convolution.set_target(filter)?;
        }
        convolution.render_range(segment.begin, segment.end - segment.begin, output)?;
    }
    Ok(())
}
pub struct WorldTail {
    group: Arc<Mutex<Decoders>>,
    workers: Arc<super::workers::Workers>,
}
impl WorldTail {
    pub fn new(room: Arc<PreparedRoom>) -> Result<Self> {
        Self::new_kind(room, TailKind::Combined)
    }
    pub fn new_field(room: Arc<PreparedRoom>) -> Result<Self> {
        Self::new_kind(room, TailKind::Field)
    }
    pub fn new_decoder(room: Arc<PreparedRoom>) -> Result<Self> {
        Self::new_kind(room, TailKind::Decoder)
    }
    fn new_kind(room: Arc<PreparedRoom>, kind: TailKind) -> Result<Self> {
        let count = room.profile.late_directions.len();
        let decoders = (0..count)
            .map(|index| {
                Ok(Decoder {
                    convolution: if kind != TailKind::Field {
                        Some(Dynamic::new(2, room.hrir.data.output_taps)?)
                    } else {
                        None
                    },
                    field: if kind != TailKind::Decoder {
                        Some(LongReader::new(room.late[index].clone())?)
                    } else {
                        None
                    },
                    query: if kind != TailKind::Field {
                        Some(super::hrir::TimeQuery::new(room.decoder_hrir.clone())?)
                    } else {
                        None
                    },
                    face: usize::MAX,
                    filter: vec![
                        0.;
                        if kind != TailKind::Field {
                            room.hrir.data.output_taps * 2
                        } else {
                            0
                        }
                    ],
                    input: [0.; BLOCK],
                    output: [0.; BLOCK * 2],
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let mut result = Self {
            group: Arc::new(Mutex::new(Decoders {
                history: if kind != TailKind::Decoder {
                    Some(SharedLongHistory::new(&room.late[0])?)
                } else {
                    None
                },
                room,
                decoders,
                orientation: [0.; 3],
                initialized: false,
                changed: false,
                begin: false,
                offset: 0,
                frames: 0,
                source: [0.; BLOCK],
                parallel: true,
                kind,
                segments: Vec::with_capacity(BLOCK),
            })),
            workers: super::workers::shared()?,
        };
        // macOS initializes std::Mutex lazily. Touch it in preparation.
        result.reset();
        Ok(result)
    }
    pub fn reset(&mut self) {
        let mut group = self.group.lock().unwrap();
        for decoder in &mut group.decoders {
            if let Some(convolution) = &mut decoder.convolution {
                convolution.reset();
            }
            if let Some(field) = &mut decoder.field {
                field.reset();
            }
        }
        group.initialized = false;
        if let Some(history) = &mut group.history {
            history.reset();
        }
        group.changed = false;
        group.begin = false;
    }
    pub fn set_parallel(&mut self, enabled: bool) {
        self.group.lock().unwrap().parallel = enabled;
    }
    pub fn update(&mut self, orientation: [f32; 3]) -> Result<()> {
        let mut group = self.group.lock().unwrap();
        if group.kind == TailKind::Field {
            return Err(Error::InvalidArgument(
                "World field does not accept head pose",
            ));
        }
        Rotation::new(orientation)?;
        if !group.initialized || group.orientation != orientation {
            group.changed = true;
            group.orientation = orientation;
            group.initialized = true;
        }
        Ok(())
    }
    pub fn process(&mut self, input: &[f32], output: &mut [f32]) -> Result<()> {
        self.begin(input)?;
        self.render_range(0, BLOCK, output)
    }
    pub fn begin(&mut self, input: &[f32]) -> Result<()> {
        if input.len() != BLOCK {
            return Err(Error::InvalidArgument("Invalid late-field input"));
        }
        let mut group = self.group.lock().unwrap();
        if group.kind != TailKind::Combined {
            return Err(Error::InvalidArgument("Invalid combined late decoder"));
        }
        group.source.copy_from_slice(input);
        group.begin = true;
        Ok(())
    }
    pub fn process_field(&mut self, input: &[f32], output: &mut [f32]) -> Result<()> {
        let parallel;
        {
            let mut group = self.group.lock().unwrap();
            if group.kind != TailKind::Field
                || input.len() != BLOCK
                || output.len() != group.decoders.len() * BLOCK
            {
                return Err(Error::InvalidArgument("Invalid room field buffers"));
            }
            group.source.copy_from_slice(input);
            parallel = group.parallel && group.decoders.len() >= 8;
        }
        if parallel {
            self.workers.run(self.group.clone())?;
        } else {
            self.group.lock().unwrap().render()?;
        }
        let group = self.group.lock().unwrap();
        for (out, decoder) in output
            .as_chunks_mut::<BLOCK>()
            .0
            .iter_mut()
            .zip(&group.decoders)
        {
            out.copy_from_slice(&decoder.input);
        }
        Ok(())
    }
    /// Field kind: advance the shared input history for this block. Its
    /// directional FIRs are rendered by the decoder's `render_world` pass.
    pub fn begin_field(&mut self, input: &[f32]) -> Result<()> {
        let mut group = self.group.lock().unwrap();
        if group.kind != TailKind::Field || input.len() != BLOCK {
            return Err(Error::InvalidArgument("Invalid room field buffers"));
        }
        group.history.as_mut().unwrap().begin(input)
    }
    /// Decoder kind: one pass over world directions per canonical block. Each
    /// direction renders the current (and incoming) field after `begin_field`,
    /// keeps samples from `current_frames` onward at zero, and decodes each
    /// head-pose segment. Outputs are reduced afterwards in direction order.
    pub fn render_world(
        &mut self,
        current: &WorldTail,
        current_frames: usize,
        incoming: Option<&WorldTail>,
        marks: &[bool; BLOCK],
        poses: &[[f32; 3]; BLOCK],
        output: &mut [f32],
    ) -> Result<()> {
        let _profile = super::profile::Guard::new(super::profile::Stage::LateDecoder);
        // Never fall back to Rayon's global pool from a non-room thread.
        let in_pool = self.workers.in_pool();
        let mut world = self.group.lock().unwrap();
        let mut current = current.group.lock().unwrap();
        let mut incoming = incoming.map(|v| v.group.lock().unwrap());
        let count = world.decoders.len();
        if world.kind != TailKind::Decoder
            || current.kind != TailKind::Field
            || current.decoders.len() != count
            || incoming
                .as_ref()
                .is_some_and(|v| v.kind != TailKind::Field || v.decoders.len() != count)
            || output.len() != BLOCK * 2
            || current_frames > BLOCK
        {
            return Err(Error::InvalidArgument("Invalid room world decoder pass"));
        }
        let Decoders {
            room,
            decoders,
            orientation,
            initialized,
            segments,
            ..
        } = &mut *world;
        // Same pose updates as one `update` per control mark.
        segments.clear();
        for (i, pose) in poses.iter().enumerate() {
            if i != 0 && !marks[i] {
                continue;
            }
            Rotation::new(*pose)?;
            let changed = !*initialized || *orientation != *pose;
            if changed {
                *orientation = *pose;
                *initialized = true;
            }
            if i == 0 || changed {
                if let Some(last) = segments.last_mut() {
                    last.end = i;
                }
                segments.push(Segment {
                    begin: i,
                    end: BLOCK,
                    changed,
                    rotation: Rotation::new(*orientation)?,
                });
            }
        }
        let segments = segments.as_slice();
        let directions = &room.profile.late_directions;
        let Decoders {
            decoders: current_decoders,
            history: current_history,
            ..
        } = &mut *current;
        let current_history = current_history.as_ref().unwrap();
        let parallel = in_pool && count >= 8;
        if let Some(next) = incoming.as_deref_mut() {
            let Decoders {
                decoders: next_decoders,
                history: next_history,
                ..
            } = next;
            let next_history = next_history.as_ref().unwrap();
            let task = |(((decoder, direction), current), next): (
                ((&mut Decoder, &[f64; 3]), &mut Decoder),
                &mut Decoder,
            )| {
                render_direction(
                    decoder,
                    direction,
                    (current, current_history, current_frames),
                    Some((next, next_history)),
                    segments,
                )
            };
            if parallel {
                decoders
                    .par_iter_mut()
                    .zip(directions.par_iter())
                    .zip(current_decoders.par_iter_mut())
                    .zip(next_decoders.par_iter_mut())
                    .try_for_each(task)?;
            } else {
                decoders
                    .iter_mut()
                    .zip(directions)
                    .zip(current_decoders.iter_mut())
                    .zip(next_decoders.iter_mut())
                    .try_for_each(task)?;
            }
        } else {
            let task =
                |((decoder, direction), current): ((&mut Decoder, &[f64; 3]), &mut Decoder)| {
                    render_direction(
                        decoder,
                        direction,
                        (current, current_history, current_frames),
                        None,
                        segments,
                    )
                };
            if parallel {
                decoders
                    .par_iter_mut()
                    .zip(directions.par_iter())
                    .zip(current_decoders.par_iter_mut())
                    .try_for_each(task)?;
            } else {
                decoders
                    .iter_mut()
                    .zip(directions)
                    .zip(current_decoders.iter_mut())
                    .try_for_each(task)?;
            }
        }
        // Direction order is independent of worker scheduling.
        output.fill(0.);
        for decoder in decoders.iter() {
            for (destination, value) in output.iter_mut().zip(&decoder.output) {
                *destination += *value;
            }
        }
        world.begin = false;
        world.changed = false;
        Ok(())
    }
    pub fn begin_world(&mut self, input: &[f32]) -> Result<()> {
        let mut group = self.group.lock().unwrap();
        if group.kind != TailKind::Decoder || input.len() != group.decoders.len() * BLOCK {
            return Err(Error::InvalidArgument("Invalid room world decoder input"));
        }
        for (decoder, input) in group
            .decoders
            .iter_mut()
            .zip(input.as_chunks::<BLOCK>().0.iter())
        {
            decoder.input.copy_from_slice(input);
        }
        group.begin = true;
        Ok(())
    }
    pub fn render_range(&mut self, offset: usize, frames: usize, output: &mut [f32]) -> Result<()> {
        let count;
        {
            let mut group = self.group.lock().unwrap();
            if output.len() != BLOCK * 2
                || group.kind == TailKind::Field
                || !group.initialized
                || offset > BLOCK
                || frames > BLOCK - offset
            {
                return Err(Error::InvalidArgument("Unprepared room late decoder"));
            }
            group.offset = offset;
            group.frames = frames;
            count = if group.parallel {
                group.decoders.len()
            } else {
                0
            };
        }
        if count >= 8 {
            self.workers.run(self.group.clone())?;
        } else {
            self.group.lock().unwrap().render()?;
        }
        let group = self.group.lock().unwrap();
        for ear in 0..2 {
            output[ear * BLOCK + offset..ear * BLOCK + offset + frames].fill(0.);
        }
        // Direction order is independent of worker scheduling.
        for decoder in &group.decoders {
            for ear in 0..2 {
                let begin = ear * BLOCK + offset;
                for (destination, value) in output[begin..begin + frames]
                    .iter_mut()
                    .zip(&decoder.output[begin..begin + frames])
                {
                    *destination += *value;
                }
            }
        }
        Ok(())
    }
}

/// Preparation / verification helper. Returns interleaved stereo and never
/// includes reference-layout distance calibration. Audio callers use sessions.
pub fn impulse(
    room: Arc<PreparedRoom>,
    position: Position,
    orientation: [f32; 3],
) -> Result<Vec<f32>> {
    let mut work = ResponseWork::new(&room)?;
    let mut emitter = Emitter::new(room.clone(), position)?;
    let mut field = WorldTail::new(room.clone())?;
    // A background room preparation must never occupy the playback pool while
    // synthesizing its common LFE kernel. Arithmetic and reduction order agree.
    field.set_parallel(false);
    emitter.update(position, orientation, &mut work)?;
    field.update(orientation)?;
    let frames = room.history_frames() + 1;
    let mut result = vec![0.; frames * 2];
    let mut input = [0.; BLOCK];
    let wet_gain = [1.; BLOCK];
    let mut direct = [0.; BLOCK * 2];
    let mut early = [0.; BLOCK * 2];
    let mut send = [0.; BLOCK];
    let mut late = [0.; BLOCK * 2];
    input[0] = 1.;
    for start in (0..frames).step_by(BLOCK) {
        emitter.process(&input, &wet_gain, &mut direct, &mut early, &mut send)?;
        field.process(&send, &mut late)?;
        for i in 0..BLOCK.min(frames - start) {
            result[(start + i) * 2] = (direct[i] + early[i]) + late[i];
            result[(start + i) * 2 + 1] = (direct[BLOCK + i] + early[BLOCK + i]) + late[BLOCK + i];
        }
        input.fill(0.);
    }
    Ok(result)
}
