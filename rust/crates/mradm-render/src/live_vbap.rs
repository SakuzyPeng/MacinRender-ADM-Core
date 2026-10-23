//! The Scene stream VBAP renderer (PCM consistency 4.5): element generations, the per-frame
//! staging of initial states and metadata updates, the panning tables of Objects (after channel
//! lock and divergence), DirectSpeakers (label routing or the canonical position) and LFE, the
//! one-per-renderer warnings and the `.30-vbap` checkpoints. Moved from the C++
//! `live_vbap_renderer` with the same checks, order, arithmetic and texts; the ramps are the
//! `mradm-dsp` `live_vbap::Mixer`. A frame is staged in full and committed only when it renders.
use crate::routing::{self, Error, Target};
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::live_vbap::{LEVEL, Mixer, PAN, VbapCommand};
use mradm_dsp::scene_math::{self as math, CloudPoint, Speaker as SceneSpeaker};
use mradm_dsp::vbap::{Panner, PannerSource};
use mradm_scene::layouts::{self, Geometry, Layout};
use std::collections::{HashMap, HashSet};

/// The fields of an element state (`live_scene::StateField`).
pub mod field {
    pub const ACTIVE: u64 = 1 << 0;
    pub const LINEAR_GAIN: u64 = 1 << 1;
    pub const POSITION: u64 = 1 << 2;
    pub const EXTENT: u64 = 1 << 3;
    pub const DIFFUSE: u64 = 1 << 4;
    pub const DIVERGENCE: u64 = 1 << 5;
    pub const CHANNEL_LOCK: u64 = 1 << 6;
    pub const SCREEN_REFERENCE: u64 = 1 << 7;
    pub const HEAD_LOCKED: u64 = 1 << 8;
    pub const DIVERGENCE_RANGE: u64 = 1 << 9;
    pub const CHANNEL_LOCK_MAX_DISTANCE: u64 = 1 << 10;
    pub const KNOWN: u64 = (1 << 11) - 1;
}

/// An element state (`live_scene::ObjectState`); `valid_fields` says which fields are set.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct State {
    pub valid_fields: u64,
    pub active: bool,
    pub linear_gain: f32,
    pub position: [f32; 3],
    pub width: f32,
    pub height: f32,
    pub depth: f32,
    pub diffuse: f32,
    pub divergence: f32,
    pub channel_lock: bool,
    pub screen_reference: bool,
    pub head_locked: bool,
    pub divergence_azimuth_range: f32,
    pub divergence_position_range: f32,
    pub channel_lock_max_distance: Option<f32>,
}

impl Default for State {
    fn default() -> Self {
        Self {
            valid_fields: 0,
            active: true,
            linear_gain: 1.,
            position: [0., 1., 0.],
            width: 0.,
            height: 0.,
            depth: 0.,
            diffuse: 0.,
            divergence: 0.,
            channel_lock: false,
            screen_reference: false,
            head_locked: false,
            divergence_azimuth_range: 45.,
            divergence_position_range: 0.,
            channel_lock_max_distance: None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Object,
    DirectSpeaker,
    Lfe,
}

/// An element of a generation; `role` is `None` for a value outside the three roles.
#[derive(Clone, Copy, Debug)]
pub struct Element<'a> {
    pub id: u64,
    pub role: Option<Role>,
    pub speaker_label: &'a [u8],
    pub has_position: bool,
    pub position: [f32; 3],
}

#[derive(Clone, Copy, Debug)]
pub struct Plane<'a> {
    pub element_id: u64,
    pub has_signal: bool,
    pub samples: &'a [f32],
}

#[derive(Clone, Copy, Debug)]
pub struct Entry {
    pub element_id: u64,
    pub state: State,
}

#[derive(Clone, Copy, Debug)]
pub struct Update {
    pub element_id: u64,
    pub offset: u32,
    pub ramp: u32,
    pub jump: bool,
    pub changed: u64,
    pub cleared: u64,
    pub state: State,
}

