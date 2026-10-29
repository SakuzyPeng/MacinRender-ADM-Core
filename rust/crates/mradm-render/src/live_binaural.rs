//! The Scene stream binaural renderer (PCM consistency 4.7 D): element generations with their
//! fallback directions, the per-frame staging of initial states, PCM views and metadata updates,
//! the DirectSpeakers direction rules (state position, the fixed label table, the producer
//! position), the warnings and the `.30-binaural` checkpoints. Moved from the C++
//! `live_binaural_renderer` with the same checks, order and texts; the convolution and ramps are
//! the `mradm-dsp` `live_binaural::Session`. A frame is staged in full and its targets and warning
//! mask are committed only when the session renders it.
use crate::binaural_hrtf::State as Hrtf;
use crate::live::{
    Diagnostic, Element, FrameView, PlaneSource, Role, State, code, copy_state_fields,
    default_state, field, valid_state,
};
use crate::routing::Error;
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::live_binaural::{self as dsp, Command, Description, Session};
use mradm_dsp::scene_math::cartesian_to_polar;
use std::collections::HashMap;
use std::sync::Arc;

const DSP_CONTEXT: &[u8] = b"Live binaural DSP";

pub struct Config {
    pub sample_rate: u32,
    /// `BinauralSpreadMode`: 0 cloud (automatic), 1 none, 2 SAF spreader.
    pub spread: u32,
    pub smoothing_frames: u32,
}

fn error(code: i32, message: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: Vec::new(),
    }
}

fn dsp_error(e: mradm_dsp::Error) -> Error {
    Error {
        code: crate::offline::dsp_code(&e),
        message: e.to_string().into_bytes(),
        context: DSP_CONTEXT.to_vec(),
    }
}

fn polar(position: [f32; 3]) -> [f32; 2] {
    let p = cartesian_to_polar(position);
    [p[0], p[1]]
}

/// The fixed directions of the DirectSpeakers labels the renderer knows, matched exactly.
fn label_position(label: &[u8]) -> Option<[f32; 2]> {
    const POSITIONS: [(&[u8], [f32; 2]); 25] = [
        (b"M+000", [0., 0.]),
        (b"M+030", [30., 0.]),
        (b"M-030", [-30., 0.]),
        (b"M+060", [60., 0.]),
        (b"M-060", [-60., 0.]),
        (b"M+090", [90., 0.]),
        (b"M-090", [-90., 0.]),
        (b"M+110", [110., 0.]),
        (b"M-110", [-110., 0.]),
        (b"M+135", [135., 0.]),
        (b"M-135", [-135., 0.]),
        (b"M+180", [180., 0.]),
        (b"U+000", [0., 30.]),
        (b"U+030", [30., 30.]),
        (b"U-030", [-30., 30.]),
        (b"U+045", [45., 30.]),
        (b"U-045", [-45., 30.]),
        (b"U+110", [110., 30.]),
        (b"U-110", [-110., 30.]),
        (b"U+135", [135., 30.]),
        (b"U-135", [-135., 30.]),
        (b"U+180", [180., 30.]),
        (b"T+000", [0., 90.]),
        (b"B+045", [45., -30.]),
        (b"B-045", [-45., -30.]),
    ];
    POSITIONS
        .iter()
        .find(|(name, _)| *name == label)
        .map(|(_, p)| *p)
}

fn numerical_state(s: &State) -> dsp::State {
    dsp::State {
        valid: s.valid_fields,
        active: u32::from(s.active),
        channel_lock: u32::from(s.channel_lock),
        screen_reference: u32::from(s.screen_reference),
        head_locked: u32::from(s.head_locked),
        gain: s.linear_gain,
        position: s.position,
        extent: [s.width, s.height, s.depth],
        diffuse: s.diffuse,
        divergence: s.divergence,
        divergence_range: [s.divergence_azimuth_range, s.divergence_position_range],
        has_max_distance: u32::from(s.channel_lock_max_distance.is_some()),
        max_distance: s.channel_lock_max_distance.unwrap_or(0.),
    }
}

