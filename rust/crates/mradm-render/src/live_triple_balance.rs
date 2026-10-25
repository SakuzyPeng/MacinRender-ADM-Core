//! The Scene stream Triple Balance renderer (PCM consistency 4.6): element generations with the
//! RC 7.1.2 bed lanes, the per-frame staging of initial states and metadata updates, the state
//! rules of the room panner, the spread=none warning and the `.30-triple` checkpoints. Moved from
//! the C++ `live_triple_balance_renderer` with the same checks, order and texts; the ramps are the
//! `mradm-dsp` `triple_balance::live::Mixer`. A frame is staged in full and committed only when
//! it renders a nonempty slice. New warnings of one frame follow the generation's element order
//! (the C++ renderer walked an unordered set).
use crate::live::{
    Diagnostic, Element, FrameView, PlaneSource, Role, State, code, copy_state_fields,
    default_state, field, level_for, valid_state,
};
use crate::routing::{self, Error};
use crate::triple_balance::tb_layout;
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::triple_balance::live::{LEVEL, Mixer, POSITION, SIZE, TbLiveCommand, TbLiveElement};
use mradm_dsp::triple_balance::{Layout, Position, bed};
use std::collections::HashMap;

pub const SPREAD_NONE_WARNING: &str =
    "Triple Balance spread=none explicitly ignores extent/diffuse";

/// `SpeakerSpreadMode`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Spread {
    Automatic,
    None,
    Mdap,
}

pub struct Config<'a> {
    pub layout_id: &'a [u8],
    pub standard_geometry: bool,
    pub spread: Spread,
    pub has_sofa: bool,
    pub sample_rate: u32,
    pub split_power: bool,
    pub smoothing_frames: u32,
}

/// The bed labels by RC 7.1.2 channel (L R C LFE Lss Rss Lrs Rrs Lts Rts).
const BED_LABELS: [&[u8]; bed::BED_CHANNELS] = [
    b"M+030", b"M-030", b"M+000", b"LFE1", b"M+090", b"M-090", b"M+135", b"M-135", b"U+090",
    b"U-090",
];

fn error(code: i32, message: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: Vec::new(),
    }
}

fn unsupported(id: u64, field: &str, reason: &str) -> Error {
    Error {
        code: 2,
        message: format!("Live Triple Balance {reason}").into_bytes(),
        context: format!("element={id}; field={field}").into_bytes(),
    }
}

fn dsp_error(e: mradm_dsp::Error, context: &[u8]) -> Error {
    Error {
        code: crate::offline::dsp_code(&e),
        message: e.to_string().into_bytes(),
        context: context.to_vec(),
    }
}

#[derive(Clone, Debug)]
struct Runtime {
    id: u64,
    role: Role,
    position: Option<[f32; 3]>,
    target: State,
    initialized: bool,
}

impl Runtime {
    fn defaults(&self) -> State {
        default_state(self.role, self.position)
    }
}

#[derive(Clone, Copy, Debug)]
struct Staged {
    target: State,
    initialized: bool,
    seen_initial: bool,
    seen_pcm: bool,
}

pub struct Renderer {
    layout: Layout,
    spread_none: bool,
    sample_rate: u32,
    split_power: bool,
    smoothing_frames: u32,
    generation: u64,
    mixer: Option<Mixer>,
    elements: Vec<Runtime>,
    index: HashMap<u64, usize>,
    /// Whether the generation already warned for an element, by element index.
    warned: Vec<bool>,
    staged: Vec<Staged>,
    planes: Vec<Option<usize>>,
    initial: Vec<TbLiveCommand>,
    events: Vec<TbLiveCommand>,
    pending_warned: Vec<bool>,
    diagnostics: Vec<Diagnostic>,
    lane_kinds: Vec<i32>,
    lane_gains: Vec<f32>,
    /// The lanes of a new generation reach the trace on its first frame.
    lanes_pending: bool,
}

