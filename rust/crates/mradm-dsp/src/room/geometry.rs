//! World-space geometry. Scalar f64 operations, fixed ordering, portable trig.
use super::resources::RoomProfile;
use crate::{
    Error, Result,
    geometry::{Vec3, dot, norm, scale, sub},
};

#[derive(Clone, Copy, Debug)]
pub struct Image {
    pub parity: [i32; 3],
    pub lattice: [i32; 3],
    pub walls: [u32; 6],
    pub order: usize,
}

pub fn images(maximum_order: usize) -> Vec<Image> {
    let mut result = Vec::new();
    let bound = maximum_order as i32 / 2 + 1;
    for x in -bound..=bound {
        for y in -bound..=bound {
            for z in -bound..=bound {
                for mask in 0..8 {
                    let lattice = [x, y, z];
                    let mut parity = [1; 3];
                    let mut walls = [0; 6];
                    for axis in 0..3 {
                        let n = lattice[axis];
                        let (lower, upper) = if mask & (1 << axis) == 0 {
                            (n.unsigned_abs(), n.unsigned_abs())
                        } else {
                            parity[axis] = -1;
                            if n <= 0 {
                                ((1 - n) as u32, (-n) as u32)
                            } else {
                                ((n - 1) as u32, n as u32)
                            }
                        };
                        walls[axis * 2] = lower;
                        walls[axis * 2 + 1] = upper;
                    }
                    let order = walls.iter().sum::<u32>() as usize;
                    if order <= maximum_order {
                        result.push(Image {
                            parity,
                            lattice,
                            walls,
                            order,
                        });
                    }
                }
            }
        }
    }
    result.sort_by_key(|p| (p.order, p.lattice, p.parity));
    result
}

#[derive(Clone, Copy, Debug)]
pub struct Position {
    pub point: Vec3,
    pub direction: Vec3,
    pub distance: f64,
    pub constrained: bool,
}

#[derive(Clone, Copy, Debug)]
pub struct WeightedPosition {
    pub position: Position,
    pub weight: f32,
}

impl Default for WeightedPosition {
    fn default() -> Self {
        Self {
            position: Position {
                point: [0.; 3],
                direction: [1., 0., 0.],
                distance: 1.,
                constrained: false,
            },
            weight: 0.,
        }
    }
}

pub fn position(
    room: &RoomProfile,
    scene: [f32; 3],
    metres: f64,
    previous: Vec3,
) -> Result<Position> {
    if !valid_scale(metres) || scene.iter().any(|v| !v.is_finite()) {
        return Err(Error::InvalidArgument(
            "Invalid room coordinates or distance scale",
        ));
    }
    let relative = [
        f64::from(scene[1]),
        -f64::from(scene[0]),
        f64::from(scene[2]),
    ];
    if metres == 0. {
        let previous = unit_or_front(room_offset(room, previous));
        return physical_position(room, room_offset(room, relative), previous);
    }
    let length = norm(relative);
    let direction = if length > 1e-15 {
        scale(relative, 1. / length)
    } else {
        previous
    };
    let requested = length * metres;
    constrain(room, direction, requested)
}

/// Zero selects listener-centred room coordinates; positive values retain the
/// explicit uniform scale. Keep the existing positive-scale arithmetic intact.
pub fn valid_scale(metres: f64) -> bool {
    metres == 0. || (metres.is_finite() && (0.01..=100.).contains(&metres))
}

pub(crate) fn unit_or_front(value: Vec3) -> Vec3 {
    let length = norm(value);
    if length > 1e-15 {
        scale(value, 1. / length)
    } else {
        [1., 0., 0.]
    }
}

/// World-axis normalized offsets, with each signed unit at its usable wall.
pub(crate) fn room_offset(room: &RoomProfile, offset: Vec3) -> Vec3 {
    std::array::from_fn(|axis| {
        offset[axis]
            * if offset[axis] >= 0. {
                room.dimensions[axis] - room.clearance - room.listener[axis]
            } else {
                room.listener[axis] - room.clearance
            }
    })
}

pub(crate) fn physical_position(
    room: &RoomProfile,
    offset: Vec3,
    previous: Vec3,
) -> Result<Position> {
    let requested = norm(offset);
    let direction = if requested > 1e-15 {
        scale(offset, 1. / requested)
    } else {
        previous
    };
    constrain(room, direction, requested)
}

fn constrain(room: &RoomProfile, direction: Vec3, requested: f64) -> Result<Position> {
    let mut maximum = f64::INFINITY;
    for (axis, component) in direction.into_iter().enumerate() {
        if component > 1e-15 {
            maximum = maximum
                .min((room.dimensions[axis] - room.clearance - room.listener[axis]) / component);
        } else if component < -1e-15 {
            maximum = maximum.min((room.clearance - room.listener[axis]) / component);
        }
    }
    let distance = requested.max(room.minimum_distance).min(maximum);
    Ok(Position {
        point: std::array::from_fn(|i| room.listener[i] + direction[i] * distance),
        direction,
        distance,
        constrained: distance != requested,
    })
}

#[derive(Clone, Copy)]
pub struct Path {
    pub direction: Vec3,
    /// Complete image-source path length in metres, before time conversion.
    pub distance: f64,
    pub samples: f64,
    pub gain: f64,
}

