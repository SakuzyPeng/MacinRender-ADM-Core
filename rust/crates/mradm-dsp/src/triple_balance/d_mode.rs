//! D mode: coherent room-size distribution and independent metadata/output clocks.
//! Offline 48 kHz native-order 9.1.6 and the project's three-layer 22.2 extension.
mod room_222;
use super::{Event, Layout, Position, count, invalid, panner, session::TbRowInput};
use crate::{Error, Result};
use std::sync::{Arc, OnceLock};

const SPATIAL: usize = 15;
pub const CHANNELS: usize = 16;
const METADATA: u64 = 512;
const CONTROL: u64 = 1536;
const ROOM_MAX: f32 = 32767. / 32768.;
const RADIUS_STEP: f32 = 0.7 / 19.;
type Spatial = [f32; SPATIAL];
type Gains = panner::Gains;
const TOP_FRONT: f32 = 0.241_935_48;
const TOP_REAR: f32 = 0.758_064_5;
const WIDE: f32 = 0.161_290_32;
const NODES: [[f32; 3]; SPATIAL] = [
    [0., 0., 0.],
    [ROOM_MAX, 0., 0.],
    [0.5, 0., 0.],
    [0., 0.5, 0.],
    [ROOM_MAX, 0.5, 0.],
    [0., ROOM_MAX, 0.],
    [ROOM_MAX, ROOM_MAX, 0.],
    [0., WIDE, 0.],
    [ROOM_MAX, WIDE, 0.],
    [TOP_FRONT, TOP_FRONT, ROOM_MAX],
    [TOP_REAR, TOP_FRONT, ROOM_MAX],
    [TOP_FRONT, 0.5, ROOM_MAX],
    [TOP_REAR, 0.5, ROOM_MAX],
    [TOP_FRONT, TOP_REAR, ROOM_MAX],
    [TOP_REAR, TOP_REAR, ROOM_MAX],
];
const ROWS: [&[usize]; 7] = [
    &[0, 2, 1],
    &[7, 8],
    &[3, 4],
    &[5, 6],
    &[9, 10],
    &[11, 12],
    &[13, 14],
];
const LAYERS: [&[usize]; 2] = [&[0, 1, 2, 3], &[4, 5, 6]];
static LATTICE: OnceLock<Box<[Spatial]>> = OnceLock::new();

fn normalize<const N: usize>(mut v: [f32; N]) -> [f32; N] {
    let mut energy = 0f32;
    for x in v {
        energy += x * x;
    }
    let denominator = libm::sqrtf(energy).max(1e-5);
    for x in &mut v {
        *x /= denominator;
    }
    v
}

fn pair(value: f32, knots: &[f32]) -> [f32; 4] {
    let mut result = [0.; 4];
    if value <= knots[0] {
        result[0] = 1.;
    } else if value >= knots[knots.len() - 1] {
        result[knots.len() - 1] = 1.;
    } else {
        for i in 1..knots.len() {
            if value <= knots[i] {
                let t = (value - knots[i - 1]) / (knots[i] - knots[i - 1]);
                let angle = t * std::f32::consts::FRAC_PI_2;
                result[i - 1] = libm::cosf(angle);
                result[i] = libm::sinf(angle);
                break;
            }
        }
    }
    result
}

fn basis(axis: usize, value: f32) -> Spatial {
    let mut result = [0.; SPATIAL];
    match axis {
        0 => {
            for row in ROWS {
                let mut knots = [0.; 3];
                for (i, c) in row.iter().enumerate() {
                    knots[i] = NODES[*c][0];
                }
                let weights = pair(value, &knots[..row.len()]);
                for (i, c) in row.iter().enumerate() {
                    result[*c] = weights[i];
                }
            }
        }
        1 => {
            for layer in LAYERS {
                let mut knots = [0.; 4];
                for (i, r) in layer.iter().enumerate() {
                    knots[i] = NODES[ROWS[*r][0]][1];
                }
                let weights = pair(value, &knots[..layer.len()]);
                for (i, r) in layer.iter().enumerate() {
                    for c in ROWS[*r] {
                        result[*c] = weights[i];
                    }
                }
            }
        }
        _ => {
            let weights = pair(value, &[0., ROOM_MAX]);
            result[..9].fill(weights[0]);
            result[9..].fill(weights[1]);
        }
    }
    result
}

fn repeated(step: f32, count: usize) -> f32 {
    let mut result = 0.;
    for _ in 0..count {
        result += step;
    }
    result
}