impl Renderer {
    pub fn new(config: &Config) -> Result<Self, Error> {
        let layout = tb_layout(config.layout_id)
            .ok_or_else(|| error(2, "Unsupported Triple Balance layout"))?;
        if !config.standard_geometry
            || config.spread == Spread::Mdap
            || config.has_sofa
            || (layout == Layout::Room222 && config.sample_rate != 48000)
        {
            return Err(error(
                2,
                "Live Triple Balance requires fixed room geometry, no MDAP/SOFA and 48 kHz for 22.2",
            ));
        }
        Ok(Self {
            layout,
            spread_none: config.spread == Spread::None,
            sample_rate: config.sample_rate,
            split_power: config.split_power,
            smoothing_frames: config.smoothing_frames,
            generation: 0,
            mixer: None,
            elements: Vec::new(),
            index: HashMap::new(),
            warned: Vec::new(),
            staged: Vec::new(),
            planes: Vec::new(),
            initial: Vec::new(),
            events: Vec::new(),
            pending_warned: Vec::new(),
            diagnostics: Vec::new(),
            lane_kinds: Vec::new(),
            lane_gains: Vec::new(),
            lanes_pending: false,
        })
    }

    pub fn output_channels(&self) -> u32 {
        self.layout.channels() as u32
    }

    /// Replaces the generation; a failure keeps the previous one.
    pub fn configure(&mut self, generation: u64, elements: &[Element]) -> Result<(), Error> {
        if elements.len() > u32::MAX as usize {
            return Err(error(1, "Live Triple Balance has too many elements"));
        }
        let channels = self.layout.channels();
        let mut next = Vec::with_capacity(elements.len());
        let mut index = HashMap::with_capacity(elements.len());
        let mut lanes = Vec::with_capacity(elements.len());
        let mut present = [false; bed::BED_CHANNELS];
        let mut bed_object: &[u8] = &[];
        for e in elements {
            let (true, Some(role)) = (index.insert(e.id, next.len()).is_none(), e.role) else {
                return Err(error(1, "Live Triple Balance invalid or duplicate element"));
            };
            let mut lane = TbLiveElement::default();
            if role != Role::Object {
                let label = if role == Role::Lfe && e.speaker_label.is_empty() {
                    b"LFE1".to_vec()
                } else {
                    routing::direct_label(e.speaker_label)
                };
                let found = BED_LABELS.iter().position(|l| *l == label.as_slice());
                let Some(channel) = found.filter(|_| role != Role::Lfe || label == b"LFE1") else {
                    return Err(unsupported(
                        e.id,
                        "speaker_label",
                        "requires standard 7.1.2 bed labels",
                    ));
                };
                if present[channel] {
                    return Err(unsupported(e.id, "speaker_label", "duplicate bed channel"));
                }
                present[channel] = true;
                if !e.object_id.is_empty() {
                    if !bed_object.is_empty() && bed_object != e.object_id {
                        return Err(unsupported(
                            e.id,
                            "object_id",
                            "multiple beds are unsupported",
                        ));
                    }
                    bed_object = e.object_id;
                }
                bed::gains(
                    self.layout,
                    channel,
                    self.split_power,
                    &mut lane.gains[..channels],
                )
                .map_err(|e| dsp_error(e, b"Triple Balance DSP"))?;
                lane.kind = 1;
            }
            let position = e.has_position.then_some(e.position);
            // Initial states and semantic policy can override descriptor defaults; render()
            // validates the effective state.
            next.push(Runtime {
                id: e.id,
                role,
                position,
                target: default_state(role, position),
                initialized: false,
            });
            lanes.push(lane);
        }
        if present.iter().any(|&p| p) && (self.sample_rate != 48000 || !present.iter().all(|&p| p))
        {
            return Err(error(
                2,
                "Live Triple Balance requires one complete 48 kHz 7.1.2 bed",
            ));
        }
        let mixer = Mixer::new(self.layout, self.sample_rate, &lanes)
            .map_err(|e| dsp_error(e, b"Live Triple Balance DSP"))?;
        // Generation ids may be reused in another epoch: the first frame names the trace.
        self.lane_kinds = lanes.iter().map(|l| l.kind as i32).collect();
        self.lane_gains = lanes.iter().flat_map(|l| l.gains).collect();
        self.lanes_pending = true;
        self.mixer = Some(mixer);
        self.warned = vec![false; next.len()];
        self.elements = next;
        self.index = index;
        self.generation = generation;
        Ok(())
    }

    pub fn reset(&mut self) {
        self.mixer = None;
        self.elements.clear();
        self.index.clear();
        self.warned.clear();
        self.generation = 0;
    }

    /// The warnings of the last frame, empty when it failed.
    pub fn diagnostics(&self) -> &[Diagnostic] {
        &self.diagnostics
    }

