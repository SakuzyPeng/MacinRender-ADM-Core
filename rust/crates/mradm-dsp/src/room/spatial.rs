//! ADM spatial adapters. File cloud and Scene extent retain their established
//! sampling conventions; both feed the same weighted room paths. No speaker bus.
use super::{
    control::State,
    geometry::{self, WeightedPosition},
    resources::RoomProfile,
};
use crate::{
    Result,
    geometry::{Vec3, cross, norm, unit},
    scene_math::DISK,
};

pub const MAX_SAMPLES: usize = 51;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Shape {
    pub position: [f32; 3],
    pub extent: [f32; 3],
    pub divergence: f32,
    pub ranges: [f32; 2],
    pub channel_lock: bool,
    pub maximum_lock: Option<f32>,
    mapped: Option<(Vec3, Vec3)>,
}

impl Shape {
    pub fn from_state(state: State, fallback: [f32; 3], role: u32, spread: bool) -> Self {
        let object = role == 0;
        Self {
            mapped: None,
            position: if state.valid & 4 != 0 {
                state.position
            } else {
                fallback
            },
            extent: if object && spread && state.valid & 8 != 0 {
                state.extent
            } else {
                [0.; 3]
            },
            divergence: if object && state.valid & 32 != 0 {
                state.divergence.clamp(0., 1.)
            } else {
                0.
            },
            ranges: if object && state.valid & 512 != 0 {
                state.divergence_range
            } else {
                [45., 0.]
            },
            channel_lock: state.valid & 64 != 0 && state.channel_lock != 0,
            maximum_lock: if state.valid & 1024 != 0 && state.has_max_distance != 0 {
                Some(state.max_distance)
            } else {
                None
            },
        }
    }
    /// Resolve automatic coordinates independently for each active room. Screen
    /// warping operates on physical directions and leaves the radius unchanged.
    pub(crate) fn map_room(
        mut self,
        profile: &RoomProfile,
        previous: Vec3,
        screen: &super::screen::Mapping,
        screen_active: bool,
        previous_screen: bool,
    ) -> Self {
        let offset = geometry::room_offset(
            profile,
            [
                f64::from(self.position[1]),
                -f64::from(self.position[0]),
                f64::from(self.position[2]),
            ],
        );
        let previous = geometry::room_offset(profile, previous);
        let warp = |v: Vec3| {
            let v = screen.physical_position([-v[1], v[0], v[2]]);
            [v[1], -v[0], v[2]]
        };
        self.mapped = Some((
            if screen_active { warp(offset) } else { offset },
            geometry::unit_or_front(if previous_screen {
                warp(previous)
            } else {
                previous
            }),
        ));
        self
    }
    pub(crate) fn primary(
        &self,
        profile: &RoomProfile,
        metres: f64,
        previous: Vec3,
    ) -> Result<geometry::Position> {
        match self.mapped {
            Some((offset, previous)) => geometry::physical_position(profile, offset, previous),
            None => geometry::position(profile, self.position, metres, previous),
        }
    }
    /// `scene_flavor` retains Scene's eight-direction expansion. File rendering
    /// uses the existing 17-point angular cloud (up to three divergence lobes).
    pub fn samples(
        &self,
        profile: &RoomProfile,
        metres: f64,
        previous: Vec3,
        scene_flavor: bool,
        output: &mut [WeightedPosition; MAX_SAMPLES],
    ) -> Result<usize> {
        let primary = self.primary(profile, metres, previous)?;
        if self.extent == [0.; 3] && self.divergence <= 1e-4 && !self.channel_lock {
            output[0] = WeightedPosition {
                position: primary,
                weight: 1.,
            };
            return Ok(1);
        }
        let mut center = primary.direction;
        if self.channel_lock {
            let anchors = [
                crate::geometry::portable_direction(30., 0.),
                crate::geometry::portable_direction(-30., 0.),
            ];
            let distance = |v: Vec3| norm(std::array::from_fn(|i| center[i] - v[i]));
            let a = distance(anchors[0]);
            let b = distance(anchors[1]);
            let (candidate, distance) = if a <= b {
                (anchors[0], a)
            } else {
                (anchors[1], b)
            };
            if self
                .maximum_lock
                .is_none_or(|limit| distance <= f64::from(limit) + 1e-4)
            {
                center = candidate;
            }
        }
        let az = libm::atan2(center[1], center[0]).to_degrees() as f32;
        let el = libm::atan2(
            center[2],
            (center[0] * center[0] + center[1] * center[1]).sqrt(),
        )
        .to_degrees() as f32;
        let authored_radius = norm(self.position.map(f64::from));
        let angle = if self.ranges[1] > 0. {
            libm::atan2(f64::from(self.ranges[1]), authored_radius.max(1e-6)).to_degrees() as f32
        } else {
            self.ranges[0]
        }
        .clamp(0., 120.);
        let side = self.divergence / (self.divergence + 1.);
        let central = (1. - self.divergence) / (self.divergence + 1.);
        let mut directions = [[0f32; 3]; 8];
        directions[0] = [az, el, 1.];
        let mut count = 1;
        if self.divergence > 1e-4 {
            directions[..3].copy_from_slice(&[
                [az - angle, el, side],
                [az, el, central],
                [az + angle, el, side],
            ]);
            count = 3;
        }
        let to_position = |direction: Vec3| {
            if metres == 0. {
                let offset = self.mapped.map_or_else(
                    || {
                        geometry::room_offset(
                            profile,
                            [
                                f64::from(self.position[1]),
                                -f64::from(self.position[0]),
                                f64::from(self.position[2]),
                            ],
                        )
                    },
                    |v| v.0,
                );
                let radius = norm(offset)
                    .min(norm(profile.dimensions))
                    .max(profile.minimum_distance);
                return geometry::physical_position(
                    profile,
                    direction.map(|v| v * radius),
                    primary.direction,
                );
            }
            // Bound the physical radius before narrowing back to the Scene ABI.
            // Extent samples preserve the parent's requested radius until each
            // ray is independently constrained by the room boundary.
            let radius = authored_radius
                .min(norm(profile.dimensions) / metres)
                .max(profile.minimum_distance / metres);
            geometry::position(
                profile,
                [
                    (-direction[1] * radius) as f32,
                    (direction[0] * radius) as f32,
                    (direction[2] * radius) as f32,
                ],
                metres,
                previous,
            )
        };
        if scene_flavor {
            let width = self.extent[0].clamp(0., 1.) * 60.;
            let height = self.extent[1].clamp(0., 1.) * 45.;
            let depth = self.extent[2].clamp(0., 1.) * 20.;
            if width > 0.01 || height > 0.01 || depth > 0.01 {
                directions[count..count + 4].copy_from_slice(&[
                    [az - width, el, 0.5],
                    [az + width, el, 0.5],
                    [az, (el - height).clamp(-90., 90.), 0.5],
                    [az, (el + height).clamp(-90., 90.), 0.5],
                ]);
                count += 4;
                if depth > 0.01 {
                    directions[count] = [az + 180., el, depth / 20.];
                    count += 1;
                }
            }
            let sum = directions[..count].iter().map(|v| v[2]).sum::<f32>();
            for (out, point) in output.iter_mut().zip(&directions[..count]) {
                *out = WeightedPosition {
                    position: to_position(crate::geometry::portable_direction(point[0], point[1]))?,
                    weight: point[2] / sum,
                };
            }
            return Ok(count);
        }
        let [width, height] = crate::scene_math::radii(
            self.extent[0],
            self.extent[1],
            self.extent[2],
            authored_radius.min(f64::from(f32::MAX)) as f32,
            false,
        );
        let mut written = 0;
        for lobe in &directions[..count] {
            if lobe[2] <= 0. {
                continue;
            }
            let direction = crate::geometry::portable_direction(lobe[0], lobe[1]);
            if width <= 1e-4 && height <= 1e-4 {
                output[written] = WeightedPosition {
                    position: to_position(direction)?,
                    weight: lobe[2],
                };
                written += 1;
                continue;
            }
            let horizontal = cross([0., 0., 1.], direction);
            let horizontal = if norm(horizontal) < 1e-4 {
                [0., -1., 0.]
            } else {
                unit(horizontal)
            };
            let vertical = unit(cross(direction, horizontal));
            for point in DISK {
                if point[2] <= 0. {
                    continue;
                }
                let x = libm::tan(f64::from(point[0] * width * crate::scene_math::RAD));
                let y = libm::tan(f64::from(point[1] * height * crate::scene_math::RAD));
                let v = unit(std::array::from_fn(|i| {
                    direction[i] + horizontal[i] * x + vertical[i] * y
                }));
                output[written] = WeightedPosition {
                    position: to_position(v)?,
                    weight: lobe[2] * point[2],
                };
                written += 1;
            }
        }
        if written == 0 {
            output[0] = WeightedPosition {
                position: primary,
                weight: 1.,
            };
            return Ok(1);
        }
        Ok(written)
    }
}