pub fn path(room: &RoomProfile, source: Position, image: Image) -> Path {
    let reflected = std::array::from_fn(|i| {
        f64::from(image.parity[i]) * source.point[i]
            + 2. * f64::from(image.lattice[i]) * room.dimensions[i]
    });
    let relative = sub(reflected, room.listener);
    let distance = norm(relative);
    let direction = scale(relative, 1. / distance);
    let departure = std::array::from_fn(|i| -f64::from(image.parity[i]) * direction[i]);
    let beam = dot(scale(source.direction, -1.), departure).max(0.);
    let relative_time = ((distance - source.distance) / room.speed).max(0.);
    let fade = if relative_time >= room.early_fade[1] {
        0.
    } else if relative_time <= room.early_fade[0] {
        1.
    } else {
        0.5 * (1.
            + libm::cos(
                std::f64::consts::PI * (relative_time - room.early_fade[0])
                    / (room.early_fade[1] - room.early_fade[0]),
            ))
    };
    Path {
        direction,
        distance,
        samples: distance * f64::from(room.sample_rate) / room.speed,
        gain: beam * beam * fade * room.reference_distance / distance,
    }
}

/// Inverse listener rotation in the room +X-front/+Y-left/+Z-up basis.
/// Public Euler convention: yaw left, pitch up, roll right.
#[derive(Clone, Copy)]
pub struct Rotation {
    matrix: [Vec3; 3],
}

impl Rotation {
    pub fn new(degrees: [f32; 3]) -> Result<Self> {
        if degrees.iter().any(|v| !v.is_finite()) {
            return Err(Error::InvalidArgument("Nonfinite room head orientation"));
        }
        let [yaw, pitch, roll] = degrees.map(|v| f64::from(v).to_radians());
        let (sy, cy) = libm::sincos(yaw);
        let (sp, cp) = libm::sincos(pitch);
        let (sr, cr) = libm::sincos(roll);
        // Forward Rz(yaw) Ry(-pitch) Rx(roll), transposed for world -> head.
        let matrix = [
            [cy * cp, sy * cp, sp],
            [-cy * sp * sr - sy * cr, -sy * sp * sr + cy * cr, cp * sr],
            [-cy * sp * cr + sy * sr, -sy * sp * cr - cy * sr, cp * cr],
        ];
        Ok(Self { matrix })
    }
    pub fn apply(&self, direction: Vec3) -> Vec3 {
        self.matrix.map(|row| dot(row, direction))
    }
    /// Cosine of the relative 3D rotation angle, including yaw wrap and poles.
    pub fn difference_cosine(&self, other: &Self) -> f64 {
        let trace = dot(self.matrix[0], other.matrix[0])
            + dot(self.matrix[1], other.matrix[1])
            + dot(self.matrix[2], other.matrix[2]);
        ((trace - 1.) * 0.5).clamp(-1., 1.)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn relative_rotation_uses_orientation_not_euler_distance() {
        let a = Rotation::new([179.9, 0., 0.]).unwrap();
        let b = Rotation::new([-179.9, 0., 0.]).unwrap();
        assert!(a.difference_cosine(&b) > libm::cos(0.21f64.to_radians()));
        assert!(a.difference_cosine(&b) < libm::cos(0.19f64.to_radians()));
        let a = Rotation::new([17., 90., 31.]).unwrap();
        let b = Rotation::new([17., 90., 31.]).unwrap();
        assert!((a.difference_cosine(&b) - 1.).abs() < 1e-14);
        let b = Rotation::new([17.3, 90., 31.]).unwrap();
        assert!(a.difference_cosine(&b) > libm::cos(0.31f64.to_radians()));
        assert!(a.difference_cosine(&b) < libm::cos(0.29f64.to_radians()));
        let a = Rotation::new([0.; 3]).unwrap();
        let b = Rotation::new([0.3, 0.3, 0.3]).unwrap();
        assert!(a.difference_cosine(&b) < libm::cos(0.5f64.to_radians()));
    }
    #[test]
    fn shoebox_enumeration_counts_and_reflection_parity() {
        assert_eq!(images(1).len(), 7);
        assert_eq!(images(4).len(), 129);
        assert_eq!(images(6).len(), 377);
        assert_eq!(images(4)[0].order, 0);
        for p in images(4) {
            for axis in 0..3 {
                assert_eq!(
                    p.parity[axis],
                    if (p.walls[axis * 2] + p.walls[axis * 2 + 1]) % 2 == 0 {
                        1
                    } else {
                        -1
                    }
                );
            }
        }
    }
    #[test]
    fn rotations_follow_public_euler_and_preserve_length() {
        let left = Rotation::new([90., 0., 0.]).unwrap().apply([0., 1., 0.]);
        let up = Rotation::new([0., 90., 0.]).unwrap().apply([0., 0., 1.]);
        for v in [left, up] {
            assert!((v[0] - 1.).abs() < 1e-12);
            assert!(v[1].abs() < 1e-12 && v[2].abs() < 1e-12);
        }
        let tilted = Rotation::new([37., -29., 63.])
            .unwrap()
            .apply([0.2, -0.4, 0.7]);
        assert!((norm(tilted) - norm([0.2, -0.4, 0.7])).abs() < 1e-12);
        let roll = Rotation::new([0., 0., 90.]).unwrap().apply([0., 0., 1.]);
        assert!(roll[0].abs() < 1e-12 && (roll[1] - 1.).abs() < 1e-12 && roll[2].abs() < 1e-12);
    }
}
