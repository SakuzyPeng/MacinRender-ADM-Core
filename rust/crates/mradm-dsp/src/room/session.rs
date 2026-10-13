//! Stateful source session with one canonical-block FIFO. The FIFO declares
//! 256 frames of processing latency; it makes fragmented input bit-identical.
use super::{
    BLOCK, FILTER_FADE,
    control::{self, Control, RoomCommand, RoomDescription, RoomOverride, State},
    geometry::{self, Position, WeightedPosition},
    partition::{Dynamic, Long, LongFilters},
    prepared::{PreparedRoom, ResponseWork},
    quality::Quality,
    signal::{self, Emitter, WorldTail},
    spatial::{MAX_SAMPLES, Shape},
};
use crate::{Error, Result, diffuse::DiffuseDelay};
use rayon::prelude::*;
use std::sync::{Arc, Mutex};

pub const MAX_SOURCES: usize = 512;

pub struct PreparedSet {
    pub room: Arc<PreparedRoom>,
    pub lfe: Arc<LongFilters>,
    workers: Arc<super::workers::Workers>,
}
impl PreparedSet {
    pub fn new(room: Arc<PreparedRoom>) -> Result<Self> {
        let reference = room.profile.reference_distance as f32;
        let source = geometry::position(&room.profile, [0., reference, 0.], 1., [1., 0., 0.])?;
        let response = signal::impulse(room.clone(), source, [0.; 3])?;
        let mean: Vec<_> = response
            .as_chunks::<2>()
            .0
            .iter()
            .map(|v| (v[0] + v[1]) * 0.5)
            .collect();
        let lfe = LongFilters::with_policy(1, &mean, room.quality.fft())?;
        Ok(Self {
            room,
            lfe,
            workers: super::workers::shared()?,
        })
    }
    pub fn tail_frames(&self) -> usize {
        (self.room.history_frames() + crate::diffuse::DELAY_LENGTH - 1).max(self.lfe.taps - 1)
            + BLOCK
    }
    pub fn preroll_frames(&self) -> usize {
        self.room
            .preroll_frames()
            .saturating_add(crate::diffuse::DELAY_LENGTH - 1)
            .max(self.lfe.history_frames() + BLOCK)
    }
}

struct HeadSignal {
    convolution: Dynamic,
    filter: Vec<f32>,
    query: Vec<f32>,
    shape: Option<Shape>,
    face: usize,
    previous: [f64; 3],
    until: u64,
}
impl HeadSignal {
    fn new(room: &PreparedRoom) -> Result<Self> {
        let taps = room.short_taps;
        Ok(Self {
            convolution: Dynamic::with_policy(2, taps, room.quality.fft())?,
            filter: vec![0.; 2 * taps],
            query: vec![0.; 2 * room.hrir.data.output_taps],
            shape: None,
            face: usize::MAX,
            previous: [1., 0., 0.],
            until: 0,
        })
    }
    fn reset(&mut self) {
        self.convolution.reset();
        self.shape = None;
        self.previous = [1., 0., 0.];
        self.until = 0;
    }
    // Explicit state and prepared scratch avoid allocations in the processing path.
    #[allow(clippy::too_many_arguments)]
    fn update(
        &mut self,
        room: &PreparedRoom,
        shape: Shape,
        previous: [f64; 3],
        metres: f64,
        scene: bool,
        samples: &mut [WeightedPosition; MAX_SAMPLES],
        work: &mut ResponseWork,
    ) -> Result<()> {
        let effective_direction =
            geometry::position(&room.profile, shape.position, metres, previous)?.direction;
        if self.shape == Some(shape) && self.previous == effective_direction {
            return Ok(());
        }
        let count = shape.samples(&room.profile, metres, previous, scene, samples)?;
        self.previous = effective_direction;
        self.filter.fill(0.);
        let n = room.hrir.data.output_taps;
        let taps = room.short_taps;
        for source in &samples[..count] {
            work.hrir(
                room,
                source.position.direction,
                &mut self.face,
                &mut self.query,
            )?;
            let arrival =
                source.position.distance / room.profile.speed * f64::from(super::SAMPLE_RATE);
            let integer = libm::floor(arrival) as usize;
            let mut kernel = [0.; super::hrir::FRACTIONAL_TAPS];
            room.propagation_kernel(
                f64::from((arrival - integer as f64) as f32),
                false,
                &mut kernel,
            );
            let gain =
                (room.profile.reference_distance / source.position.distance) as f32 * source.weight;
            let begin = integer + room.profile.alignment_guard + room.profile.band_offset;
            for ear in 0..2 {
                for i in 0..n {
                    for (j, coefficient) in kernel.iter().enumerate() {
                        self.filter[ear * taps + begin + i + j] +=
                            (self.query[ear * n + i] * *coefficient) * gain;
                    }
                }
            }
        }
        self.convolution.set_target(&self.filter)?;
        self.shape = Some(shape);
        Ok(())
    }
}