/// One rendered slice. `trace` is the checkpoint prefix (`<scene renderer key>.30-vbap<N>`);
/// empty writes none.
#[derive(Clone, Copy, Debug)]
pub struct Frame<'a> {
    pub generation: u64,
    pub duration: u32,
    pub planes: &'a [Plane<'a>],
    pub initial: &'a [Entry],
    pub updates: &'a [Update],
    pub trace: &'a [u8],
}

/// `live_scene::DiagnosticCode`.
pub mod code {
    pub const SEMANTIC_DEGRADED: u32 = 1;
    pub const DIRECT_SPEAKER_FALLBACK: u32 = 2;
    pub const MISSING_LFE_OUTPUT: u32 = 3;
}

/// A warning of the last rendered frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    pub code: u32,
    pub element_id: u64,
    pub field: u64,
    pub message: &'static str,
}

pub const DIFFUSE_WARNING: &str = "VBAP treats diffuse as direct energy";
pub const SCREEN_WARNING: &str =
    "VBAP has no live screen reference transform; canonical position is used";
pub const HEAD_LOCKED_WARNING: &str =
    "VBAP has no listener-orientation stage; head-locked is currently neutral";
pub const FALLBACK_WARNING: &str =
    "DirectSpeakers uses its current canonical position instead of fixed label routing";
pub const NO_LFE_WARNING: &str = "LFE element dropped because the output layout has no LFE channel";

pub struct Config<'a> {
    pub layout_id: &'a [u8],
    pub geometry: Geometry,
    /// `speaker_spread_mode != none`.
    pub spread: bool,
    pub split_power: bool,
    pub smoothing_frames: u32,
}

fn error(code: i32, message: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: Vec::new(),
    }
}

fn invalid(message: &str) -> Error {
    error(1, message)
}

fn dsp_error(e: mradm_dsp::Error) -> Error {
    Error {
        code: crate::offline::dsp_code(&e),
        message: e.to_string().into_bytes(),
        context: b"Live VBAP DSP".to_vec(),
    }
}

/// `default_state`: every field but the position is valid; an object's position is too, and a
/// producer position seeds it.
fn default_state(element: &Descriptor) -> State {
    let mut state = State {
        valid_fields: field::KNOWN & !field::POSITION,
        ..State::default()
    };
    if element.role == Role::Object {
        state.valid_fields |= field::POSITION;
    }
    if element.has_position {
        state.position = element.position;
    }
    state
}

fn copy_state_fields(destination: &mut State, source: &State, requested: u64) {
    let fields = source.valid_fields & requested;
    let has = |f: u64| fields & f != 0;
    if has(field::ACTIVE) {
        destination.active = source.active;
    }
    if has(field::LINEAR_GAIN) {
        destination.linear_gain = source.linear_gain;
    }
    if has(field::POSITION) {
        destination.position = source.position;
    }
    if has(field::EXTENT) {
        destination.width = source.width;
        destination.height = source.height;
        destination.depth = source.depth;
    }
    if has(field::DIFFUSE) {
        destination.diffuse = source.diffuse;
    }
    if has(field::DIVERGENCE) {
        destination.divergence = source.divergence;
    }
    if has(field::CHANNEL_LOCK) {
        destination.channel_lock = source.channel_lock;
    }
    if has(field::SCREEN_REFERENCE) {
        destination.screen_reference = source.screen_reference;
    }
    if has(field::HEAD_LOCKED) {
        destination.head_locked = source.head_locked;
    }
    if has(field::DIVERGENCE_RANGE) {
        destination.divergence_azimuth_range = source.divergence_azimuth_range;
        destination.divergence_position_range = source.divergence_position_range;
    }
    if has(field::CHANNEL_LOCK_MAX_DISTANCE) {
        destination.channel_lock_max_distance = source.channel_lock_max_distance;
    }
    destination.valid_fields |= fields;
}