fn quadrature(axis: usize, p: usize, r: usize) -> Spatial {
    let count = if axis == 2 { 8 } else { 20 };
    let center = repeated(if axis == 2 { 1. / 3. } else { 1. / 34. }, p);
    let radius = repeated(RADIUS_STEP, r);
    let exponent = panner::exponent(radius);
    let mut position = 0.;
    let mut result = [0.; SPATIAL];
    for _ in 0..count {
        let weight = panner::kernel::<true>(position, center, radius, axis);
        let gains = basis(axis, position);
        for i in 0..SPATIAL {
            result[i] += panner::power(gains[i] * weight, exponent);
        }
        position += 1. / (count - 1) as f32;
    }
    result
}

fn lattice() -> &'static [Spatial] {
    LATTICE.get_or_init(|| {
        let mut values = Vec::with_capacity(1480);
        for axis in 0..3 {
            for p in 0..if axis == 2 { 4 } else { 35 } {
                for r in 0..20 {
                    values.push(quadrature(axis, p, r));
                }
            }
        }
        values.into_boxed_slice()
    })
}

fn extent(axis: usize, p: f32, r: f32) -> Spatial {
    let steps = if axis == 2 { 3 } else { 34 };
    let step = 1. / steps as f32;
    let a = (libm::floorf(p * steps as f32) as usize).min(steps);
    let b = (a + 1).min(steps);
    let x = if a == b {
        0.
    } else {
        ((p - step * a as f32) / step).max(0.)
    };
    let c = (libm::floorf(r / 0.7 * 19.) as usize).min(19);
    let d = (c + 1).min(19);
    let y = if c == d {
        0.
    } else {
        ((r - RADIUS_STEP * c as f32) / RADIUS_STEP).max(0.)
    };
    let weights = [
        1. - x.max(y),
        1. - (1. - x).max(y),
        1. - x.max(1. - y),
        1. - (1. - x).max(1. - y),
    ];
    let sum = weights.iter().sum::<f32>();
    let offsets = [a * 20 + c, b * 20 + c, a * 20 + d, b * 20 + d];
    let table = lattice();
    let mut result = [0.; SPATIAL];
    for i in 0..SPATIAL {
        for k in 0..4 {
            result[i] += table[axis * 700 + offsets[k]][i] * (weights[k] / sum);
        }
    }
    result
}

fn validate(position: Position, size: f32, layout: Layout) -> Result<()> {
    let minimum_z = if layout == Layout::Room222 { -1. } else { 0. };
    if ![position.x, position.y, position.z, size]
        .iter()
        .all(|x| x.is_finite())
        || !(-1. ..=1.).contains(&position.x)
        || !(-1. ..=1.).contains(&position.y)
        || !(minimum_z..=1.).contains(&position.z)
        || !(0. ..=1.).contains(&size)
    {
        return Err(Error::Unsupported(
            "D mode requires finite Cartesian position and equal size in range",
        ));
    }
    Ok(())
}

fn quantize_axis(v: f32, steps: u32) -> f32 {
    let n = libm::floor(f64::from(v) * f64::from(steps) + 0.5) as u32;
    ((n * 32768 / steps).min(32767)) as f32 / 32768.
}

fn canonical(p: Position, size: f32) -> ([f32; 3], f32) {
    let mut s = f64::from(size);
    let nearest = libm::floor(s * 31. + 0.5) / 31.;
    if (s - nearest).abs() > 0.01 {
        s = nearest;
    }
    let s = (libm::floor(s * 32768. + 0.5) as u32).min(32767) as f32 / 32768.;
    (
        [
            quantize_axis(p.x, 310),
            quantize_axis(p.y, 310),
            quantize_axis(p.z, 75),
        ],
        s,
    )
}