struct Slot {
    description: RoomDescription,
    screen: super::screen::Mapping,
    screen_input: Option<[f32; 3]>,
    screen_position: [f32; 3],
    control: Control,
    live: Control,
    live_target: State,
    query_cache: Arc<Mutex<super::hrir::QueryCache>>,
    diffuse: DiffuseDelay,
    states: Vec<State>,
    directions: Vec<[f64; 3]>,
    last_direction: [f64; 3],
    last_position: Option<[f32; 3]>,
    world: [f32; BLOCK],
    head: [f32; BLOCK],
    head_signal: HeadSignal,
    /// This source's own update points in the current canonical block: its
    /// geometry events, initial command and live override changes (ADR 0019).
    marks: [bool; BLOCK],
    moving: [bool; BLOCK],
    hard: [bool; BLOCK],
    discrete: Option<(u64, [u32; 4])>,
    quality: Quality,
    active: bool,
    born: u64,
    retired_at: Option<u64>,
    lock_initialized: bool,
    lock_history: [u8; FILTER_FADE],
    lock_position: usize,
    lock_sum: usize,
}
impl Slot {
    fn new(description: RoomDescription, room: &PreparedRoom, born: u64) -> Result<Self> {
        let state = State {
            position: description.position,
            ..State::default()
        };
        let query_cache = Arc::new(Mutex::new(super::hrir::QueryCache::new(
            &room.hrir,
            if description.role == 2 {
                0
            } else {
                room.images.len()
            },
        )));
        drop(query_cache.lock().unwrap());
        Ok(Self {
            description,
            screen: super::screen::Mapping::new(description.screens)?,
            screen_input: None,
            screen_position: [0.; 3],
            control: Control::new(state),
            live: Control::new(control::neutral_override()),
            live_target: control::neutral_override(),
            query_cache,
            diffuse: DiffuseDelay::default(),
            states: vec![state; BLOCK],
            directions: vec![[1., 0., 0.]; BLOCK],
            last_direction: [1., 0., 0.],
            last_position: None,
            world: [0.; BLOCK],
            head: [0.; BLOCK],
            head_signal: HeadSignal::new(room)?,
            marks: [false; BLOCK],
            moving: [false; BLOCK],
            hard: [false; BLOCK],
            discrete: None,
            quality: room.quality,
            active: true,
            born,
            retired_at: None,
            lock_initialized: false,
            lock_history: [0; FILTER_FADE],
            lock_position: 0,
            lock_sum: 0,
        })
    }
    fn reset(&mut self) {
        self.control.initialize(State {
            position: self.description.position,
            ..State::default()
        });
        self.diffuse.reset();
        self.live.initialize(self.live_target);
        self.head_signal.reset();
        self.marks.fill(false);
        self.moving.fill(false);
        self.hard.fill(false);
        self.discrete = None;
        self.world.fill(0.);
        self.head.fill(0.);
        self.lock_initialized = false;
        self.lock_position = 0;
        self.lock_sum = 0;
        self.lock_history.fill(0);
        self.born = 0;
        self.last_direction = [1., 0., 0.];
        self.last_position = None;
        self.screen_input = None;
        self.directions.fill([1., 0., 0.]);
    }
    fn track_direction(&mut self, state: State) {
        let position = if state.valid & 4 != 0 {
            state.position
        } else {
            self.description.position
        };
        if self.last_position == Some(position) {
            return;
        }
        self.last_position = Some(position);
        let vector = [
            f64::from(position[1]),
            -f64::from(position[0]),
            f64::from(position[2]),
        ];
        let length = crate::geometry::norm(vector);
        if length > 1e-15 {
            self.last_direction = crate::geometry::scale(vector, 1. / length);
        }
    }
    fn spatial_state(&self, mut state: State) -> State {
        if state.valid & 128 != 0
            && state.screen_reference != 0
            && self.description.screens.enabled != 0
        {
            let position = if state.valid & 4 != 0 {
                state.position
            } else {
                self.description.position
            };
            state.position = if self.screen_input == Some(position) {
                self.screen_position
            } else {
                self.screen.position(position)
            };
            state.valid |= 4;
        }
        state
    }
    fn input_state(&mut self, sample: u64) -> State {
        let mut state = self.control.at(sample);
        let live = self.live.at(sample);
        state.gain *= live.gain;
        for i in 0..3 {
            state.extent[i] *= live.extent[i];
        }
        state.diffuse = (state.diffuse * live.diffuse).clamp(0., 1.);
        state.divergence = (state.divergence * live.divergence).clamp(0., 1.);
        if live.valid & 256 != 0 {
            state.head_locked = live.head_locked;
        }
        let authored = if state.valid & 4 != 0 {
            state.position
        } else {
            self.description.position
        };
        let mapped = state.valid & 128 != 0
            && state.screen_reference != 0
            && self.description.screens.enabled != 0;
        state = self.spatial_state(state);
        if mapped {
            self.screen_input = Some(authored);
            self.screen_position = state.position;
        }
        self.track_direction(state);
        state
    }
    fn input_span(
        &mut self,
        input: Option<&[f32]>,
        sample: u64,
        offset: usize,
        scene: bool,
        snapshots: &[bool],
    ) -> Result<()> {
        // The caller ends spans at every command and canonical block boundary.
        // Stable controls and screen geometry can be combined once; active
        // ramps still evaluate at every sample exactly as before. States are
        // kept at the session's update points and at this source's own.
        if self.control.is_settled(sample) && self.live.is_settled(sample) {
            let state = self.input_state(sample);
            for (i, snapshot) in snapshots.iter().copied().enumerate() {
                self.input_with_state(
                    input.map_or(0., |v| v[i]),
                    &state,
                    sample + i as u64,
                    offset + i,
                    scene,
                    snapshot || self.marks[offset + i],
                )?;
            }
        } else {
            for (i, snapshot) in snapshots.iter().copied().enumerate() {
                let state = self.input_state(sample + i as u64);
                self.input_with_state(
                    input.map_or(0., |v| v[i]),
                    &state,
                    sample + i as u64,
                    offset + i,
                    scene,
                    snapshot || self.marks[offset + i],
                )?;
            }
        }
        Ok(())
    }
    #[inline]
    fn input_with_state(
        &mut self,
        value: f32,
        state: &State,
        sample: u64,
        offset: usize,
        scene: bool,
        snapshot: bool,
    ) -> Result<()> {
        if snapshot {
            if self.quality == Quality::Realtime {
                self.moving[offset] =
                    self.control.geometry_ramping(sample) || self.live.geometry_ramping(sample);
                // Screen mapping synthesizes a valid effective position. Keep
                // authored validity so a clear at a ramp deadline still forces
                // an immediate reflection update while another field ramps.
                let discrete = (
                    self.control.at(sample).valid & control::GEOMETRY_FIELDS,
                    [
                        state.channel_lock,
                        state.screen_reference,
                        state.head_locked,
                        state.has_max_distance,
                    ],
                );
                self.hard[offset] |= self.discrete != Some(discrete);
                self.discrete = Some(discrete);
            }
            self.states[offset] = *state;
            self.directions[offset] = self.last_direction;
        }
        let gain = if state.active != 0 { state.gain } else { 0. };
        let value = if self.description.role == 2 {
            value * gain
        } else {
            let mut delayed = [0.];
            self.diffuse.process(&[value], &mut delayed)?;
            let d = state.diffuse.clamp(0., 1.);
            if scene {
                gain * (value * (1. - d) + delayed[0] * d)
            } else {
                gain * (value * (1. - d).sqrt() + delayed[0] * d.sqrt())
            }
        };
        if !value.is_finite() {
            return Err(Error::InvalidArgument("Room source gain overflow"));
        }
        let target = if state.head_locked != 0 && self.description.role != 2 {
            1.
        } else {
            0.
        };
        if !self.lock_initialized {
            self.lock_initialized = true;
            self.lock_history.fill(target as u8);
            self.lock_sum = target as usize * FILTER_FADE;
        }
        let weight = self.lock_sum as f32 / FILTER_FADE as f32;
        self.world[offset] = value * (1. - weight);
        self.head[offset] = value * weight;
        self.lock_sum -= usize::from(self.lock_history[self.lock_position]);
        self.lock_history[self.lock_position] = target as u8;
        self.lock_sum += target as usize;
        self.lock_position = (self.lock_position + 1) % FILTER_FADE;
        Ok(())
    }
}

struct Voice {
    emitter: Emitter,
    shape: Option<Shape>,
    until: u64,
    constrained: bool,
    work: ResponseWork,
    samples: [WeightedPosition; MAX_SAMPLES],
    direct: [f32; BLOCK * 2],
    early: [f32; BLOCK * 2],
    send: [f32; BLOCK],
}