/// Finite values (policy-expanded coordinates and extents may leave the producer ranges), a
/// non-negative gain, divergence ranges and lock distance.
fn valid_state(s: &State) -> bool {
    let has = |f: u64| s.valid_fields & f != 0;
    let finite = |v: &[f32]| v.iter().all(|x| x.is_finite());
    !(s.valid_fields & !field::KNOWN != 0
        || has(field::LINEAR_GAIN) && (!s.linear_gain.is_finite() || s.linear_gain < 0.)
        || has(field::POSITION) && !finite(&s.position)
        || has(field::EXTENT) && !finite(&[s.width, s.height, s.depth])
        || has(field::DIFFUSE) && !s.diffuse.is_finite()
        || has(field::DIVERGENCE) && !s.divergence.is_finite()
        || has(field::DIVERGENCE_RANGE)
            && (!s.divergence_azimuth_range.is_finite()
                || s.divergence_azimuth_range < 0.
                || !s.divergence_position_range.is_finite()
                || s.divergence_position_range < 0.)
        || has(field::CHANNEL_LOCK_MAX_DISTANCE)
            && s.channel_lock_max_distance
                .is_some_and(|d| !d.is_finite() || d < 0.))
}

fn level_for(state: &State) -> f32 {
    if state.valid_fields & field::ACTIVE != 0 && !state.active {
        return 0.;
    }
    if state.valid_fields & field::LINEAR_GAIN != 0 {
        state.linear_gain
    } else {
        1.
    }
}

#[derive(Clone, Debug)]
struct Descriptor {
    id: u64,
    role: Role,
    label: Vec<u8>,
    has_position: bool,
    position: [f32; 3],
}

#[derive(Clone, Debug)]
struct Runtime {
    descriptor: Descriptor,
    target: State,
    initialized: bool,
}

#[derive(Clone, Copy, Debug)]
struct Staged {
    target: State,
    initialized: bool,
    seen_initial: bool,
    seen_pcm: bool,
}

/// A point position: Cartesian x/y/z or polar azimuth/elevation/distance.
#[derive(Clone, Copy, Debug)]
struct Position {
    cartesian: bool,
    value: [f32; 3],
}

/// The panning view of the layout: the non-LFE directions and the panner, made when first used.
struct Pan {
    directions: Vec<f32>,
    is_3d: bool,
    panner: Option<Panner>,
}

impl Pan {
    fn panner(&mut self) -> Result<&Panner, Error> {
        let failed = |e: mradm_dsp::Error| error(4, format!("Rust DSP: {e}"));
        if self.panner.is_none() {
            if !(4..=200_000).contains(&self.directions.len()) {
                return Err(failed(mradm_dsp::Error::InvalidArgument(
                    "Invalid speaker array",
                )));
            }
            self.panner = Some(Panner::new(&self.directions, self.is_3d).map_err(failed)?);
        }
        Ok(self.panner.as_ref().expect("created above"))
    }
}

pub struct Renderer {
    layout: &'static Layout,
    spread: bool,
    split_power: bool,
    smoothing_frames: u32,
    speakers: Vec<SceneSpeaker>,
    lfe_channels: Vec<usize>,
    pan: Pan,
    generation: u64,
    elements: Vec<Runtime>,
    index: HashMap<u64, usize>,
    warned: HashSet<u64>,
    mixer: Option<Mixer>,
    staged: Vec<Staged>,
    planes: Vec<Option<usize>>,
    initial: Vec<VbapCommand>,
    events: Vec<VbapCommand>,
    coefficients: Vec<f32>,
    pending_warned: HashSet<u64>,
    diagnostics: Vec<Diagnostic>,
}

/// LFE speakers sit below the horizon but are not panned, so they do not make a layout 3D.
fn is_2d(layout: &Layout) -> bool {
    layout
        .speakers
        .iter()
        .all(|s| s.is_lfe || s.elevation.abs() < 1.0e-6)
}

fn layout_text(id: &[u8]) -> Vec<u8> {
    [&b"unsupported live VBAP output layout '"[..], id, b"'"].concat()
}