    /// Renders `frame.duration` interleaved frames into the start of `output`. On failure the
    /// output, the element states and the warnings are unchanged; an empty slice commits nothing.
    pub fn render<P: PlaneSource + ?Sized>(
        &mut self,
        frame: &FrameView<'_, P>,
        output: &mut [f32],
    ) -> Result<(), Error> {
        self.diagnostics.clear();
        let channels = self.layout.channels();
        let duration = frame.duration as usize;
        if self.mixer.is_none()
            || frame.generation != self.generation
            || duration > usize::MAX / channels
            || output.len() < duration * channels
        {
            return Err(error(
                1,
                "Live Triple Balance invalid generation or output buffer",
            ));
        }
        self.stage(frame)?;
        if ENABLED && !frame.trace.is_empty() {
            self.trace(frame.trace);
        }
        let planes = &self.planes;
        self.mixer
            .as_mut()
            .expect("checked above")
            .process(
                frame.duration,
                planes
                    .iter()
                    .map(|p| p.map(|j| frame.planes.plane(j).samples)),
                &self.initial,
                &self.events,
                &mut output[..duration * channels],
            )
            .map_err(|e| dsp_error(e, b"Live Triple Balance DSP"))?;
        if duration == 0 {
            return Ok(());
        }
        for (element, staged) in self.elements.iter_mut().zip(&self.staged) {
            element.target = staged.target;
            element.initialized = staged.initialized;
        }
        for (i, element) in self.elements.iter().enumerate() {
            if self.pending_warned[i] && !self.warned[i] {
                self.diagnostics.push(Diagnostic {
                    code: code::SEMANTIC_DEGRADED,
                    element_id: element.id,
                    field: field::EXTENT | field::DIFFUSE,
                    message: SPREAD_NONE_WARNING,
                });
            }
        }
        std::mem::swap(&mut self.warned, &mut self.pending_warned);
        Ok(())
    }

    /// Validates the frame and builds the mixer commands.
    fn stage<P: PlaneSource + ?Sized>(&mut self, frame: &FrameView<'_, P>) -> Result<(), Error> {
        let n = self.elements.len();
        self.staged.clear();
        self.staged.extend(self.elements.iter().map(|e| Staged {
            target: e.target,
            initialized: e.initialized,
            seen_initial: false,
            seen_pcm: false,
        }));
        self.planes.clear();
        self.planes.resize(n, None);
        self.initial.clear();
        self.events.clear();
        self.pending_warned.clone_from(&self.warned);
        for (j, plane) in frame.planes.iter().enumerate() {
            let found = self.index.get(&plane.element_id).copied();
            let Some(i) = found.filter(|&i| {
                !self.staged[i].seen_pcm
                    && !(plane.has_signal && plane.samples.len() < frame.duration as usize)
            }) else {
                return Err(error(1, "Live Triple Balance invalid PCM plane"));
            };
            self.staged[i].seen_pcm = true;
            if plane.has_signal {
                self.planes[i] = Some(j);
            }
        }
        for entry in frame.initial {
            let found = self.index.get(&entry.element_id).copied();
            let Some(i) =
                found.filter(|&i| !self.staged[i].seen_initial && valid_state(&entry.state))
            else {
                return Err(error(1, "Live Triple Balance invalid initial state"));
            };
            let mut target = self.elements[i].defaults();
            copy_state_fields(&mut target, &entry.state, entry.state.valid_fields);
            self.staged[i] = Staged {
                target,
                initialized: false,
                seen_initial: true,
                ..self.staged[i]
            };
        }
        for i in 0..n {
            if !self.staged[i].initialized {
                self.validate_state(i)?;
                self.initial
                    .push(self.command(i, 0, 0, POSITION | SIZE | LEVEL));
                self.staged[i].initialized = true;
            }
        }
        let mut previous = 0;
        for update in frame.updates {
            let found = self.index.get(&update.element_id).copied();
            let changed = update.changed | update.cleared;
            let Some(i) = found.filter(|_| {
                update.offset < frame.duration
                    && update.offset >= previous
                    && changed & !field::KNOWN == 0
                    && update.changed & !update.state.valid_fields == 0
                    && valid_state(&update.state)
            }) else {
                return Err(error(1, "Live Triple Balance invalid metadata update"));
            };
            previous = update.offset;
            let defaults = self.elements[i].defaults();
            let target = &mut self.staged[i].target;
            copy_state_fields(target, &defaults, update.cleared);
            target.valid_fields &= !update.cleared;
            copy_state_fields(target, &update.state, update.changed);
            self.validate_state(i)?;
            let duration = if update.jump {
                0
            } else if update.ramp != 0 {
                update.ramp
            } else {
                self.smoothing_frames
            };
            let mut fields = 0;
            if changed & field::POSITION != 0 {
                fields |= POSITION;
            }
            if changed & field::EXTENT != 0 {
                fields |= SIZE;
            }
            if changed & (field::ACTIVE | field::LINEAR_GAIN) != 0 {
                fields |= LEVEL;
            }
            if fields != 0 {
                self.events
                    .push(self.command(i, update.offset, duration, fields));
            }
        }
        Ok(())
    }