impl Voice {
    // Explicit state and prepared scratch avoid allocations in the processing path.
    #[allow(clippy::too_many_arguments)]
    fn render(
        &mut self,
        room: &PreparedRoom,
        slot: &VoiceInput,
        marks: &[bool; BLOCK],
        poses: &[[f32; 3]; BLOCK],
        wet_gain: &[f32; BLOCK],
        start: u64,
        metres: f64,
        scene: bool,
        spread: bool,
        excitation: bool,
        prepare_filters: bool,
    ) -> Result<()> {
        self.direct.fill(0.);
        self.early.fill(0.);
        self.send.fill(0.);
        if slot.description.role == 2 {
            return Ok(());
        }
        let memory = room.eq.taps - 1 + (room.short_taps - 1).max(room.send_taps - 1);
        if excitation && slot.world.iter().any(|v| *v != 0.) {
            if start >= self.until {
                self.emitter.reset();
                self.shape = None;
            }
            self.until = start + BLOCK as u64 + memory as u64;
        }
        if start >= self.until {
            return Ok(());
        }
        let silence = [0.; BLOCK];
        self.emitter.begin_branches(
            if excitation { &slot.world } else { &silence },
            wet_gain,
            excitation,
        )?;
        // Update points: the session's (block start, pose changes, source set
        // and end of input) and this source's own events. Another source's
        // events never resample this source's ramp (ADR 0019).
        let marked = |i: usize| marks[i] || slot.marks[i];
        let mut end = (0..BLOCK).find(|i| marked(*i)).unwrap_or(BLOCK);
        while end < BLOCK {
            let begin = end;
            end = (begin + 1..BLOCK).find(|i| marked(*i)).unwrap_or(BLOCK);
            if start + (begin as u64) < slot.born {
                continue;
            }
            let _profile = super::profile::Guard::new(super::profile::Stage::VoiceSegment);
            let shape = Shape::from_state(
                slot.states[begin],
                slot.description.position,
                slot.description.role,
                spread,
            );
            let moved = self.shape != Some(shape);
            let position = geometry::position(
                &room.profile,
                shape.position,
                metres,
                slot.directions[begin],
            )?;
            let n = shape.samples(
                &room.profile,
                metres,
                slot.directions[begin],
                scene,
                &mut self.samples,
            )?;
            if prepare_filters {
                let defer = room.quality == Quality::Realtime
                    && slot.moving[begin]
                    && !slot.hard[begin]
                    && !room
                        .quality
                        .reflection_due(start + begin as u64, slot.description.id);
                self.emitter.update_sources(
                    position,
                    &self.samples[..n],
                    poses[begin],
                    moved,
                    true,
                    defer,
                    &mut self.work,
                )?;
            }
            self.shape = Some(shape);
            self.constrained = position.constrained;
            self.emitter.render_range(
                begin,
                end - begin,
                &mut self.direct,
                &mut self.early,
                &mut self.send,
            )?;
        }
        Ok(())
    }
}

struct VoiceInput {
    description: RoomDescription,
    born: u64,
    marks: [bool; BLOCK],
    moving: [bool; BLOCK],
    hard: [bool; BLOCK],
    states: [State; BLOCK],
    directions: [[f64; 3]; BLOCK],
    world: [f32; BLOCK],
}
impl Default for VoiceInput {
    fn default() -> Self {
        Self {
            description: RoomDescription::default(),
            born: 0,
            marks: [false; BLOCK],
            moving: [false; BLOCK],
            hard: [false; BLOCK],
            states: [State::default(); BLOCK],
            directions: [[1., 0., 0.]; BLOCK],
            world: [0.; BLOCK],
        }
    }
}
#[derive(Clone, Copy)]
struct VoiceBlock {
    marks: [bool; BLOCK],
    poses: [[f32; 3]; BLOCK],
    wet_gain: [f32; BLOCK],
    start: u64,
    metres: f64,
    scene: bool,
    spread: bool,
    excitation: bool,
    prepare_filters: bool,
    sources: usize,
}
struct Voices {
    room: Arc<PreparedRoom>,
    voices: Vec<Voice>,
    inputs: Vec<VoiceInput>,
    block: Option<VoiceBlock>,
}
fn render_voice(
    voice: &mut Voice,
    room: &PreparedRoom,
    slot: &VoiceInput,
    block: &VoiceBlock,
) -> Result<()> {
    let _profile = super::profile::Guard::new(super::profile::Stage::VoiceRender);
    voice.render(
        room,
        slot,
        &block.marks,
        &block.poses,
        &block.wet_gain,
        block.start,
        block.metres,
        block.scene,
        block.spread,
        block.excitation,
        block.prepare_filters,
    )
}
impl Voices {
    fn render(&mut self) -> Result<()> {
        let block = self
            .block
            .ok_or(Error::InvalidArgument("Missing room block"))?;
        let room = &self.room;
        let render =
            |(voice, slot): (&mut Voice, &VoiceInput)| render_voice(voice, room, slot, &block);
        if block.sources >= 4 {
            self.voices[..block.sources]
                .par_iter_mut()
                .zip(self.inputs[..block.sources].par_iter())
                .try_for_each(render)
        } else {
            self.voices[..block.sources]
                .iter_mut()
                .zip(&self.inputs[..block.sources])
                .try_for_each(render)
        }
    }
}
impl super::workers::Work for Mutex<Voices> {
    fn run(&self) -> Result<()> {
        self.lock().unwrap().render()
    }
}

// During a room transition, process the same source in both rooms on the same
// worker before moving to another source. Its exact HRIR query cache stays hot;
// audio reduction still happens later in the fixed source-ID order.
struct BlockJob {
    current: Arc<Mutex<Voices>>,
    incoming: Option<Arc<Mutex<Voices>>>,
    current_output: Arc<Mutex<InstanceOutput>>,
    incoming_output: Option<Arc<Mutex<InstanceOutput>>>,
    order: [usize; MAX_SOURCES],
    sources: usize,
    draining: bool,
    drain_remaining: usize,
    old_gain: [f32; BLOCK],
    new_gain: [f32; BLOCK],
    marks: [bool; BLOCK],
    poses: [[f32; 3]; BLOCK],
    late_decoder: WorldTail,
    late_output: [f32; BLOCK * 2],
    output: [f32; BLOCK * 2],
}
impl BlockJob {
    fn render(&mut self) -> Result<()> {
        self.render_voices()?;
        let mut current = self.current_output.lock().unwrap();
        current.finish(&self.current.lock().unwrap(), &self.order[..self.sources])?;
        let mut incoming = self.incoming_output.as_ref().map(|v| v.lock().unwrap());
        if let (Some(output), Some(voices)) = (&mut incoming, &self.incoming) {
            output.finish(&voices.lock().unwrap(), &self.order[..self.sources])?;
        }
        for i in 0..BLOCK {
            for ear in 0..2 {
                let at = ear * BLOCK + i;
                let tail_gain = if self.draining && i >= self.drain_remaining {
                    0.
                } else {
                    1.
                };
                let mut sample =
                    current.direct[at] * self.old_gain[i] + current.wet[at] * tail_gain;
                if let Some(incoming) = &incoming {
                    sample += incoming.direct[at] * self.new_gain[i] + incoming.wet[at];
                }
                self.output[2 * i + ear] = sample;
            }
        }
        // Both room fields and the head-pose decoder share one direction pass.
        // Source geometry marks do not split it; only pose changes retarget.
        let current_frames = if self.draining {
            self.drain_remaining.min(BLOCK)
        } else {
            BLOCK
        };
        self.late_decoder.render_world(
            &current.field,
            current_frames,
            incoming.as_deref().map(|v| &v.field),
            &self.marks,
            &self.poses,
            &mut self.late_output,
        )?;
        for i in 0..BLOCK {
            for ear in 0..2 {
                self.output[2 * i + ear] += self.late_output[ear * BLOCK + i];
            }
        }
        Ok(())
    }
    fn render_voices(&mut self) -> Result<()> {
        let _profile = super::profile::Guard::new(super::profile::Stage::VoiceBatch);
        let mut current = self.current.lock().unwrap();
        let Some(incoming) = &self.incoming else {
            return current.render();
        };
        let mut incoming = incoming.lock().unwrap();
        let a = current
            .block
            .ok_or(Error::InvalidArgument("Missing paired room block"))?;
        let b = incoming
            .block
            .ok_or(Error::InvalidArgument("Missing paired room block"))?;
        if a.sources != b.sources {
            return Err(Error::InvalidArgument(
                "Mismatched paired room source counts",
            ));
        }
        let Voices {
            room: room_a,
            voices: voices_a,
            inputs: inputs_a,
            ..
        } = &mut *current;
        let Voices {
            room: room_b,
            voices: voices_b,
            inputs: inputs_b,
            ..
        } = &mut *incoming;
        let room_a = &**room_a;
        let room_b = &**room_b;
        let render = |(((voice_a, voice_b), input_a), input_b): (
            ((&mut Voice, &mut Voice), &VoiceInput),
            &VoiceInput,
        )| {
            render_voice(voice_a, room_a, input_a, &a)?;
            render_voice(voice_b, room_b, input_b, &b)
        };
        if a.sources >= 4 {
            voices_a[..a.sources]
                .par_iter_mut()
                .zip(voices_b[..a.sources].par_iter_mut())
                .zip(inputs_a[..a.sources].par_iter())
                .zip(inputs_b[..a.sources].par_iter())
                .try_for_each(render)
        } else {
            voices_a[..a.sources]
                .iter_mut()
                .zip(voices_b[..a.sources].iter_mut())
                .zip(&inputs_a[..a.sources])
                .zip(&inputs_b[..a.sources])
                .try_for_each(render)
        }
    }
}
impl super::workers::Work for Mutex<BlockJob> {
    fn run(&self) -> Result<()> {
        self.lock().unwrap().render()
    }
}