impl Renderer {
    pub fn new(config: &Config) -> Result<Self, Error> {
        let layout = std::str::from_utf8(config.layout_id)
            .ok()
            .and_then(|id| layouts::find(id, config.geometry))
            .ok_or_else(|| error(2, layout_text(config.layout_id)))?;
        if layout.speakers.is_empty() {
            return Err(error(2, "live VBAP output layout has no speakers"));
        }
        Ok(Self {
            layout,
            spread: !is_2d(layout) && config.spread,
            split_power: config.split_power,
            smoothing_frames: config.smoothing_frames,
            speakers: layout
                .speakers
                .iter()
                .map(|s| SceneSpeaker {
                    azimuth: s.azimuth,
                    elevation: s.elevation,
                    is_lfe: u32::from(s.is_lfe),
                })
                .collect(),
            lfe_channels: (0..layout.speakers.len())
                .filter(|&c| layout.speakers[c].is_lfe)
                .collect(),
            pan: Pan {
                directions: layout
                    .speakers
                    .iter()
                    .filter(|s| !s.is_lfe)
                    .flat_map(|s| [s.azimuth, s.elevation])
                    .collect(),
                is_3d: !is_2d(layout),
                panner: None,
            },
            generation: 0,
            elements: Vec::new(),
            index: HashMap::new(),
            warned: HashSet::new(),
            mixer: None,
            staged: Vec::new(),
            planes: Vec::new(),
            initial: Vec::new(),
            events: Vec::new(),
            coefficients: Vec::new(),
            pending_warned: HashSet::new(),
            diagnostics: Vec::new(),
        })
    }

    pub fn output_channels(&self) -> u32 {
        self.layout.speakers.len() as u32
    }

    /// Replaces the generation; a failure keeps the previous one.
    pub fn configure(&mut self, generation: u64, elements: &[Element]) -> Result<(), Error> {
        if elements.len() > u32::MAX as usize {
            return Err(invalid(
                "Live VBAP element count overflows numerical indices",
            ));
        }
        let mut next = Vec::with_capacity(elements.len());
        let mut index = HashMap::with_capacity(elements.len());
        for e in elements {
            let Some(role) = e.role else {
                return Err(invalid("Live VBAP has an invalid or duplicate element"));
            };
            if index.insert(e.id, next.len()).is_some() {
                return Err(invalid("Live VBAP has an invalid or duplicate element"));
            }
            let descriptor = Descriptor {
                id: e.id,
                role,
                label: e.speaker_label.to_vec(),
                has_position: e.has_position,
                position: e.position,
            };
            let defaults = default_state(&descriptor);
            if descriptor.has_position {
                let mut position = defaults;
                position.valid_fields |= field::POSITION;
                if !valid_state(&position) {
                    return Err(invalid("Live VBAP element position is invalid"));
                }
            }
            next.push(Runtime {
                descriptor,
                target: defaults,
                initialized: false,
            });
        }
        let mixer = Mixer::new(next.len() as u32, self.output_channels()).map_err(dsp_error)?;
        self.mixer = Some(mixer);
        self.elements = next;
        self.index = index;
        self.generation = generation;
        self.warned.clear();
        Ok(())
    }

    pub fn reset(&mut self) {
        if let Some(mixer) = self.mixer.as_mut() {
            mixer.reset();
        }
        self.mixer = None;
        self.generation = 0;
        self.elements.clear();
        self.index.clear();
        self.warned.clear();
        self.staged.clear();
        self.diagnostics.clear();
        self.pending_warned.clear();
        self.planes.clear();
        self.initial.clear();
        self.events.clear();
        self.coefficients.clear();
    }

    /// The warnings of the last frame, empty when it failed.
    pub fn diagnostics(&self) -> &[Diagnostic] {
        &self.diagnostics
    }