    /// The staged state of element `i` against the room panner's rules; a spread=none extent or
    /// diffuse is accepted and warned once.
    fn validate_state(&mut self, i: usize) -> Result<(), Error> {
        let state = &self.staged[i].target;
        let element = &self.elements[i];
        let id = element.id;
        if !valid_state(state) {
            return Err(error(1, "Live Triple Balance invalid state"));
        }
        if state.divergence != 0.
            || state.channel_lock
            || state.screen_reference
            || state.head_locked
        {
            return Err(unsupported(
                id,
                "divergence/channelLock/screenRef/headLocked",
                "does not support these object modifiers",
            ));
        }
        let extent = state.width != 0. || state.height != 0. || state.depth != 0.;
        if element.role != Role::Object {
            if state.valid_fields & field::POSITION != 0 || extent || state.diffuse != 0. {
                return Err(unsupported(
                    id,
                    "position/extent/diffuse",
                    "bed routing is fixed",
                ));
            }
            return Ok(());
        }
        let [x, y, z] = state.position;
        let floor = if self.layout == Layout::Room222 {
            -1.
        } else {
            0.
        };
        if x.abs() > 1. || y.abs() > 1. || z > 1. || z < floor {
            return Err(unsupported(
                id,
                "position",
                "position is outside the supported room",
            ));
        }
        if self.spread_none {
            if extent || state.diffuse != 0. {
                self.pending_warned[i] = true;
            }
        } else if (extent
            && (self.sample_rate != 48000
                || state.width < 0.
                || state.width > 1.
                || state.width != state.height
                || state.width != state.depth))
            || (state.diffuse != 0. && (state.diffuse != 1. || !extent))
        {
            return Err(unsupported(
                id,
                "extent/diffuse",
                "requires a 48 kHz equal-size Object; diffuse must be 0 or 1 with nonzero size",
            ));
        }
        Ok(())
    }

    fn command(&self, i: usize, offset: u32, duration: u32, fields: u32) -> TbLiveCommand {
        let state = &self.staged[i].target;
        let [x, y, z] = if self.elements[i].role == Role::Object {
            state.position
        } else {
            [0., 1., 0.]
        };
        TbLiveCommand {
            element: i as u32,
            offset,
            duration,
            fields,
            position: Position { x, y, z },
            size: if self.spread_none { 0. } else { state.width },
            level: level_for(state),
        }
    }

    /// The generation's lanes on its first frame, then the commands as handed to the mixer.
    fn trace(&mut self, key: &[u8]) {
        let key = String::from_utf8_lossy(key);
        if self.lanes_pending && !self.lane_kinds.is_empty() {
            let _ = process(
                &format!("{key}-lane-kinds.i32"),
                Words::I32(&self.lane_kinds),
            );
            let _ = process(
                &format!("{key}-lane-gains.f32"),
                Words::F32(&self.lane_gains),
            );
        }
        self.lanes_pending = false;
        let mut commands = Vec::new();
        let mut values = Vec::new();
        for c in self.initial.iter().chain(&self.events) {
            commands.extend([
                c.element as i32,
                c.offset as i32,
                c.duration as i32,
                c.fields as i32,
            ]);
            values.extend([c.position.x, c.position.y, c.position.z, c.size, c.level]);
        }
        if !commands.is_empty() {
            let _ = process(&format!("{key}-commands.i32"), Words::I32(&commands));
            let _ = process(&format!("{key}-values.f32"), Words::F32(&values));
        }
    }
}