fn spatial(p: Position, size: f32) -> Spatial {
    let ([x, y, z], size) = canonical(p, size);
    let point_axes = [basis(0, x), basis(1, y), basis(2, z)];
    let point = normalize(std::array::from_fn(|i| {
        (point_axes[0][i] * point_axes[1][i]) * point_axes[2][i]
    }));
    let radius = panner::radius(size);
    if size == 0. || radius < 2.5e-5 {
        return point;
    }
    let power = panner::exponent(radius);
    let axes = [
        extent(0, x, radius),
        extent(1, y, radius),
        extent(2, z, radius),
    ];
    let volume: Spatial = normalize(std::array::from_fn(|i| {
        (axes[0][i] * axes[1][i]) * axes[2][i]
    }));
    let walls = [
        panner::kernel::<true>(0., x, radius, 0),
        panner::kernel::<true>(1., x, radius, 0),
        panner::kernel::<true>(0., y, radius, 1),
        panner::kernel::<true>(1., y, radius, 1),
        panner::kernel::<true>(1., z, radius, 0) * 0.203_455_92,
    ];
    let faces = [
        basis(0, 0.),
        basis(0, 1.),
        basis(1, 0.),
        basis(1, 1.),
        basis(2, 1.),
    ];
    let boundary: Spatial = normalize(std::array::from_fn(|i| {
        let w: [f32; 5] = std::array::from_fn(|j| panner::power(walls[j] * faces[j][i], power));
        let [a, b, c] = [axes[0][i], axes[1][i], axes[2][i]];
        (((w[4] * (a * b) + (b * c) * w[0]) + (b * c) * w[1]) + (a * c) * w[2]) + (a * c) * w[3]
    }));
    let distance = x.min(1. - x).min(y).min(1. - y).min(1. - z);
    let weight = if distance * 0.25 > radius && distance * 0.25 > 0.05 {
        1.
    } else {
        distance / (radius * 4.) * ((distance * 5.) * (distance * 5.))
    };
    let spread = normalize(std::array::from_fn(|i| {
        panner::power(
            ((boundary[i] + weight * volume[i]) * 0.5) * 0.0625,
            1. / power,
        )
    }));
    let amount = ((radius * 4.) * 0.625) * 8.;
    if radius > 0.05 || amount >= ROOM_MAX {
        return spread;
    }
    let angle = (amount * std::f32::consts::FRAC_PI_8) * 4.;
    let direct = libm::cosf(angle) * 0.5;
    let size = libm::sinf(angle) * 0.5;
    normalize(std::array::from_fn(|i| {
        direct * point[i] + size * spread[i]
    }))
}

fn internal_gains(p: Position, size: f32) -> [f32; CHANNELS] {
    let spatial = spatial(p, size);
    std::array::from_fn(|i| {
        if i == 3 {
            0.
        } else {
            spatial[if i < 3 { i } else { i - 1 }]
        }
    })
}

/// Static targets for diagnostics and independent numerical fixtures.
pub fn gains(position: Position, size: f32) -> Result<[f32; CHANNELS]> {
    validate(position, size, Layout::Nine)?;
    Ok(internal_gains(position.internal(), size))
}

fn internal_position(position: Position, layout: Layout) -> Position {
    let mut result = position.internal();
    if layout == Layout::Room222 {
        result.z = (position.z + 1.) * 0.5;
    }
    result
}

fn layout_gains(position: Position, size: f32, layout: Layout) -> Gains {
    if layout == Layout::Room222 {
        room_222::gains(position, size)
    } else {
        let mut result = [0.; 24];
        result[..CHANNELS].copy_from_slice(&internal_gains(position, size));
        result
    }
}

/// Static targets in native channel order; LFE slots remain zero for Objects.
pub fn gains_for_layout(position: Position, size: f32, layout: Layout) -> Result<Gains> {
    if layout == Layout::Seven {
        return Err(Error::Unsupported("D mode supports 9.1.6 and 22.2"));
    }
    validate(position, size, layout)?;
    Ok(layout_gains(
        internal_position(position, layout),
        size,
        layout,
    ))
}

struct Lane {
    input: usize,
    gain: f32,
    events: Box<[Event]>, // Internal room coordinates; empty for a fixed bed route.
    bed: Gains,
}
pub struct Plan {
    inputs: usize,
    layout: Layout,
    total: u64,
    lanes: Vec<Lane>,
}
impl Plan {
    pub fn new(
        inputs: usize,
        layout: Layout,
        total: u64,
        rows: &[TbRowInput],
        events: &[Event],
        bed: &[f32],
    ) -> Result<Self> {
        if inputs == 0 || total == 0 {
            return Err(invalid());
        }
        if layout == Layout::Seven {
            return Err(Error::Unsupported("D mode supports 9.1.6 and 22.2"));
        }
        let channels = layout.channels();
        count(inputs, channels)?;
        let mut seen = vec![false; inputs];
        let mut lanes = Vec::with_capacity(rows.len());
        for r in rows {
            if r.input >= inputs
                || seen[r.input]
                || !r.gain.is_finite()
                || r.kind > 2
                || r.event_offset > events.len()
                || r.event_count > events.len() - r.event_offset
            {
                return Err(invalid());
            }
            seen[r.input] = true;
            let mut lane = Lane {
                input: r.input,
                gain: r.gain,
                events: Box::new([]),
                bed: [0.; 24],
            };
            if r.kind == 0 {
                if r.bed_offset > bed.len() || channels > bed.len() - r.bed_offset {
                    return Err(invalid());
                }
                lane.bed[..channels].copy_from_slice(&bed[r.bed_offset..r.bed_offset + channels]);
                if !lane.bed.iter().all(|v| v.is_finite()) {
                    return Err(invalid());
                }
            } else {
                let source = &events[r.event_offset..r.event_offset + r.event_count];
                if source.first().is_none_or(|e| e.start != 0) {
                    return Err(invalid());
                }
                let mut converted = Vec::with_capacity(source.len());
                for (i, e) in source.iter().enumerate() {
                    let size = if r.kind == 1 { 0. } else { e.size };
                    validate(e.position, size, layout)?;
                    if e.start >= total || (i > 0 && e.start < source[i - 1].start) {
                        return Err(invalid());
                    }
                    converted.push(Event {
                        position: internal_position(e.position, layout),
                        size,
                        ..*e
                    });
                }
                lane.events = converted.into_boxed_slice();
            }
            lanes.push(lane);
        }
        lanes.sort_by_key(|lane| lane.input);
        if layout == Layout::Room222 {
            room_222::prepare();
        } else {
            let _ = lattice();
        }
        Ok(Self {
            inputs,
            layout,
            total,
            lanes,
        })
    }
}