    /// Renders `frame.duration` interleaved frames into the start of `output`. On failure neither
    /// the output nor any state changes, and no warning is published.
    pub fn render(&mut self, frame: &Frame, output: &mut [f32]) -> Result<(), Error> {
        let channels = self.layout.speakers.len();
        let duration = frame.duration as usize;
        if duration > usize::MAX / channels || output.len() < duration * channels {
            return Err(invalid("live VBAP output buffer is too small"));
        }
        if self.mixer.is_none() || frame.generation != self.generation {
            return Err(invalid("live VBAP frame uses an unconfigured generation"));
        }
        let required = duration * channels;
        if let Err(e) = self.stage(frame) {
            self.diagnostics.clear();
            return Err(e);
        }
        if ENABLED && !frame.trace.is_empty() {
            self.trace(frame.trace);
        }
        let planes = &self.planes;
        let rendered = self.mixer.as_mut().expect("checked above").process(
            frame.duration,
            planes.iter().map(|p| p.map(|j| frame.planes[j].samples)),
            &self.initial,
            &self.events,
            &self.coefficients,
            &mut output[..required],
        );
        if let Err(e) = rendered {
            self.diagnostics.clear();
            return Err(dsp_error(e));
        }
        for (element, staged) in self.elements.iter_mut().zip(&self.staged) {
            element.target = staged.target;
            element.initialized = staged.initialized;
        }
        std::mem::swap(&mut self.warned, &mut self.pending_warned);
        Ok(())
    }

    /// Validates the frame and builds the mixer commands and coefficients.
    fn stage(&mut self, frame: &Frame) -> Result<(), Error> {
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
        self.coefficients.clear();
        self.diagnostics.clear();
        self.pending_warned.clone_from(&self.warned);
        for (j, plane) in frame.planes.iter().enumerate() {
            let found = self.index.get(&plane.element_id).copied();
            let Some(i) = found.filter(|&i| {
                !self.staged[i].seen_pcm
                    && !(plane.has_signal && plane.samples.len() < frame.duration as usize)
            }) else {
                return Err(invalid("Live VBAP PCM plane has an invalid ID or length"));
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
                return Err(invalid("Live VBAP initial state is invalid"));
            };
            self.staged[i].seen_initial = true;
            let mut target = default_state(&self.elements[i].descriptor);
            copy_state_fields(&mut target, &entry.state, entry.state.valid_fields);
            self.staged[i].target = target;
            self.prepare_initial(i)?;
        }
        for i in 0..n {
            if !self.staged[i].initialized {
                self.prepare_initial(i)?;
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
                return Err(invalid("Live VBAP metadata update is invalid"));
            };
            previous = update.offset;
            let target = &mut self.staged[i].target;
            target.valid_fields &= !update.cleared;
            copy_state_fields(target, &update.state, update.changed);
            let target = *target;
            let duration = if update.jump {
                0
            } else if update.ramp != 0 {
                update.ramp
            } else {
                self.smoothing_frames
            };
            let mut command = VbapCommand {
                element: i as u32,
                offset: update.offset,
                duration,
                ..VbapCommand::default()
            };
            if changed & (field::ACTIVE | field::LINEAR_GAIN) != 0 {
                command.fields |= LEVEL;
                command.level = level_for(&target);
            }
            if changed & !(field::ACTIVE | field::LINEAR_GAIN | field::HEAD_LOCKED) != 0 {
                let gains = self.gains_for(i, &target)?;
                command.fields |= PAN;
                command.coefficient_offset = self.coefficients.len() as u64;
                self.coefficients.extend_from_slice(&gains);
            }
            if command.fields != 0 {
                self.events.push(command);
            }
        }
        Ok(())
    }

    /// Commands and panning coefficients exactly as handed to the mixer.
    fn trace(&self, key: &[u8]) {
        let key = String::from_utf8_lossy(key);
        let mut commands = Vec::new();
        let mut levels = Vec::new();
        for c in self.initial.iter().chain(&self.events) {
            commands.extend([
                c.element as i32,
                c.offset as i32,
                c.duration as i32,
                c.fields as i32,
                c.coefficient_offset as i32,
            ]);
            levels.push(c.level);
        }
        if !commands.is_empty() {
            let _ = process(&format!("{key}-commands.i32"), Words::I32(&commands));
            let _ = process(&format!("{key}-levels.f32"), Words::F32(&levels));
        }
        if !self.coefficients.is_empty() {
            let _ = process(
                &format!("{key}-coefficients.f32"),
                Words::F32(&self.coefficients),
            );
        }
    }