/// The session's warning kinds 1–4.
fn diagnostic(kind: u32) -> (u32, u64, &'static str) {
    match kind {
        1 => (
            code::DIRECT_SPEAKER_FALLBACK,
            0,
            "DirectSpeakers label is unknown; using its canonical fixed position",
        ),
        2 => (
            code::SEMANTIC_DEGRADED,
            field::SCREEN_REFERENCE,
            "live binaural has no screen transform; canonical position is used",
        ),
        3 => (
            code::SEMANTIC_DEGRADED,
            field::EXTENT,
            "binaural spread mode is none; extent is ignored",
        ),
        4 => (
            code::SEMANTIC_DEGRADED,
            field::EXTENT,
            "dynamic extent uses the live HRTF cloud path; SAF spreader topology is fixed",
        ),
        _ => unreachable!("the session reports kinds 1 to 4"),
    }
}

#[derive(Clone, Debug)]
struct Runtime {
    id: u64,
    role: Role,
    label: Vec<u8>,
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
    hrtf: Arc<Hrtf>,
    sample_rate: u32,
    spread: u32,
    smoothing_frames: u32,
    session: Session,
    generation: u64,
    elements: Vec<Runtime>,
    index: HashMap<u64, usize>,
    staged: Vec<Staged>,
    planes: Vec<Option<usize>>,
    initial: Vec<Command>,
    events: Vec<Command>,
    warned: u32,
    pose: [f32; 3],
    diagnostics: Vec<Diagnostic>,
}

impl Renderer {
    pub fn new(hrtf: Arc<Hrtf>, config: &Config) -> Result<Self, Error> {
        let session = Session::new(
            Arc::clone(&hrtf.filters),
            &[],
            config.sample_rate,
            config.spread,
            false,
        )
        .map_err(dsp_error)?;
        if config.sample_rate == 0 {
            return Err(error(
                4,
                "live binaural renderer has an invalid sample rate",
            ));
        }
        Ok(Self {
            hrtf,
            sample_rate: config.sample_rate,
            spread: config.spread,
            smoothing_frames: config.smoothing_frames,
            session,
            generation: 0,
            elements: Vec::new(),
            index: HashMap::new(),
            staged: Vec::new(),
            planes: Vec::new(),
            initial: Vec::new(),
            events: Vec::new(),
            warned: 0,
            pose: [0.; 3],
            diagnostics: Vec::new(),
        })
    }

    pub fn output_channels(&self) -> u32 {
        2
    }

    /// The input frames the convolution still sounds after the last one.
    pub fn tail_frames(&self) -> u64 {
        (self.hrtf.fft_size - 1) as u64 + 32
    }

    pub fn set_pose(&mut self, pose: [f32; 3]) {
        self.pose = pose;
    }

    /// Replaces the generation; a failure keeps the previous one.
    pub fn configure(&mut self, generation: u64, elements: &[Element]) -> Result<(), Error> {
        if elements.len() > u32::MAX as usize {
            return Err(error(
                1,
                "Live binaural element count overflows numerical indices",
            ));
        }
        let mut next = Vec::with_capacity(elements.len());
        let mut index = HashMap::with_capacity(elements.len());
        let mut descriptions = Vec::with_capacity(elements.len());
        for e in elements {
            let (Some(role), true) = (e.role, !index.contains_key(&e.id)) else {
                return Err(error(
                    1,
                    "Live binaural has an invalid or duplicate element",
                ));
            };
            index.insert(e.id, next.len());
            let position = e.has_position.then_some(e.position);
            let defaults = default_state(role, position);
            if position.is_some() {
                let mut with_position = defaults;
                with_position.valid_fields |= field::POSITION;
                if !valid_state(&with_position) {
                    return Err(error(1, "Live binaural element position is invalid"));
                }
            }
            descriptions.push(Description {
                role: match role {
                    Role::Object => 0,
                    Role::DirectSpeaker => 1,
                    Role::Lfe => 2,
                },
                reserved: 0,
                fallback: position.map_or([0., 0.], polar),
            });
            next.push(Runtime {
                id: e.id,
                role,
                label: e.speaker_label.to_vec(),
                position,
                target: defaults,
                initialized: false,
            });
        }
        let session = Session::new(
            Arc::clone(&self.hrtf.filters),
            &descriptions,
            self.sample_rate,
            self.spread,
            false,
        )
        .map_err(dsp_error)?;
        self.session = session;
        self.elements = next;
        self.index = index;
        self.generation = generation;
        self.warned = 0;
        Ok(())
    }