struct InstanceOutput {
    field: WorldTail,
    lfe: Long,
    send: [f32; BLOCK],
    lfe_input: [f32; BLOCK],
    lfe_output: [f32; BLOCK],
    direct: [f32; 2 * BLOCK],
    wet: [f32; 2 * BLOCK],
}
impl InstanceOutput {
    fn finish(&mut self, group: &Voices, order: &[usize]) -> Result<()> {
        let block = group
            .block
            .ok_or(Error::InvalidArgument("Missing room reduction block"))?;
        let wet_gain = &block.wet_gain;
        let excitation = block.excitation;
        self.direct.fill(0.);
        self.wet.fill(0.);
        self.send.fill(0.);
        self.lfe_input.fill(0.);
        let profile = super::profile::Guard::new(super::profile::Stage::SourceReduction);
        // Parallel work never reduces audio. Preserve the fixed source-ID order.
        for index in order {
            let slot = &group.inputs[*index];
            if slot.description.role == 2 {
                if excitation {
                    for (i, gain) in wet_gain.iter().enumerate() {
                        self.lfe_input[i] += slot.world[i] * gain;
                    }
                }
            } else {
                let voice = &group.voices[*index];
                for i in 0..BLOCK {
                    self.send[i] += voice.send[i];
                    for ear in 0..2 {
                        let at = ear * BLOCK + i;
                        self.direct[at] += voice.direct[at];
                        self.wet[at] += voice.early[at];
                    }
                }
            }
        }
        drop(profile);
        self.field.begin_field(&self.send)?;
        self.lfe.process(&self.lfe_input, &mut self.lfe_output)?;
        for i in 0..BLOCK {
            for ear in 0..2 {
                let at = ear * BLOCK + i;
                self.wet[at] += self.lfe_output[i];
            }
        }
        Ok(())
    }
}