    fn prepare_initial(&mut self, i: usize) -> Result<(), Error> {
        let target = self.staged[i].target;
        let gains = self.gains_for(i, &target)?;
        self.initial.push(VbapCommand {
            element: i as u32,
            offset: 0,
            duration: 0,
            fields: PAN | LEVEL,
            coefficient_offset: self.coefficients.len() as u64,
            level: level_for(&target),
            reserved: 0,
        });
        self.coefficients.extend_from_slice(&gains);
        self.staged[i].initialized = true;
        Ok(())
    }

    /// Each source's table times its gain, summed in source order for an object and its
    /// divergence sources, scattered to the non-LFE outputs.
    fn panned_gains(
        &mut self,
        sources: &[PannerSource],
        spread: bool,
        sum: bool,
    ) -> Result<Vec<f32>, Error> {
        if self.pan.directions.len() < 4 {
            return Err(error(
                2,
                "live VBAP layout has fewer than two non-LFE speakers",
            ));
        }
        let real = self.pan.directions.len() / 2;
        let mut scratch = vec![0.; real];
        let mut table = vec![0.; real];
        self.pan
            .panner()?
            .source_gains(sources, spread, sum, &mut scratch, &mut table)
            .map_err(|e| error(4, format!("Rust DSP: {e}")))?;
        let mut panned = table.into_iter();
        Ok(self
            .layout
            .speakers
            .iter()
            .map(|s| {
                if s.is_lfe {
                    0.
                } else {
                    panned.next().unwrap_or(0.)
                }
            })
            .collect())
    }

    // Unit-level panning avoids coupling a new gain target to an older position ramp; the
    // independent content level is applied per sample.
    fn gains_for(&mut self, i: usize, state: &State) -> Result<Vec<f32>, Error> {
        const GAIN: f32 = 1.;
        let descriptor = &self.elements[i].descriptor;
        let id = descriptor.id;
        match descriptor.role {
            Role::Lfe => return Ok(self.lfe_gains(i, GAIN)),
            Role::DirectSpeaker => return self.direct_speaker_gains(i, state, GAIN),
            Role::Object => {}
        }
        let mut position = Position {
            cartesian: true,
            value: state.position,
        };
        if state.channel_lock
            && let Some(locked) = math::channel_lock(
                position.value,
                true,
                state.channel_lock_max_distance,
                &self.speakers,
            )
        {
            position = Position {
                cartesian: false,
                value: locked,
            };
        }
        let sources = divergence(position, state, GAIN);
        let spread = self.spread;
        let result = self.panned_gains(&sources, spread, true)?;
        if state.valid_fields & field::DIFFUSE != 0 && state.diffuse > 1.0e-4 {
            self.warn_once(id, field::DIFFUSE, DIFFUSE_WARNING, code::SEMANTIC_DEGRADED);
        }
        if state.valid_fields & field::SCREEN_REFERENCE != 0 && state.screen_reference {
            self.warn_once(
                id,
                field::SCREEN_REFERENCE,
                SCREEN_WARNING,
                code::SEMANTIC_DEGRADED,
            );
        }
        if state.valid_fields & field::HEAD_LOCKED != 0 && state.head_locked {
            self.warn_once(
                id,
                field::HEAD_LOCKED,
                HEAD_LOCKED_WARNING,
                code::SEMANTIC_DEGRADED,
            );
        }
        Ok(result)
    }

