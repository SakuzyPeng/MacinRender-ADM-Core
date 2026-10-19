//! Sample-clock control anchors. Evaluating a ramp does not consume it, so
//! packetization and file preroll cannot change its floating-point endpoints.
pub use crate::live_binaural::State;
use crate::{Error, Result};
pub const FIELDS: u64 = (1 << 11) - 1;
pub const GEOMETRY_FIELDS: u64 = 4 | 8 | 32 | 64 | 128 | 256 | 512 | 1024;

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct RoomDescription {
    pub id: u64,
    pub generation: u64,
    pub role: u32,
    pub reserved: u32,
    pub position: [f32; 3],
    pub padding: u32,
    pub screens: super::screen::RoomScreens,
}
impl RoomDescription {
    pub fn same_source(&self, other: &Self) -> bool {
        self.id == other.id && self.generation == other.generation
    }
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct RoomCommand {
    pub element: u32,
    pub offset: u32,
    pub duration: u32,
    pub reserved: u32,
    pub changed: u64,
    pub cleared: u64,
    pub state: State,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct RoomOverride {
    pub gain_db: f32,
    pub mute: u32,
    pub extent: [f32; 3],
    pub diffuse: f32,
    pub divergence: f32,
    pub has_head: u32,
    pub head: u32,
    pub reserved: u32,
}
pub fn neutral_override() -> State {
    State {
        valid: 2 | 8 | 16 | 32,
        extent: [1.; 3],
        diffuse: 1.,
        divergence: 1.,
        ..State::default()
    }
}
impl RoomOverride {
    pub fn state(&self) -> Result<State> {
        if !self.gain_db.is_finite()
            || self.mute > 1
            || self.has_head > 1
            || self.head > 1
            || self.reserved != 0
            || self
                .extent
                .iter()
                .chain([&self.diffuse, &self.divergence])
                .any(|v| !v.is_finite() || *v < 0.)
        {
            return Err(Error::InvalidArgument("Invalid room live override"));
        }
        let gain = if self.mute != 0 {
            0.
        } else {
            libm::pow(10., f64::from(self.gain_db) / 20.) as f32
        };
        if !gain.is_finite() {
            return Err(Error::InvalidArgument("Room live gain overflow"));
        }
        Ok(State {
            gain,
            extent: self.extent,
            diffuse: self.diffuse,
            divergence: self.divergence,
            head_locked: self.head,
            valid: 2 | 8 | 16 | 32 | if self.has_head != 0 { 256 } else { 0 },
            ..State::default()
        })
    }
}

pub fn validate_state(state: &State) -> Result<()> {
    if state.valid & !FIELDS != 0
        || [
            state.active,
            state.channel_lock,
            state.screen_reference,
            state.head_locked,
            state.has_max_distance,
        ]
        .iter()
        .any(|v| *v > 1)
        || [
            state.gain,
            state.diffuse,
            state.divergence,
            state.max_distance,
        ]
        .iter()
        .chain(&state.position)
        .chain(&state.extent)
        .chain(&state.divergence_range)
        .any(|v| !v.is_finite())
        || state.gain < 0.
        || state.max_distance < 0.
    {
        return Err(Error::InvalidArgument("Invalid room source control"));
    }
    Ok(())
}

pub fn copy_fields(destination: &mut State, source: State, fields: u64) {
    if fields & 1 != 0 {
        destination.active = source.active;
    }
    if fields & 2 != 0 {
        destination.gain = source.gain;
    }
    if fields & 4 != 0 {
        destination.position = source.position;
    }
    if fields & 8 != 0 {
        destination.extent = source.extent;
    }
    if fields & 16 != 0 {
        destination.diffuse = source.diffuse;
    }
    if fields & 32 != 0 {
        destination.divergence = source.divergence;
    }
    if fields & 64 != 0 {
        destination.channel_lock = source.channel_lock;
    }
    if fields & 128 != 0 {
        destination.screen_reference = source.screen_reference;
    }
    if fields & 256 != 0 {
        destination.head_locked = source.head_locked;
    }
    if fields & 512 != 0 {
        destination.divergence_range = source.divergence_range;
    }
    if fields & 1024 != 0 {
        destination.has_max_distance = source.has_max_distance;
        destination.max_distance = source.max_distance;
    }
    destination.valid = (destination.valid & !fields) | (source.valid & fields);
}

#[derive(Clone, Copy)]
pub struct Control {
    origin: State,
    target: State,
    start: [u64; 11],
    end: [u64; 11],
    settled: u64,
}

impl Control {
    pub fn new(state: State) -> Self {
        Self {
            origin: state,
            target: state,
            start: [0; 11],
            end: [0; 11],
            settled: 0,
        }
    }
    pub fn initialize(&mut self, state: State) {
        *self = Self::new(state);
    }
    pub fn apply(&mut self, command: &RoomCommand, sample: u64) -> Result<()> {
        validate_state(&command.state)?;
        if (command.changed | command.cleared) & !FIELDS != 0 || command.reserved != 0 {
            return Err(Error::InvalidArgument("Invalid room control field mask"));
        }
        let end = sample
            .checked_add(u64::from(command.duration))
            .ok_or(Error::InvalidArgument("Room control timeline overflow"))?;
        let current = self.at(sample);
        for field in 0..11 {
            let mask = 1 << field;
            if (command.changed | command.cleared) & mask != 0 {
                copy_fields(&mut self.origin, current, mask);
                copy_fields(&mut self.target, command.state, mask);
                self.target.valid &= !(command.cleared & mask);
                self.start[field] = sample;
                // Head lock is a reference-frame change; its audio transition is
                // independent of positional metadata interpolation deadlines.
                self.end[field] = if mask == 128 || mask == 256 {
                    sample
                } else {
                    end
                };
            }
        }
        self.settled = self.end.iter().copied().max().unwrap_or(0);
        Ok(())
    }
    /// The state remains constant from this sample until another command.
    #[inline]
    pub(super) fn is_settled(&self, sample: u64) -> bool {
        sample >= self.settled
    }
    #[cfg(feature = "room-gpu-experiment")]
    pub(super) fn only_position_ramping(&self, sample: u64) -> bool {
        sample < self.end[2]
            && self
                .end
                .iter()
                .enumerate()
                .all(|(field, end)| field == 2 || sample >= *end)
    }
    #[cfg(feature = "room-gpu-experiment")]
    pub(super) fn position_at(&self, sample: u64) -> ([f32; 3], u64) {
        if sample >= self.end[2] {
            return (self.target.position, self.target.valid & 4);
        }
        let alpha =
            sample.saturating_sub(self.start[2]) as f64 / (self.end[2] - self.start[2]) as f64;
        let position = std::array::from_fn(|i| {
            (f64::from(self.origin.position[i])
                + (f64::from(self.target.position[i]) - f64::from(self.origin.position[i])) * alpha)
                as f32
        });
        (position, self.origin.valid & 4)
    }
    /// Geometry interpolation at this absolute sample; reference-frame changes
    /// are instantaneous and are never held by the realtime reflection grid.
    #[inline]
    pub(super) fn geometry_ramping(&self, sample: u64) -> bool {
        sample < self.settled
            && (0..11).any(|field| (1 << field) & GEOMETRY_FIELDS != 0 && sample < self.end[field])
    }
    #[inline]
    pub fn at(&self, sample: u64) -> State {
        // Most PCM samples have no pending metadata ramps. This returns exactly
        // the state produced by the per-field loop after its final deadline.
        if sample >= self.settled {
            return self.target;
        }
        let mut output = self.target;
        for field in 0..11 {
            if sample >= self.end[field] {
                continue;
            }
            let mask = 1 << field;
            let alpha64 = sample.saturating_sub(self.start[field]) as f64
                / (self.end[field] - self.start[field]) as f64;
            let alpha = alpha64 as f32;
            let interpolate = |a: f32, b: f32| a + (b - a) * alpha;
            let a = self.origin;
            let b = self.target;
            let mut value = a;
            match mask {
                2 => value.gain = interpolate(a.gain, b.gain),
                4 => {
                    value.position = std::array::from_fn(|i| {
                        (f64::from(a.position[i])
                            + (f64::from(b.position[i]) - f64::from(a.position[i])) * alpha64)
                            as f32
                    })
                }
                8 => value.extent = std::array::from_fn(|i| interpolate(a.extent[i], b.extent[i])),
                16 => value.diffuse = interpolate(a.diffuse, b.diffuse),
                32 => value.divergence = interpolate(a.divergence, b.divergence),
                512 => {
                    value.divergence_range = std::array::from_fn(|i| {
                        interpolate(a.divergence_range[i], b.divergence_range[i])
                    })
                }
                1024 if a.has_max_distance != 0 && b.has_max_distance != 0 => {
                    value.max_distance = interpolate(a.max_distance, b.max_distance);
                }
                _ => {}
            }
            copy_fields(&mut output, value, mask);
        }
        output
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn settled_fast_path_preserves_independent_ramps_and_historical_queries() {
        let mut control = Control::new(State::default());
        let mut state = State {
            gain: 0.,
            ..State::default()
        };
        control
            .apply(
                &RoomCommand {
                    changed: 2,
                    duration: 100,
                    state,
                    ..RoomCommand::default()
                },
                10,
            )
            .unwrap();
        state.diffuse = 1.;
        control
            .apply(
                &RoomCommand {
                    changed: 16,
                    duration: 200,
                    state,
                    ..RoomCommand::default()
                },
                10,
            )
            .unwrap();
        assert_eq!(control.at(110).gain, 0.);
        assert_eq!(control.at(110).diffuse, 0.5);
        assert_eq!(control.at(210).diffuse, 1.);
        // Retarget one field immediately while another field still ramps.
        state.gain = 0.25;
        control
            .apply(
                &RoomCommand {
                    changed: 2,
                    state,
                    ..RoomCommand::default()
                },
                110,
            )
            .unwrap();
        assert_eq!(control.at(160).gain, 0.25);
        assert_eq!(control.at(160).diffuse, 0.75);
        let endpoint = control.at(210);
        assert_eq!(control.at(10000), endpoint);
        assert_eq!(control.at(160).diffuse, 0.75);
        // Initialization must discard the previous deadline, including on seek.
        control.initialize(State::default());
        assert_eq!(control.at(0), State::default());
    }

    #[test]
    fn independent_deadlines_and_preroll_are_identical() {
        let initial = State::default();
        let mut control = Control::new(initial);
        let mut state = initial;
        state.position = [-1., 0., 0.];
        let move_command = RoomCommand {
            changed: 4,
            duration: 10000,
            state,
            ..RoomCommand::default()
        };
        control.apply(&move_command, 100).unwrap();
        state.gain = 0.25;
        control
            .apply(
                &RoomCommand {
                    changed: 2,
                    duration: 100,
                    state,
                    ..RoomCommand::default()
                },
                500,
            )
            .unwrap();
        assert_eq!(control.at(600).gain, 0.25);
        assert!(control.at(600).position[1] > 0.9);
        let mut seek = Control::new(initial);
        seek.apply(&move_command, 100).unwrap();
        seek.apply(
            &RoomCommand {
                changed: 2,
                duration: 100,
                state,
                ..RoomCommand::default()
            },
            500,
        )
        .unwrap();
        for frame in 4000..4200 {
            assert_eq!(control.at(frame), seek.at(frame));
        }
        assert_eq!(control.at(10100).position, [-1., 0., 0.]);
    }
}

#[cfg(all(test, feature = "room-gpu-experiment"))]
mod gpu_tests {
    use super::*;
    #[test]
    fn position_projection_preserves_original_f64_ramps_validity_and_deadlines() {
        let mut c = Control::new(State::default());
        for (step, at) in [0, 17, 73, 127, 255].into_iter().enumerate() {
            c.apply(
                &RoomCommand {
                    changed: 4 | if step == 2 { 2 } else { 0 },
                    cleared: if step == 3 { 4 } else { 0 },
                    duration: [128, 1, 257, 96, 0][step],
                    state: State {
                        position: [step as f32 * 0.2 - 0.4, 0.7, -0.3],
                        gain: 0.5,
                        ..State::default()
                    },
                    ..RoomCommand::default()
                },
                at,
            )
            .unwrap();
            for sample in 0..640 {
                let expected = c.at(sample);
                let (position, valid) = c.position_at(sample);
                assert_eq!(
                    position.map(f32::to_bits),
                    expected.position.map(f32::to_bits)
                );
                assert_eq!(valid, expected.valid & 4);
                if c.only_position_ramping(sample) {
                    for later in sample..sample + 17 {
                        let value = c.at(later);
                        assert_eq!(value.gain.to_bits(), expected.gain.to_bits());
                        assert_eq!(value.valid & !4, expected.valid & !4);
                    }
                }
            }
        }
    }
}