pub struct Instance {
    pub prepared: Arc<PreparedSet>,
    group: Arc<Mutex<Voices>>,
    output: Arc<Mutex<InstanceOutput>>,
}
impl Instance {
    fn query_caches(&mut self, slots: Option<&[Slot]>) {
        let mut group = self.group.lock().unwrap();
        for (index, voice) in group.voices.iter_mut().enumerate() {
            voice.work.query_cache = slots
                .and_then(|slots| slots.get(index))
                .map(|s| s.query_cache.clone());
        }
    }
    pub fn new(prepared: Arc<PreparedSet>, capacity: usize) -> Result<Self> {
        if capacity > MAX_SOURCES {
            return Err(Error::InvalidArgument("Room source capacity exceeded"));
        }
        let mut result = Self {
            group: Arc::new(Mutex::new(Voices {
                room: prepared.room.clone(),
                voices: Vec::with_capacity(capacity),
                inputs: Vec::with_capacity(capacity),
                block: None,
            })),
            output: Arc::new(Mutex::new(InstanceOutput {
                field: WorldTail::new_field(prepared.room.clone())?,
                lfe: Long::new(prepared.lfe.clone())?,
                send: [0.; BLOCK],
                lfe_input: [0.; BLOCK],
                lfe_output: [0.; BLOCK],
                direct: [0.; 2 * BLOCK],
                wet: [0.; 2 * BLOCK],
            })),
            prepared,
        };
        result.reserve(capacity)?;
        drop(result.output.lock().unwrap());
        Ok(result)
    }
    pub fn capacity(&self) -> usize {
        self.group.lock().unwrap().voices.len()
    }
    /// May allocate. Preparation/configuration only, never during processing.
    pub fn reserve(&mut self, count: usize) -> Result<()> {
        if count > MAX_SOURCES {
            return Err(Error::InvalidArgument("Room source capacity exceeded"));
        }
        let mut group = self.group.lock().unwrap();
        while group.voices.len() < count {
            group.inputs.push(VoiceInput::default());
            let position =
                geometry::position(&self.prepared.room.profile, [0., 1., 0.], 1.5, [1., 0., 0.])?;
            group.voices.push(Voice {
                emitter: Emitter::new(self.prepared.room.clone(), position)?,
                shape: None,
                until: 0,
                constrained: false,
                work: ResponseWork::new(&self.prepared.room)?,
                samples: [WeightedPosition::default(); MAX_SAMPLES],
                direct: [0.; BLOCK * 2],
                early: [0.; BLOCK * 2],
                send: [0.; BLOCK],
            });
        }
        Ok(())
    }
    fn reset(&mut self) {
        for voice in &mut self.group.lock().unwrap().voices {
            voice.emitter.reset();
            voice.shape = None;
            voice.until = 0;
            voice.constrained = false;
        }
        let mut output = self.output.lock().unwrap();
        output.field.reset();
        output.lfe.reset();
        output.direct.fill(0.);
        output.wet.fill(0.);
    }
    fn reset_voice(&mut self, index: usize) {
        let mut group = self.group.lock().unwrap();
        let voice = &mut group.voices[index];
        voice.emitter.reset();
        voice.shape = None;
        voice.until = 0;
        voice.constrained = false;
    }
    // Explicit state and prepared scratch avoid allocations in the processing path.
    #[allow(clippy::too_many_arguments)]
    fn prepare_render(
        &mut self,
        slots: &[Slot],
        marks: &[bool; BLOCK],
        poses: &[[f32; 3]; BLOCK],
        wet_gain: &[f32; BLOCK],
        start: u64,
        metres: f64,
        scene: bool,
        spread: bool,
        excitation: bool,
        prepare_filters: bool,
    ) -> Result<()> {
        {
            let mut group = self.group.lock().unwrap();
            for (input, slot) in group.inputs.iter_mut().zip(slots) {
                input.description = slot.description;
                input.born = slot.born;
                input.marks = slot.marks;
                if self.prepared.room.quality == Quality::Realtime {
                    input.moving = slot.moving;
                    input.hard = slot.hard;
                }
                input.states.copy_from_slice(&slot.states);
                input.directions.copy_from_slice(&slot.directions);
                input.world = slot.world;
            }
            group.block = Some(VoiceBlock {
                marks: *marks,
                poses: *poses,
                wet_gain: *wet_gain,
                start,
                metres,
                scene,
                spread,
                excitation,
                prepare_filters,
                sources: slots.len(),
            });
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct RoomInfo {
    pub input_frames: u64,
    pub latency_frames: u64,
    pub tail_frames: u64,
    pub material_guard_frames: u64,
    pub source_count: u32,
    pub source_capacity: u32,
    pub constrained_sources: u32,
    pub phase: u32,
    pub applied_request: u64,
    pub transition_sample: u64,
    pub pending_request: u64,
}

pub struct Session {
    current: Box<Instance>,
    paired: Arc<Mutex<BlockJob>>,
    incoming: Option<Box<Instance>>,
    pending: Option<(u64, Box<Instance>)>,
    retired: Option<Box<Instance>>,
    phase: u32,
    warm_remaining: usize,
    fade_done: usize,
    drain_remaining: usize,
    incoming_request: u64,
    requested_revision: u64,
    applied_request: u64,
    transition_sample: u64,
    slots: Vec<Slot>,
    active: Vec<usize>,
    order: Vec<usize>,
    clock: u64,
    fill: usize,
    fifo: [f32; BLOCK * 2],
    /// Update points for every source: block start, pose changes, the source
    /// set and end of input. Sources add their own events (`Slot::marks`).
    marks: [bool; BLOCK],
    /// The same without pose changes, for head-locked dry paths.
    geometry_marks: [bool; BLOCK],
    poses: [[f32; 3]; BLOCK],
    pose: [f32; 3],
    pose_changed: bool,
    metres: f64,
    scene: bool,
    spread: bool,
    head_work: ResponseWork,
    head_room: Arc<PreparedRoom>,
    head_samples: [WeightedPosition; MAX_SAMPLES],
    head_output: [f32; BLOCK * 2],
    ended: Option<u64>,
    end_tail: usize,
}

impl Session {
    pub fn set_overrides(&mut self, overrides: &[RoomOverride]) -> Result<()> {
        if overrides.len() != self.active.len() {
            return Err(Error::InvalidArgument(
                "Invalid room live override count/state",
            ));
        }
        for value in overrides {
            value.state()?;
        }
        for (index, value) in self.active.iter().zip(overrides) {
            let target = value.state()?;
            let slot = &mut self.slots[*index];
            if target == slot.live_target {
                continue;
            }
            slot.marks[self.fill] = true;
            slot.hard[self.fill] = true;
            if self.ended.is_some() {
                slot.live_target = target;
                continue;
            }
            if self.clock == slot.born {
                slot.live.initialize(target);
            } else {
                slot.live.apply(
                    &RoomCommand {
                        changed: 2 | 16,
                        duration: super::FILTER_FADE as u32,
                        state: target,
                        ..RoomCommand::default()
                    },
                    self.clock,
                )?;
                slot.live.apply(
                    &RoomCommand {
                        changed: 8 | 32 | 256,
                        cleared: if target.valid & 256 == 0 { 256 } else { 0 },
                        state: target,
                        ..RoomCommand::default()
                    },
                    self.clock,
                )?;
            }
            slot.live_target = target;
        }
        Ok(())
    }
    pub fn new(prepared: Arc<PreparedSet>, metres: f64, scene: bool, spread: bool) -> Result<Self> {
        if !metres.is_finite() || !(0.01..=100.).contains(&metres) {
            return Err(Error::InvalidArgument("Invalid room distance scale"));
        }
        let current = Box::new(Instance::new(prepared.clone(), 0)?);
        let paired = Arc::new(Mutex::new(BlockJob {
            current: current.group.clone(),
            incoming: None,
            current_output: current.output.clone(),
            incoming_output: None,
            order: [0; MAX_SOURCES],
            sources: 0,
            draining: false,
            drain_remaining: 0,
            old_gain: [1.; BLOCK],
            new_gain: [0.; BLOCK],
            marks: [false; BLOCK],
            poses: [[0.; 3]; BLOCK],
            late_decoder: WorldTail::new_decoder(prepared.room.clone())?,
            late_output: [0.; BLOCK * 2],
            output: [0.; BLOCK * 2],
        }));
        drop(paired.lock().unwrap());
        Ok(Self {
            current,
            paired,
            incoming: None,
            pending: None,
            retired: None,
            phase: 0,
            warm_remaining: 0,
            fade_done: 0,
            drain_remaining: 0,
            incoming_request: 0,
            requested_revision: 0,
            applied_request: 0,
            transition_sample: 0,
            slots: Vec::new(),
            active: Vec::new(),
            order: Vec::new(),
            clock: 0,
            fill: 0,
            fifo: [0.; BLOCK * 2],
            marks: [false; BLOCK],
            geometry_marks: [false; BLOCK],
            poses: [[0.; 3]; BLOCK],
            pose: [0.; 3],
            pose_changed: true,
            metres,
            scene,
            spread,
            head_work: ResponseWork::new(&prepared.room)?,
            head_room: prepared.room.clone(),
            head_samples: [WeightedPosition::default(); MAX_SAMPLES],
            head_output: [0.; BLOCK * 2],
            ended: None,
            end_tail: 0,
        })
    }
    /// Preparation call; source histories survive generation changes.
    pub fn configure(&mut self, descriptions: &[RoomDescription]) -> Result<()> {
        let block_start = self.clock - self.fill as u64;
        let history = self.info().tail_frames;
        let reusable: Vec<bool> = self
            .slots
            .iter()
            .map(|slot| {
                !slot.active
                    && slot
                        .retired_at
                        .is_some_and(|at| at.saturating_add(history) <= block_start)
            })
            .collect();
        if descriptions.len() > MAX_SOURCES
            || descriptions.iter().any(|d| {
                d.role > 2
                    || d.reserved != 0
                    || d.padding != 0
                    || d.position.iter().any(|v| !v.is_finite())
            })
        {
            return Err(Error::InvalidArgument("Invalid room source descriptions"));
        }
        for (i, d) in descriptions.iter().enumerate() {
            super::screen::Mapping::new(d.screens)?;
            if descriptions[..i].iter().any(|v| v.id == d.id)
                || self.slots.iter().enumerate().any(|(i, v)| {
                    v.description.same_source(d)
                        && (v.description.role != d.role || v.description.screens != d.screens)
                        && !reusable[i]
                })
            {
                return Err(Error::InvalidArgument(
                    "Duplicate room source or changed role/screen configuration",
                ));
            }
        }
        let missing = descriptions
            .iter()
            .filter(|d| !self.slots.iter().any(|v| v.description.same_source(d)))
            .count();
        let free = self
            .slots
            .iter()
            .enumerate()
            .filter(|(i, s)| {
                reusable[*i] && !descriptions.iter().any(|d| d.same_source(&s.description))
            })
            .count();
        let extra = missing.saturating_sub(free);
        if self.slots.len() + extra > MAX_SOURCES {
            return Err(Error::InvalidArgument(
                "Room source/history capacity exceeded",
            ));
        }
        self.current.reserve(self.slots.len() + extra)?;
        if let Some(incoming) = &mut self.incoming {
            incoming.reserve(self.slots.len() + extra)?;
        }
        if let Some((_, pending)) = &mut self.pending {
            pending.reserve(self.slots.len() + extra)?;
        }
        for slot in &mut self.slots {
            if !descriptions
                .iter()
                .any(|d| d.same_source(&slot.description))
            {
                slot.control.initialize(slot.control.at(self.clock));
                slot.live_target = slot.live.at(self.clock);
                slot.live.initialize(slot.live_target);
                if slot.active {
                    slot.retired_at = Some(self.clock);
                }
            }
            slot.active = false;
        }
        self.active.clear();
        for description in descriptions {
            let existing = self
                .slots
                .iter()
                .position(|v| v.description.same_source(description));
            let available = existing.or_else(|| {
                self.slots.iter().enumerate().position(|(i, s)| {
                    !s.active
                        && reusable.get(i).copied().unwrap_or(false)
                        && !descriptions.iter().any(|d| d.same_source(&s.description))
                })
            });
            let index = if let Some(index) = available {
                if reusable.get(index).copied().unwrap_or(false) {
                    self.slots[index] = Slot::new(*description, &self.head_room, self.clock)?;
                    self.current.reset_voice(index);
                    if let Some(v) = &mut self.incoming {
                        v.reset_voice(index);
                    }
                    if let Some((_, v)) = &mut self.pending {
                        v.reset_voice(index);
                    }
                }
                index
            } else {
                self.slots
                    .push(Slot::new(*description, &self.head_room, self.clock)?);
                self.slots.len() - 1
            };
            self.slots[index].active = true;
            self.slots[index].retired_at = None;
            self.slots[index].description = *description;
            self.active.push(index);
        }
        self.order = (0..self.slots.len()).collect();
        self.order.sort_unstable_by_key(|i| {
            (
                self.slots[*i].description.id,
                self.slots[*i].description.generation,
            )
        });
        self.marks[self.fill] = true;
        self.geometry_marks[self.fill] = true;
        for slot in &mut self.slots {
            slot.hard[self.fill] = true;
        }
        self.current.prepared.workers.warm()?;
        if self.phase != 0 {
            self.current.query_caches(Some(&self.slots));
            if let Some(next) = &mut self.incoming {
                next.query_caches(Some(&self.slots));
            }
        }
        Ok(())
    }
    pub fn reset(&mut self, sample: u64) -> Result<()> {
        if !sample.is_multiple_of(2048) {
            return Err(Error::InvalidArgument(
                "Room preroll must start on a 2048-frame partition boundary",
            ));
        }
        // Reset is a preparation/control operation. A committed incoming room
        // becomes the sole configuration; an inaudible warmup is cancelled.
        if self.phase >= 2
            && let Some(incoming) = self.incoming.take()
        {
            self.current = incoming;
            self.applied_request = self.incoming_request;
        }
        self.incoming = None;
        self.pending = None;
        self.retired = None;
        self.phase = 0;
        self.sync_voice_pair();
        self.current.reset();
        self.current.query_caches(None);
        for slot in &mut self.slots {
            slot.reset();
            slot.born = sample;
        }
        self.clock = sample;
        self.fill = 0;
        self.fifo.fill(0.);
        {
            let mut job = self.paired.lock().unwrap();
            job.late_decoder.reset();
            job.late_output.fill(0.);
            job.output.fill(0.);
        }
        self.marks.fill(false);
        self.geometry_marks.fill(false);
        self.pose_changed = true;
        self.ended = None;
        self.end_tail = 0;
        Ok(())
    }
    pub fn clear_generation(&mut self) {
        self.slots.clear();
        self.active.clear();
        self.order.clear();
    }
    pub fn prime(&mut self, command: &RoomCommand, sample: u64) -> Result<()> {
        self.validate_command(command)?;
        if self.fill != 0 || sample > self.clock {
            return Err(Error::InvalidArgument(
                "Invalid room preroll control anchor",
            ));
        }
        let slot = &mut self.slots[self.active[command.element as usize]];
        if sample > 0 {
            slot.track_direction(slot.spatial_state(slot.control.at(sample - 1)));
        }
        slot.control.apply(command, sample)?;
        slot.track_direction(slot.spatial_state(slot.control.at(sample)));
        Ok(())
    }
    pub fn set_pose(&mut self, pose: [f32; 3]) -> Result<()> {
        if pose.iter().any(|v| !v.is_finite()) {
            return Err(Error::InvalidArgument("Invalid room head pose"));
        }
        if self.pose != pose {
            self.pose = pose;
            self.pose_changed = true;
        }
        Ok(())
    }
    pub fn end_input(&mut self) {
        if self.ended.is_none() {
            if self.phase < 2 {
                self.incoming = None;
                self.phase = 0;
            }
            self.end_tail = self.tail_frames();
            self.pending = None;
            self.sync_voice_pair();
            self.ended = Some(self.clock);
            for slot in &mut self.slots {
                slot.control.initialize(slot.control.at(self.clock));
                slot.live.initialize(slot.live.at(self.clock));
            }
            self.marks[self.fill] = true;
            self.geometry_marks[self.fill] = true;
        }
    }
    fn tail_frames(&self) -> usize {
        // Head-locked sources retain the initial dry propagation filters across
        // room switches, even when the new world's acoustic support is shorter.
        let head = self.head_room.short_taps - 1 + crate::diffuse::DELAY_LENGTH - 1 + BLOCK;
        self.incoming
            .as_ref()
            .map_or(self.current.prepared.tail_frames(), |v| {
                self.current
                    .prepared
                    .tail_frames()
                    .max(v.prepared.tail_frames())
            })
            .max(head)
    }
    fn pending_request(&self) -> u64 {
        self.pending
            .as_ref()
            .filter(|v| v.0 == self.requested_revision)
            .map_or(0, |v| v.0)
    }
    pub fn info(&self) -> RoomInfo {
        RoomInfo {
            input_frames: self.ended.unwrap_or(self.clock),
            latency_frames: BLOCK as u64,
            tail_frames: if self.ended.is_some() {
                self.end_tail as u64
            } else {
                self.tail_frames() as u64
            },
            source_count: self.active.len() as u32,
            material_guard_frames: self.current.prepared.room.profile.band_offset as u64,
            source_capacity: self.current.capacity() as u32,
            constrained_sources: self
                .slots
                .iter()
                .filter(|s| s.active)
                .filter_map(|s| self.position_for_slot(s))
                .filter(|p| p.constrained)
                .count() as u32,
            phase: if self.phase == 0 && self.pending_request() != 0 {
                1
            } else {
                self.phase
            },
            applied_request: self.applied_request,
            transition_sample: self.transition_sample,
            pending_request: if self.pending_request() != 0 {
                self.pending_request()
            } else if self.phase != 0 {
                self.incoming_request
            } else {
                0
            },
        }
    }
    pub fn source_position(&self, id: u64) -> Option<Position> {
        let slot = self
            .slots
            .iter()
            .find(|s| s.description.id == id && s.active)?;
        self.position_for_slot(slot)
    }
    fn position_for_slot(&self, slot: &Slot) -> Option<Position> {
        let room = if self.phase >= 2 {
            self.incoming.as_ref().unwrap_or(&self.current)
        } else {
            &self.current
        };
        let mut state = slot.control.at(self.ended.unwrap_or(self.clock));
        state = slot.spatial_state(state);
        let live = slot.live.at(self.ended.unwrap_or(self.clock));
        if live.valid & 256 != 0 {
            state.head_locked = live.head_locked;
        }
        if slot.description.role == 2 {
            return geometry::position(
                &room.prepared.room.profile,
                [0., room.prepared.room.profile.reference_distance as f32, 0.],
                1.,
                [1., 0., 0.],
            )
            .ok();
        }
        let profile = if state.head_locked != 0 {
            &self.head_room.profile
        } else {
            &room.prepared.room.profile
        };
        let shape = Shape::from_state(
            state,
            slot.description.position,
            slot.description.role,
            self.spread,
        );
        geometry::position(profile, shape.position, self.metres, slot.last_direction).ok()
    }
    /// The caller prepares `next` off the render worker. A displaced, inaudible
    /// candidate is returned to the caller for background destruction.
    pub fn validate_room(&self, next: &Instance, request: u64) -> Result<()> {
        if next.prepared.room.quality != self.head_room.quality {
            return Err(Error::InvalidArgument(
                "Room quality is fixed for the session",
            ));
        }
        if request < self.requested_revision {
            return Err(Error::InvalidArgument("Room switch has been superseded"));
        }
        if self.ended.is_some() || request == 0 || next.capacity() < self.slots.len() {
            return Err(Error::InvalidArgument(
                "Room switch is ended, invalid or under-prepared",
            ));
        }
        if next.prepared.room.profile.late_directions.len()
            != self.head_room.profile.late_directions.len()
            || next
                .prepared
                .room
                .profile
                .late_directions
                .iter()
                .zip(&self.head_room.profile.late_directions)
                .any(|(a, b)| a.map(f64::to_bits) != b.map(f64::to_bits))
        {
            return Err(Error::Unsupported(
                "Room switching requires the same world late-field direction basis",
            ));
        }
        if next.prepared.room.hrir.data.identity != self.head_room.hrir.data.identity
            || next.prepared.room.profile.alignment_guard
                != self.current.prepared.room.profile.alignment_guard
            || next.prepared.room.profile.band_offset
                != self.current.prepared.room.profile.band_offset
        {
            return Err(Error::Unsupported(
                "Room switching requires the same HRIR binding and alignment guard",
            ));
        }
        Ok(())
    }
    /// Notify the worker before asynchronous preparation completes (or fails).
    /// Superseded pending resources are reclaimed through `take_retired`, never
    /// destroyed here or started by render_block. An ongoing transition finishes.
    pub fn note_room_request(&mut self, request: u64) -> Result<()> {
        if request == 0 {
            return Err(Error::InvalidArgument("Invalid room request revision"));
        }
        self.requested_revision = self.requested_revision.max(request);
        Ok(())
    }
    pub fn queue_room(
        &mut self,
        next: Box<Instance>,
        request: u64,
    ) -> Result<[Option<Box<Instance>>; 2]> {
        self.validate_room(&next, request)?;
        self.note_room_request(request)?;
        if self.phase == 0
            && next.prepared.room.profile.identity == self.current.prepared.room.profile.identity
        {
            self.applied_request = request;
            return Ok([Some(next), self.pending.take().map(|v| v.1)]);
        }
        if self.incoming.as_ref().is_some_and(|v| {
            v.prepared.room.profile.identity == next.prepared.room.profile.identity
        }) {
            self.incoming_request = request;
            return Ok([Some(next), self.pending.take().map(|v| v.1)]);
        }
        if self.phase <= 1 && self.retired.is_none() {
            let old = self.incoming.replace(next);
            self.incoming_request = request;
            self.begin_warmup();
            Ok([old, None])
        } else {
            Ok([self.pending.replace((request, next)).map(|v| v.1), None])
        }
    }
    pub fn take_retired(&mut self) -> Option<Box<Instance>> {
        self.retired.take().or_else(|| {
            if self
                .pending
                .as_ref()
                .is_some_and(|v| v.0 < self.requested_revision)
            {
                self.pending.take().map(|v| v.1)
            } else {
                None
            }
        })
    }
    fn sync_voice_pair(&mut self) {
        let mut paired = self.paired.lock().unwrap();
        paired.current = self.current.group.clone();
        paired.incoming = self.incoming.as_ref().map(|v| v.group.clone());
        paired.current_output = self.current.output.clone();
        paired.incoming_output = self.incoming.as_ref().map(|v| v.output.clone());
    }
    fn begin_warmup(&mut self) {
        self.sync_voice_pair();
        self.current.query_caches(Some(&self.slots));
        if let Some(next) = &mut self.incoming {
            next.query_caches(Some(&self.slots));
        }
        let incoming = self.incoming.as_ref().expect("prepared room switch");
        self.warm_remaining = (incoming.prepared.room.eq.taps + incoming.prepared.room.short_taps
            - 2)
        .div_ceil(BLOCK)
            * BLOCK;
        self.fade_done = 0;
        self.phase = 1;
    }
    fn finish_switch(&mut self) {
        if let Some(next) = self.incoming.take() {
            self.retired = Some(std::mem::replace(&mut self.current, next));
            self.applied_request = self.incoming_request;
            self.current.query_caches(None);
        }
        self.sync_voice_pair();
        self.phase = 0;
    }
    fn validate_command(&self, c: &RoomCommand) -> Result<()> {
        if c.element as usize >= self.active.len()
            || c.reserved != 0
            || (c.changed | c.cleared) & !control::FIELDS != 0
        {
            return Err(Error::InvalidArgument("Invalid room source command"));
        }
        control::validate_state(&c.state)
    }
    pub fn process<'a, I>(
        &mut self,
        frames: usize,
        planes: I,
        initial: &[RoomCommand],
        events: &[RoomCommand],
        output: &mut [f32],
    ) -> Result<RoomInfo>
    where
        I: Iterator<Item = Option<&'a [f32]>> + Clone + ExactSizeIterator,
    {
        let _profile = super::profile::Guard::new(super::profile::Stage::SessionProcess);
        if frames > 65536
            || output.len() < frames * 2
            || planes.len() != self.active.len()
            || self
                .clock
                .checked_add(frames as u64 + u64::from(u32::MAX))
                .is_none()
        {
            return Err(Error::InvalidArgument("Invalid room frame dimensions"));
        }
        for plane in planes.clone().flatten() {
            if plane.len() < frames || plane[..frames].iter().any(|v| !v.is_finite()) {
                return Err(Error::InvalidArgument("Invalid room source PCM"));
            }
            if self.ended.is_some() && plane[..frames].iter().any(|v| *v != 0.) {
                return Err(Error::InvalidArgument("Room input has already ended"));
            }
        }
        if self.ended.is_some() && (!initial.is_empty() || !events.is_empty()) {
            return Err(Error::InvalidArgument(
                "Source controls are frozen while draining",
            ));
        }
        for command in initial {
            self.validate_command(command)?;
            if command.offset != 0 {
                return Err(Error::InvalidArgument("Room initial offset must be zero"));
            }
        }
        let mut previous = 0;
        for command in events {
            self.validate_command(command)?;
            if command.offset as usize >= frames || command.offset < previous {
                return Err(Error::InvalidArgument("Unordered room event offsets"));
            }
            previous = command.offset;
        }
        for command in initial {
            let slot = &mut self.slots[self.active[command.element as usize]];
            slot.control.initialize(command.state);
            slot.marks[self.fill] = true;
            slot.hard[self.fill] = true;
        }
        let mut event = 0;
        let mut frame = 0;
        while frame < frames {
            while event < events.len() && events[event].offset as usize == frame {
                let command = &events[event];
                let slot = &mut self.slots[self.active[command.element as usize]];
                slot.control.apply(command, self.clock)?;
                if (command.changed | command.cleared) & control::GEOMETRY_FIELDS != 0 {
                    slot.marks[self.fill] = true;
                    // Jumps, clears and reference-frame changes cannot wait for
                    // the reflection grid, even while another field is ramping.
                    slot.hard[self.fill] |= command.duration == 0
                        || command.cleared & control::GEOMETRY_FIELDS != 0
                        || command.changed & (64 | 128 | 256) != 0;
                }
                event += 1;
            }
            if self.fill == 0 {
                self.geometry_marks[self.fill] = true;
            }
            if self.fill == 0 || self.pose_changed {
                self.marks[self.fill] = true;
                self.pose_changed = false;
            }
            // No control event or DSP block boundary lies inside this span.
            // Finish each source's contiguous input while its control/delay
            // state is hot, preserving that source's exact sample order.
            let next_event = events.get(event).map_or(frames, |c| c.offset as usize);
            let count = (BLOCK - self.fill)
                .min(frames - frame)
                .min(next_event - frame);
            // The pose only changes between calls. Each source samples it at
            // its own update points, not only at the session's.
            self.poses[self.fill..self.fill + count].fill(self.pose);
            for slot in &mut self.slots {
                if !slot.active {
                    slot.input_span(
                        None,
                        self.clock,
                        self.fill,
                        self.scene,
                        &self.marks[self.fill..self.fill + count],
                    )?;
                }
            }
            for (index, plane) in self.active.iter().zip(planes.clone()) {
                self.slots[*index].input_span(
                    plane.map(|v| &v[frame..frame + count]),
                    self.clock,
                    self.fill,
                    self.scene,
                    &self.marks[self.fill..self.fill + count],
                )?;
            }
            output[frame * 2..(frame + count) * 2]
                .copy_from_slice(&self.fifo[self.fill * 2..(self.fill + count) * 2]);
            self.fill += count;
            self.clock += count as u64;
            frame += count;
            if self.fill == BLOCK {
                self.render_block()?;
                self.fill = 0;
                self.marks.fill(false);
                self.geometry_marks.fill(false);
                for slot in &mut self.slots {
                    slot.marks.fill(false);
                    slot.hard.fill(false);
                }
            }
        }
        if self
            .ended
            .is_some_and(|end| self.clock >= end + self.end_tail as u64)
            && self.phase >= 2
        {
            self.finish_switch();
        }
        Ok(self.info())
    }
    fn render_block(&mut self) -> Result<()> {
        let _profile = super::profile::Guard::new(super::profile::Stage::CanonicalBlock);
        let start = self.clock - BLOCK as u64;
        if self.phase == 0
            && self.retired.is_none()
            && self.pending_request() != 0
            && let Some((request, next)) = self.pending.take()
        {
            self.incoming = Some(next);
            self.incoming_request = request;
            self.begin_warmup();
        }
        let mut old_gain = [1.; BLOCK];
        let mut new_gain = [0.; BLOCK];
        if self.phase == 2 {
            for i in 0..BLOCK {
                let alpha = ((self.fade_done + i) as f32 / super::ROOM_FADE as f32).min(1.);
                old_gain[i] = 1. - alpha;
                new_gain[i] = alpha;
            }
        } else if self.phase == 3 {
            old_gain.fill(0.);
            new_gain.fill(1.);
        }
        self.current.prepare_render(
            &self.slots,
            &self.marks,
            &self.poses,
            &old_gain,
            start,
            self.metres,
            self.scene,
            self.spread,
            self.phase != 3,
            true,
        )?;
        if let Some(incoming) = &mut self.incoming {
            incoming.prepare_render(
                &self.slots,
                &self.marks,
                &self.poses,
                &new_gain,
                start,
                self.metres,
                self.scene,
                self.spread,
                true,
                // Incoming output is inaudible during warmup. EQ and all raw
                // convolution histories still advance. Targets only need one
                // complete transition window before their first audible sample.
                self.phase != 1
                    || self.warm_remaining
                        <= FILTER_FADE + 2 * BLOCK + self.head_room.quality.reflection_frames(),
            )?;
        }
        {
            let mut job = self.paired.lock().unwrap();
            job.order[..self.order.len()].copy_from_slice(&self.order);
            job.sources = self.order.len();
            job.draining = self.phase == 3;
            job.drain_remaining = self.drain_remaining;
            job.old_gain = old_gain;
            job.new_gain = new_gain;
            job.marks = self.marks;
            job.poses = self.poses;
        }
        // A single owned job covers voices, fixed-order reduction, the world
        // field and its decoder: one pool handoff per block for any source
        // count. Nested stages stay on the same worker pool.
        self.current.prepared.workers.run(self.paired.clone())?;
        self.fifo = self.paired.lock().unwrap().output;
        for index in &self.order {
            let slot = &mut self.slots[*index];
            if slot.description.role == 2 {
                continue;
            }
            if slot.head.iter().any(|v| *v != 0.) {
                if start >= slot.head_signal.until {
                    slot.head_signal.reset();
                }
                slot.head_signal.until = self.clock + slot.head_signal.filter.len() as u64 / 2 - 1;
            }
            if start >= slot.head_signal.until {
                continue;
            }
            slot.head_signal.convolution.begin(&slot.head)?;
            let mut begin = 0;
            while begin < BLOCK {
                // Pose changes are not update points for head-locked dry paths.
                let end = (begin + 1..BLOCK)
                    .find(|i| self.geometry_marks[*i] || slot.marks[*i])
                    .unwrap_or(BLOCK);
                if start + begin as u64 >= slot.born {
                    let shape = Shape::from_state(
                        slot.states[begin],
                        slot.description.position,
                        slot.description.role,
                        self.spread,
                    );
                    slot.head_signal.update(
                        &self.head_room,
                        shape,
                        slot.directions[begin],
                        self.metres,
                        self.scene,
                        &mut self.head_samples,
                        &mut self.head_work,
                    )?;
                    slot.head_signal.convolution.render_range(
                        begin,
                        end - begin,
                        &mut self.head_output,
                    )?;
                    for i in begin..end {
                        for ear in 0..2 {
                            self.fifo[2 * i + ear] += self.head_output[ear * BLOCK + i];
                        }
                    }
                }
                begin = end;
            }
        }
        if self.fifo.iter().any(|v| !v.is_finite()) {
            return Err(Error::RenderFailed("Room DSP produced nonfinite PCM"));
        }
        match self.phase {
            1 => {
                self.warm_remaining = self.warm_remaining.saturating_sub(BLOCK);
                if self.warm_remaining == 0 {
                    self.phase = 2;
                    self.transition_sample = self.clock;
                }
            }
            2 => {
                self.fade_done += BLOCK;
                if self.fade_done >= super::ROOM_FADE {
                    self.phase = 3;
                    self.drain_remaining = self.current.prepared.tail_frames() - BLOCK;
                }
            }
            3 => {
                self.drain_remaining = self.drain_remaining.saturating_sub(BLOCK);
                if self.drain_remaining == 0 {
                    self.finish_switch();
                }
            }
            _ => {}
        }
        Ok(())
    }
}