    /// Clears the generation; the session keeps its elements until the next configure.
    pub fn reset(&mut self) {
        self.session.reset();
        self.generation = 0;
        self.elements.clear();
        self.index.clear();
        self.staged.clear();
        self.initial.clear();
        self.events.clear();
        self.planes.clear();
        self.warned = 0;
    }

    /// The warnings of the last frame, empty when it failed.
    pub fn diagnostics(&self) -> &[Diagnostic] {
        &self.diagnostics
    }

    /// Renders `frame.duration` interleaved stereo frames into the start of `output`. On failure
    /// the element states and the warning mask are unchanged.
    pub fn render<P: PlaneSource + ?Sized>(
        &mut self,
        frame: &FrameView<'_, P>,
        output: &mut [f32],
    ) -> Result<(), Error> {
        self.diagnostics.clear();
        let Some(required) = (frame.duration as usize).checked_mul(2) else {
            return Err(error(1, "Live binaural output size overflows"));
        };
        if output.len() < required || frame.generation != self.generation {
            return Err(error(1, "Live binaural output or generation is invalid"));
        }
        self.stage_initials(frame)?;
        self.stage_planes(frame)?;
        self.stage_events(frame)?;
        if ENABLED && !frame.trace.is_empty() {
            self.trace(frame.trace);
        }
        let planes = &self.planes;
        let report = self
            .session
            .process(
                frame.duration,
                planes
                    .iter()
                    .map(|p| p.map(|j| frame.planes.plane(j).samples)),
                &self.initial,
                &self.events,
                self.pose,
                self.warned,
                &mut output[..required],
            )
            .map_err(dsp_error)?;
        for (element, staged) in self.elements.iter_mut().zip(&self.staged) {
            element.target = staged.target;
            element.initialized = staged.initialized;
        }
        self.warned = report.mask;
        for record in &report.records[..report.count as usize] {
            let (code, field, message) = diagnostic(record.kind);
            self.diagnostics.push(Diagnostic {
                code,
                element_id: self.elements[record.element as usize].id,
                field,
                message,
            });
        }
        Ok(())
    }

    fn stage_initials<P: PlaneSource + ?Sized>(
        &mut self,
        frame: &FrameView<'_, P>,
    ) -> Result<(), Error> {
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
        for entry in frame.initial {
            let found = self.index.get(&entry.element_id).copied();
            let Some(i) =
                found.filter(|&i| !self.staged[i].seen_initial && valid_state(&entry.state))
            else {
                return Err(error(
                    1,
                    "Live binaural initial state is invalid or duplicated",
                ));
            };
            let mut target = self.elements[i].defaults();
            copy_state_fields(&mut target, &entry.state, entry.state.valid_fields);
            let command = self.command(i, &target)?;
            self.initial.push(command);
            let staged = &mut self.staged[i];
            staged.seen_initial = true;
            staged.target = target;
            staged.initialized = true;
        }
        for i in 0..n {
            if !self.staged[i].initialized {
                let command = self.command(i, &self.staged[i].target)?;
                self.initial.push(command);
                self.staged[i].initialized = true;
            }
        }
        Ok(())
    }