    fn direct_speaker_gains(
        &mut self,
        i: usize,
        state: &State,
        gain: f32,
    ) -> Result<Vec<f32>, Error> {
        let descriptor = &self.elements[i].descriptor;
        let id = descriptor.id;
        let labels: Vec<&[u8]> = if descriptor.label.is_empty() {
            Vec::new()
        } else {
            vec![&descriptor.label]
        };
        let position = if state.valid_fields & field::POSITION != 0 {
            Some(Position {
                cartesian: true,
                value: state.position,
            })
        } else if let Some(target) = routing::target_index(&self.targets(), &labels)
            .filter(|&t| !self.layout.speakers[t].is_lfe)
        {
            let mut gains = vec![0.; self.layout.speakers.len()];
            gains[target] = gain;
            return Ok(gains);
        } else if descriptor.has_position {
            Some(Position {
                cartesian: true,
                value: descriptor.position,
            })
        } else {
            routing::label_position(&labels).map(|[azimuth, elevation]| Position {
                cartesian: false,
                value: [azimuth, elevation, 1.],
            })
        };
        let Some(position) = position else {
            return Err(error(
                2,
                format!("DirectSpeakers element {id} has neither a routable label nor position"),
            ));
        };
        self.warn_once(id, 0, FALLBACK_WARNING, code::DIRECT_SPEAKER_FALLBACK);
        self.panned_gains(&[point_source(position, gain)], false, false)
    }

    fn targets(&self) -> Vec<Target<'static>> {
        self.layout
            .speakers
            .iter()
            .map(|s| Target {
                label: s.label.as_bytes(),
                is_lfe: s.is_lfe,
            })
            .collect()
    }

    fn lfe_gains(&mut self, i: usize, gain: f32) -> Vec<f32> {
        let descriptor = &self.elements[i].descriptor;
        let id = descriptor.id;
        let mut gains = vec![0.; self.layout.speakers.len()];
        let Some(&first) = self.lfe_channels.first() else {
            self.warn_once(id, 0, NO_LFE_WARNING, code::MISSING_LFE_OUTPUT);
            return gains;
        };
        if self.split_power && self.lfe_channels.len() >= 2 {
            gains[self.lfe_channels[0]] = gain * routing::LFE_SPLIT_POWER_GAIN;
            gains[self.lfe_channels[1]] = gain * routing::LFE_SPLIT_POWER_GAIN;
            return gains;
        }
        if !descriptor.label.is_empty() {
            let wanted = routing::direct_label(&descriptor.label);
            if let Some(&target) = self.lfe_channels.iter().find(|&&c| {
                routing::direct_label(self.layout.speakers[c].label.as_bytes()) == wanted
            }) {
                gains[target] = gain;
                return gains;
            }
        }
        gains[first] = gain;
        gains
    }

    /// One warning per field and code for the generation; the key does not name the element.
    fn warn_once(&mut self, element_id: u64, field: u64, message: &'static str, code: u32) {
        let key = (field << 8) ^ u64::from(code);
        if self.pending_warned.insert(key) {
            self.diagnostics.push(Diagnostic {
                code,
                element_id,
                field,
                message,
            });
        }
    }
}

fn point_source(p: Position, gain: f32) -> PannerSource {
    PannerSource {
        cartesian: u32::from(p.cartesian),
        position: p.value,
        gain,
        ..PannerSource::default()
    }
}

/// The divergence sources of an object at `position`; a negligible divergence keeps the one
/// source as it is, otherwise the sources share its polar distance.
fn divergence(position: Position, state: &State, gain: f32) -> Vec<PannerSource> {
    let mut points = [CloudPoint::default(); 3];
    let count = math::divergence(
        position.value,
        position.cartesian,
        state.divergence,
        state.divergence_azimuth_range,
        state.divergence_position_range,
        gain,
        &mut points,
    );
    let extent = |source: PannerSource| PannerSource {
        width: state.width,
        height: state.height,
        depth: state.depth,
        ..source
    };
    if count == 1 {
        return vec![extent(point_source(position, gain))];
    }
    let distance = if position.cartesian {
        math::cartesian_to_polar(position.value)[2]
    } else {
        position.value[2]
    };
    points[..count]
        .iter()
        .map(|point| {
            extent(point_source(
                Position {
                    cartesian: false,
                    value: [point.azimuth, point.elevation, distance],
                },
                point.weight,
            ))
        })
        .collect()
}
