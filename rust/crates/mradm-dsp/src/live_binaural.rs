//! Transactionally validated Live Scene binaural control and signal state.
//! The preview owns only scalar controls, diffuse delay and tail counters, not FFT history.
#![allow(clippy::too_many_arguments)]
use crate::{
    Error, Result,
    convolution::{LiveConvolver, LiveState},
    diffuse::DiffuseDelay,
    hrtf_filters::{Filters, Lookup},
    scene_math::{self as math, Rotation, Speaker},
};
use std::sync::Arc;
pub const FIELDS: u64 = (1 << 11) - 1;
const HEAD: u64 = 1 << 8;
const BLOCK: usize = 1024;
fn invalid() -> Error {
    Error::InvalidArgument("Invalid Live binaural frame or numerical command")
}
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct State {
    pub valid: u64,
    pub active: u32,
    pub channel_lock: u32,
    pub screen_reference: u32,
    pub head_locked: u32,
    pub gain: f32,
    pub position: [f32; 3],
    pub extent: [f32; 3],
    pub diffuse: f32,
    pub divergence: f32,
    pub divergence_range: [f32; 2],
    pub has_max_distance: u32,
    pub max_distance: f32,
}
impl Default for State {
    fn default() -> Self {
        Self {
            valid: FIELDS,
            active: 1,
            channel_lock: 0,
            screen_reference: 0,
            head_locked: 0,
            gain: 1.,
            position: [0., 1., 0.],
            extent: [0.; 3],
            diffuse: 0.,
            divergence: 0.,
            divergence_range: [45., 0.],
            has_max_distance: 0,
            max_distance: 0.,
        }
    }
}
impl State {
    fn validate(&self) -> Result<()> {
        if self.valid & !FIELDS != 0
            || [
                self.active,
                self.channel_lock,
                self.screen_reference,
                self.head_locked,
                self.has_max_distance,
            ]
            .iter()
            .any(|x| *x > 1)
            || [self.gain, self.diffuse, self.divergence, self.max_distance]
                .iter()
                .chain(self.position.iter())
                .chain(self.extent.iter())
                .chain(self.divergence_range.iter())
                .any(|x| !x.is_finite())
        {
            return Err(invalid());
        }
        Ok(())
    }
    fn copy_fields(&mut self, source: Self, requested: u64) {
        let fields = source.valid & requested;
        if fields & 1 != 0 {
            self.active = source.active;
        }
        if fields & 2 != 0 {
            self.gain = source.gain;
        }
        if fields & 4 != 0 {
            self.position = source.position;
        }
        if fields & 8 != 0 {
            self.extent = source.extent;
        }
        if fields & 16 != 0 {
            self.diffuse = source.diffuse;
        }
        if fields & 32 != 0 {
            self.divergence = source.divergence;
        }
        if fields & 64 != 0 {
            self.channel_lock = source.channel_lock;
        }
        if fields & 128 != 0 {
            self.screen_reference = source.screen_reference;
        }
        if fields & HEAD != 0 {
            self.head_locked = source.head_locked;
        }
        if fields & 512 != 0 {
            self.divergence_range = source.divergence_range;
        }
        if fields & 1024 != 0 {
            self.has_max_distance = source.has_max_distance;
            self.max_distance = source.max_distance;
        }
        self.valid |= fields;
    }
    fn interpolate(self, target: Self, alpha: f32, contract: bool) -> Self {
        let lerp = |a, b| math::cpp_madd(b - a, alpha, a, contract);
        let mut out = self;
        out.gain = lerp(self.gain, target.gain);
        out.position = std::array::from_fn(|i| lerp(self.position[i], target.position[i]));
        out.extent = std::array::from_fn(|i| lerp(self.extent[i], target.extent[i]));
        out.diffuse = lerp(self.diffuse, target.diffuse);
        out.divergence = lerp(self.divergence, target.divergence);
        out.divergence_range =
            std::array::from_fn(|i| lerp(self.divergence_range[i], target.divergence_range[i]));
        if self.has_max_distance != 0 && target.has_max_distance != 0 {
            out.max_distance = lerp(self.max_distance, target.max_distance);
        }
        if alpha >= 1. {
            out.active = target.active;
            out.channel_lock = target.channel_lock;
            out.screen_reference = target.screen_reference;
            out.head_locked = target.head_locked;
            out.has_max_distance = target.has_max_distance;
            out.max_distance = target.max_distance;
            out.valid = target.valid;
        }
        out
    }
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct Description {
    pub role: u32,
    pub reserved: u32,
    pub fallback: [f32; 2],
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct Command {
    pub element: u32,
    pub offset: u32,
    pub duration: u32,
    pub reserved: u32,
    pub changed: u64,
    pub cleared: u64,
    pub state: State,
    pub direction: [f32; 2],
    pub has_direction: u32,
    pub diagnostic: u32,
}
#[derive(Clone, Copy, Default, Debug, PartialEq)]
#[repr(C)]
pub struct Control {
    pub current: State,
    pub target: State,
    pub direction: [f32; 2],
    pub target_direction: [f32; 2],
    pub remaining: [u32; 11],
    pub has_direction: u32,
    pub initialized: u32,
}
impl Control {
    fn initialize(&mut self, c: &Command) {
        self.current = c.state;
        self.target = c.state;
        self.direction = c.direction;
        self.target_direction = c.direction;
        self.has_direction = c.has_direction;
        self.remaining = [0; 11];
        self.initialized = 1;
    }
    fn update(&mut self, c: &Command) {
        self.target = c.state;
        if c.changed & HEAD != 0 {
            self.current.head_locked = c.state.head_locked;
            self.current.valid |= HEAD;
        }
        if c.changed == HEAD && c.cleared == 0 {
            return;
        }
        if c.has_direction != 0 {
            self.target_direction = c.direction;
        }
        for field in 0..11 {
            let mask = 1 << field;
            if (c.changed | c.cleared) & mask == 0 || mask == HEAD {
                continue;
            }
            self.remaining[field] = c.duration;
            if c.duration == 0 {
                self.current.copy_fields(c.state, mask);
                self.current.valid &= !(c.cleared & mask);
                if mask == 4 {
                    self.direction = self.target_direction;
                }
            }
        }
    }
    fn end_state(&self, frames: u32, contract: bool) -> State {
        let mut out = self.current;
        for (field, remaining) in self.remaining.into_iter().enumerate() {
            if remaining == 0 {
                continue;
            }
            let advanced = frames.min(remaining);
            let mask = 1 << field;
            let sample =
                self.current
                    .interpolate(self.target, advanced as f32 / remaining as f32, contract);
            out.copy_fields(sample, mask);
            if advanced == remaining && self.target.valid & mask == 0 {
                out.valid &= !mask;
            }
        }
        out
    }
    fn end_direction(&self, frames: u32, contract: bool) -> [f32; 2] {
        let rem = self.remaining[2];
        if self.has_direction == 0 || rem == 0 {
            return self.direction;
        }
        let a = frames.min(rem) as f32 / rem as f32;
        if a >= 1. {
            return self.target_direction;
        }
        let delta = math::remainder_degrees(self.target_direction[0] - self.direction[0]);
        [
            math::cpp_madd(delta, a, self.direction[0], contract),
            math::cpp_madd(
                self.target_direction[1] - self.direction[1],
                a,
                self.direction[1],
                contract,
            ),
        ]
    }
    fn advance(&mut self, frames: u32, contract: bool) {
        self.direction = self.end_direction(frames, contract);
        self.current = self.end_state(frames, contract);
        self.remaining = self.remaining.map(|r| r - r.min(frames));
    }
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Diagnostic {
    pub element: u32,
    pub kind: u32,
}
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Report {
    pub count: u32,
    pub mask: u32,
    pub records: [Diagnostic; 4],
}
impl Report {
    fn emit(&mut self, element: usize, kind: u32) {
        if kind == 0 || self.mask & (1 << kind) != 0 {
            return;
        }
        self.mask |= 1 << kind;
        self.records[self.count as usize] = Diagnostic {
            element: element as u32,
            kind,
        };
        self.count += 1;
    }
}
#[derive(Clone, Copy, Default)]
struct Signal {
    initialized: bool,
    tail: usize,
}
struct Element {
    control: Control,
    diffuse: DiffuseDelay,
    convolution: Option<LiveState>,
}
#[derive(Clone, Copy, Default)]
struct Preview {
    control: Control,
    diffuse: DiffuseDelay,
    signal: Signal,
}
pub struct Session {
    descriptions: Vec<Description>,
    elements: Vec<Element>,
    preview: Vec<Preview>,
    active: usize,
    filters: Arc<Filters>,
    convolver: LiveConvolver,
    source: Vec<f32>,
    left: Vec<f32>,
    right: Vec<f32>,
    hrtf: Vec<f32>,
    query: Vec<f32>,
    spread: u32,
    contract: bool,
}
impl Session {
    pub fn new(
        filters: Arc<Filters>,
        descriptions: &[Description],
        rate: u32,
        spread: u32,
        contract: bool,
    ) -> Result<Self> {
        if descriptions.len() > u32::MAX as usize
            || descriptions.len()
                > isize::MAX as usize / (size_of::<Element>() + size_of::<Preview>())
            || spread > 2
            || descriptions
                .iter()
                .any(|d| d.role > 2 || d.reserved != 0 || d.fallback.iter().any(|x| !x.is_finite()))
        {
            return Err(invalid());
        }
        let hrtf_length = filters.output_len() / 2 - 2;
        let convolver = LiveConvolver::new(hrtf_length, BLOCK, rate)?;
        let elements = descriptions
            .iter()
            .map(|d| Element {
                control: Control::default(),
                diffuse: DiffuseDelay::default(),
                convolution: if d.role == 2 {
                    None
                } else {
                    Some(convolver.make_state())
                },
            })
            .collect();
        let n = filters.output_len();
        Ok(Self {
            descriptions: descriptions.to_vec(),
            elements,
            preview: vec![Preview::default(); descriptions.len()],
            active: descriptions.len(),
            filters,
            convolver,
            source: vec![0.; BLOCK],
            left: vec![0.; BLOCK],
            right: vec![0.; BLOCK],
            hrtf: vec![0.; n],
            query: vec![0.; n],
            spread,
            contract,
        })
    }
    pub fn tail_frames(&self) -> usize {
        self.convolver.tail_frames() + 32
    }
    pub fn reset(&mut self) {
        for e in &mut self.elements {
            e.control = Control::default();
            e.diffuse.reset();
            if let Some(s) = &mut e.convolution {
                s.reset();
            }
        }
        self.preview.fill(Preview::default());
        self.active = 0;
    }
    pub fn control(&self, index: usize) -> Result<Control> {
        if index >= self.active {
            return Err(invalid());
        }
        Ok(self.elements[index].control)
    }
    fn read_control(&self, index: usize, preview: bool) -> Control {
        if preview {
            self.preview[index].control
        } else {
            self.elements[index].control
        }
    }
    fn write_control(&mut self, index: usize, preview: bool, value: Control) {
        if preview {
            self.preview[index].control = value;
        } else {
            self.elements[index].control = value;
        }
    }
    fn validate_command(&self, c: &Command) -> Result<()> {
        if c.element as usize >= self.active
            || c.reserved != 0
            || (c.changed | c.cleared) & !FIELDS != 0
            || c.has_direction > 1
            || c.diagnostic > 1
            || c.direction.iter().any(|x| !x.is_finite())
        {
            return Err(invalid());
        }
        c.state.validate()?;
        if self.descriptions[c.element as usize].role == 1 && c.has_direction == 0 {
            return Err(invalid());
        }
        Ok(())
    }
    pub fn process<'a, I>(
        &mut self,
        frames: u32,
        planes: I,
        initial: &[Command],
        events: &[Command],
        pose: [f32; 3],
        warned: u32,
        output: &mut [f32],
    ) -> Result<Report>
    where
        I: Iterator<Item = Option<&'a [f32]>> + Clone + ExactSizeIterator,
    {
        let required = (frames as usize).checked_mul(2).ok_or_else(invalid)?;
        if output.len() < required
            || !output.len().is_multiple_of(2)
            || planes.len() != self.active
            || warned & !30 != 0
        {
            return Err(invalid());
        }
        let rotation = Rotation::new_compat(pose, self.contract)?;
        for (i, p) in planes.clone().enumerate() {
            if let Some(p) = p
                && (p.len() < frames as usize
                    || (self.descriptions[i].role != 2
                        && p[..frames as usize].iter().any(|x| !x.is_finite())))
            {
                return Err(invalid());
            }
        }
        for c in initial {
            self.validate_command(c)?;
            if c.offset != 0 {
                return Err(invalid());
            }
        }
        let mut offset = 0;
        for c in events {
            self.validate_command(c)?;
            if c.offset < offset || c.offset >= frames {
                return Err(invalid());
            }
            offset = c.offset;
        }
        for (e, p) in self.elements[..self.active].iter().zip(&mut self.preview) {
            p.control = e.control;
            p.diffuse = e.diffuse;
            p.signal = e
                .convolution
                .as_ref()
                .map_or(Signal::default(), |s| Signal {
                    initialized: s.initialized(),
                    tail: s.tail_remaining(),
                });
        }
        self.run(
            frames,
            planes.clone(),
            initial,
            events,
            &rotation,
            pose != [0.; 3],
            warned,
            true,
            None,
        )?;
        output[..required].fill(0.);
        // All user-recoverable failures were checked above, including every diffuse result
        // and spectrum consumed by the convolution. A remaining error is an invariant bug.
        Ok(self
            .run(
                frames,
                planes,
                initial,
                events,
                &rotation,
                pose != [0.; 3],
                warned,
                false,
                Some(output),
            )
            .expect("prevalidated Live binaural processing"))
    }
    fn run<'a, I>(
        &mut self,
        frames: u32,
        planes: I,
        initial: &[Command],
        events: &[Command],
        rotation: &Rotation,
        rotate: bool,
        warned: u32,
        preview: bool,
        mut output: Option<&mut [f32]>,
    ) -> Result<Report>
    where
        I: Iterator<Item = Option<&'a [f32]>> + Clone + ExactSizeIterator,
    {
        let mut report = Report {
            mask: warned,
            ..Report::default()
        };
        for c in initial {
            let i = c.element as usize;
            let mut control = self.read_control(i, preview);
            control.initialize(c);
            self.write_control(i, preview, control);
            report.emit(i, c.diagnostic);
        }
        for i in 0..self.active {
            if self.read_control(i, preview).initialized == 0 {
                return Err(invalid());
            }
        }
        let mut cursor = 0;
        let mut event = 0;
        while cursor < frames {
            while event < events.len() && events[event].offset == cursor {
                let c = &events[event];
                let i = c.element as usize;
                let mut control = self.read_control(i, preview);
                control.update(c);
                self.write_control(i, preview, control);
                report.emit(i, c.diagnostic);
                event += 1;
            }
            let mut count = (frames - cursor).min(BLOCK as u32);
            if event < events.len() {
                count = count.min(events[event].offset - cursor);
            }
            for i in 0..self.active {
                for r in self.read_control(i, preview).remaining {
                    if r != 0 {
                        count = count.min(r);
                    }
                }
            }
            if count == 0 {
                return Err(invalid());
            }
            for (i, plane) in planes.clone().enumerate() {
                let mut control = self.read_control(i, preview);
                self.render_element(
                    i,
                    control,
                    plane,
                    cursor as usize,
                    count as usize,
                    rotation,
                    rotate,
                    preview,
                    output.as_deref_mut(),
                    &mut report,
                )?;
                control.advance(count, self.contract);
                self.write_control(i, preview, control);
            }
            cursor += count;
        }
        Ok(report)
    }
    fn hrtf_for(
        &mut self,
        index: usize,
        state: State,
        direction: Option<[f32; 2]>,
        rotation: &Rotation,
        rotate: bool,
        report: &mut Report,
    ) -> Result<()> {
        let description = self.descriptions[index];
        let mut dir = direction.unwrap_or_else(|| {
            if state.valid & 4 != 0 {
                let p = math::cartesian_to_polar(state.position);
                [p[0], p[1]]
            } else {
                description.fallback
            }
        });
        if state.channel_lock != 0 {
            let speakers = [
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
            if let Some((i, d)) =
                math::nearest_compat([dir[0], dir[1], 1.], false, &speakers, self.contract)
                && (state.has_max_distance == 0 || d <= state.max_distance + 1e-4)
            {
                dir = [speakers[i].azimuth, speakers[i].elevation];
            }
        }
        if state.screen_reference != 0 {
            report.emit(index, 2);
        }
        let [az, el] = dir;
        let mut directions = [[0f32; 3]; 8];
        directions[0] = [az, el, 1.];
        let mut count = 1;
        if description.role == 0 {
            let d = math::clamp(state.divergence, 0., 1.);
            if d > 0. {
                let mut angle = state.divergence_range[0];
                if state.divergence_range[1] > 0. && state.valid & 4 != 0 {
                    let [x, y, z] = state.position.map(f64::from);
                    let distance = ((x * x) + (y * y) + (z * z)).sqrt().max(1e-6);
                    angle = (f64::from(state.divergence_range[1]).atan2(distance)
                        * (180. / std::f64::consts::PI)) as f32;
                }
                angle = math::clamp(angle, 0., 120.);
                let side = d / (d + 1.);
                directions[0][2] = (1. - d) / (d + 1.);
                directions[1] = [az - angle, el, side];
                directions[2] = [az + angle, el, side];
                count = 3;
            }
            let width = math::clamp(state.extent[0], 0., 1.) * 60.;
            let height = math::clamp(state.extent[1], 0., 1.) * 45.;
            let depth = math::clamp(state.extent[2], 0., 1.) * 20.;
            let has_extent = width > 0.01 || height > 0.01 || depth > 0.01;
            if has_extent && self.spread == 1 {
                report.emit(index, 3);
            } else if has_extent {
                if self.spread == 2 {
                    report.emit(index, 4);
                }
                directions[count] = [az - width, el, 0.5];
                directions[count + 1] = [az + width, el, 0.5];
                directions[count + 2] = [az, math::clamp(el - height, -90., 90.), 0.5];
                directions[count + 3] = [az, math::clamp(el + height, -90., 90.), 0.5];
                count += 4;
                if depth > 0.01 {
                    directions[count] = [az + 180., el, depth / 20.];
                    count += 1;
                }
            }
        }
        let mut sum = 0.;
        for d in &directions[..count] {
            sum += d[2];
        }
        self.hrtf.fill(0.);
        for d in &directions[..count] {
            let [az, el] = if rotate && state.head_locked == 0 {
                rotation.apply(d[0], d[1], false)
            } else {
                [d[0], d[1]]
            };
            self.filters
                .query(az, el, Lookup::Continuous, &mut self.query, None)?;
            let normalized = d[2] / math::max(sum, 1e-6);
            for (out, sample) in self.hrtf.iter_mut().zip(&self.query) {
                *out += *sample * normalized;
            }
        }
        if self.hrtf.iter().any(|x| !x.is_finite()) {
            return Err(invalid());
        }
        Ok(())
    }
    fn render_element(
        &mut self,
        index: usize,
        control: Control,
        plane: Option<&[f32]>,
        offset: usize,
        frames: usize,
        rotation: &Rotation,
        rotate: bool,
        preview: bool,
        mut output: Option<&mut [f32]>,
        report: &mut Report,
    ) -> Result<()> {
        let start = control.current;
        let end = control.end_state(frames as u32, self.contract);
        if let Some(plane) = plane {
            self.source[..frames].copy_from_slice(&plane[offset..offset + frames]);
        } else {
            self.source[..frames].fill(0.);
        }
        let start_gain = if start.active != 0 { start.gain } else { 0. };
        let end_gain = if end.active != 0 { end.gain } else { 0. };
        if self.descriptions[index].role == 2 {
            if let Some(out) = &mut output {
                for i in 0..frames {
                    let alpha = i as f32 / frames as f32;
                    let gain =
                        math::cpp_madd(end_gain - start_gain, alpha, start_gain, self.contract);
                    let sample = self.source[i] * gain;
                    out[(offset + i) * 2] += sample;
                    out[(offset + i) * 2 + 1] += sample;
                }
            }
            return Ok(());
        }
        let mut diffuse = if preview {
            self.preview[index].diffuse
        } else {
            self.elements[index].diffuse
        };
        diffuse.mix(
            &mut self.source[..frames],
            [start_gain, end_gain],
            [start.diffuse, end.diffuse],
        )?;
        if self.source[..frames].iter().any(|x| !x.is_finite()) {
            return Err(invalid());
        }
        let mut signal = if preview {
            self.preview[index].signal
        } else {
            let s = self.elements[index]
                .convolution
                .as_ref()
                .expect("non-LFE convolution");
            Signal {
                initialized: s.initialized(),
                tail: s.tail_remaining(),
            }
        };
        let has_signal = self.source[..frames].iter().any(|x| *x != 0.);
        if signal.tail == 0 && !has_signal {
            if !preview && signal.initialized {
                self.elements[index]
                    .convolution
                    .as_mut()
                    .expect("non-LFE convolution")
                    .reset();
            }
            signal.initialized = false;
        } else {
            if !signal.initialized {
                self.hrtf_for(
                    index,
                    start,
                    if control.has_direction != 0 {
                        Some(control.direction)
                    } else {
                        None
                    },
                    rotation,
                    rotate,
                    report,
                )?;
                if !preview {
                    self.convolver.initialize(
                        self.elements[index]
                            .convolution
                            .as_mut()
                            .expect("non-LFE convolution"),
                        &self.hrtf,
                    )?;
                }
                signal.initialized = true;
            }
            let direction = if control.has_direction != 0 {
                Some(control.end_direction(frames as u32, self.contract))
            } else {
                None
            };
            self.hrtf_for(index, end, direction, rotation, rotate, report)?;
            if !preview {
                let spatial_ramp = control
                    .remaining
                    .iter()
                    .enumerate()
                    .any(|(field, r)| (*r != 0) && ((1 << field) & (1 | 2 | 16 | HEAD) == 0));
                self.convolver.process(
                    self.elements[index]
                        .convolution
                        .as_mut()
                        .expect("non-LFE convolution"),
                    &self.hrtf,
                    &self.source[..frames],
                    &mut self.left[..frames],
                    &mut self.right[..frames],
                    spatial_ramp,
                )?;
                let out = output.expect("committed output");
                for i in 0..frames {
                    out[(offset + i) * 2] += self.left[i];
                    out[(offset + i) * 2 + 1] += self.right[i];
                }
            }
            signal.tail = if has_signal {
                self.convolver.tail_frames()
            } else {
                signal.tail.saturating_sub(frames)
            };
        }
        if preview {
            self.preview[index].diffuse = diffuse;
            self.preview[index].signal = signal;
        } else {
            self.elements[index].diffuse = diffuse;
        }
        Ok(())
    }
}