struct State {
    next: usize,
    position: Position,
    target: Position,
    size: f32,
    previous: Gains,
    gains: Gains,
}
pub struct Session {
    plan: Arc<Plan>,
    states: Vec<State>,
    frame: u64,
    active: Option<u64>,
}
impl Session {
    pub fn new(plan: Arc<Plan>) -> Self {
        let states = plan
            .lanes
            .iter()
            .map(|lane| {
                let next = lane.events.partition_point(|e| e.start < METADATA);
                let event = lane.events.get(next.saturating_sub(1));
                let position = event.map_or_else(Position::default, |e| e.position);
                State {
                    next,
                    position,
                    target: position,
                    size: event.map_or(0., |e| e.size),
                    previous: [0.; 24],
                    gains: [0.; 24],
                }
            })
            .collect();
        Self {
            plan,
            states,
            frame: 0,
            active: None,
        }
    }

    fn advance(&mut self, block: u64) {
        let alpha = 1. - libm::expf(-512. / 1200.);
        for (lane, state) in self.plan.lanes.iter().zip(&mut self.states) {
            state.previous = state.gains;
            if lane.events.is_empty() {
                state.gains = lane.bed;
                continue;
            }
            for step in 1..=3 {
                let end = block.saturating_add(step * METADATA);
                while let Some(e) = lane.events.get(state.next).filter(|e| e.start < end) {
                    state.target = e.position;
                    state.size = e.size;
                    state.next += 1;
                }
                state.position.x += alpha * (state.target.x - state.position.x);
                state.position.y += alpha * (state.target.y - state.position.y);
                state.position.z += alpha * (state.target.z - state.position.z);
            }
            state.gains = layout_gains(state.position, state.size, self.plan.layout);
        }
        self.active = Some(block);
    }

    /// Adds the complete D-mode mix; valid calls are sequential and may use any block size.
    /// Validate the whole request before mutating state or output.
    pub fn process(
        &mut self,
        source: &[f32],
        output: &mut [f32],
        start: u64,
        frames: usize,
    ) -> Result<()> {
        let channels = self.plan.layout.channels();
        if start != self.frame
            || start > self.plan.total
            || frames as u64 > self.plan.total - start
            || source.len() != count(frames, self.plan.inputs)?
            || output.len() < count(frames, channels)?
            || source.iter().any(|v| !v.is_finite())
        {
            return Err(invalid());
        }
        let mut offset = 0;
        while offset < frames {
            let at = start + offset as u64;
            let block = at / CONTROL * CONTROL;
            if self.active != Some(block) {
                self.advance(block);
            }
            let first = at - block;
            let n = (frames - offset).min((CONTROL - first) as usize);
            for (lane, state) in self.plan.lanes.iter().zip(&self.states) {
                for f in 0..n {
                    let t = (first + f as u64) as f32 / CONTROL as f32;
                    let sample = source[(offset + f) * self.plan.inputs + lane.input] * lane.gain;
                    for c in 0..channels {
                        let gain = state.previous[c] + t * (state.gains[c] - state.previous[c]);
                        output[(offset + f) * channels + c] += sample * gain;
                    }
                }
            }
            offset += n;
        }
        self.frame += frames as u64;
        Ok(())
    }
}