    fn stage_planes<P: PlaneSource + ?Sized>(
        &mut self,
        frame: &FrameView<'_, P>,
    ) -> Result<(), Error> {
        for (j, plane) in frame.planes.iter().enumerate() {
            let found = self.index.get(&plane.element_id).copied();
            let Some(i) = found.filter(|&i| {
                !self.staged[i].seen_pcm
                    && !(plane.has_signal && plane.samples.len() < frame.duration as usize)
            }) else {
                return Err(error(1, "Live binaural PCM view is invalid or duplicated"));
            };
            self.staged[i].seen_pcm = true;
            self.planes[i] = plane.has_signal.then_some(j);
        }
        Ok(())
    }

    fn stage_events<P: PlaneSource + ?Sized>(
        &mut self,
        frame: &FrameView<'_, P>,
    ) -> Result<(), Error> {
        let mut previous = 0;
        for update in frame.updates {
            let found = self.index.get(&update.element_id).copied();
            let Some(i) = found.filter(|_| {
                update.offset >= previous
                    && update.offset < frame.duration
                    && (update.changed | update.cleared) & !field::KNOWN == 0
                    && update.changed & !update.state.valid_fields == 0
                    && valid_state(&update.state)
            }) else {
                return Err(error(
                    1,
                    "Live binaural metadata event is invalid or unordered",
                ));
            };
            previous = update.offset;
            let mut target = self.staged[i].target;
            target.valid_fields &= !update.cleared;
            copy_state_fields(&mut target, &update.state, update.changed);
            self.staged[i].target = target;
            let mut command = self.command(i, &target)?;
            command.offset = update.offset;
            command.changed = update.changed;
            command.cleared = update.cleared;
            command.duration = if update.jump {
                0
            } else if update.ramp != 0 {
                update.ramp
            } else {
                self.smoothing_frames
            };
            self.events.push(command);
        }
        Ok(())
    }

    /// An element's numerical command; a DirectSpeakers element also takes its direction: the
    /// state position, else its label's fixed direction (an unknown label warns), else the
    /// producer position.
    fn command(&self, index: usize, state: &State) -> Result<Command, Error> {
        let mut command = Command {
            element: index as u32,
            state: numerical_state(state),
            ..Command::default()
        };
        let element = &self.elements[index];
        if element.role != Role::DirectSpeaker {
            return Ok(command);
        }
        let mut direction = None;
        if state.valid_fields & field::POSITION != 0 {
            direction = Some(polar(state.position));
        } else {
            if !element.label.is_empty() {
                direction = label_position(&element.label);
                if direction.is_none() {
                    command.diagnostic = 1;
                }
            }
            if direction.is_none() {
                direction = element.position.map(polar);
            }
        }
        let Some(direction) = direction else {
            return Err(error(
                2,
                format!(
                    "DirectSpeakers element {} has neither a known label nor a position",
                    element.id
                ),
            ));
        };
        command.has_direction = 1;
        command.direction = direction;
        Ok(command)
    }

    /// The commands as handed to the session: the timing words and the DirectSpeakers
    /// directions (the session copies the states).
    fn trace(&self, key: &[u8]) {
        let key = String::from_utf8_lossy(key);
        let mut commands = Vec::new();
        let mut directions = Vec::new();
        for c in self.initial.iter().chain(&self.events) {
            commands.extend([
                c.element as i32,
                c.offset as i32,
                c.duration as i32,
                c.has_direction as i32,
                c.diagnostic as i32,
                c.changed as u32 as i32,
                c.cleared as u32 as i32,
            ]);
            directions.extend(c.direction);
        }
        if !commands.is_empty() {
            let _ = process(&format!("{key}-commands.i32"), Words::I32(&commands));
            let _ = process(&format!("{key}-directions.f32"), Words::F32(&directions));
        }
    }
}
