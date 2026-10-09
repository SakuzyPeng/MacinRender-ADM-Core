//! Three-layer D-mode extension, evaluated directly on the 22 full-band nodes.
//! The two LFE slots are excluded; bed routing is supplied by the caller.
use super::{
    Gains, Position, RADIUS_STEP, canonical, normalize, pair, panner, quantize_axis, repeated,
};
use std::sync::OnceLock;

const SPATIAL: usize = 22;
type Spatial = [f32; SPATIAL];
// Node indices refer to panner::NODES; rows and layers run front-to-back, bottom-to-top.
const ROWS: [&[usize]; 8] = [
    &[0, 1, 2],
    &[10, 11, 12],
    &[8, 9],
    &[6, 7],
    &[3, 4, 5],
    &[19, 20, 21],
    &[16, 17, 18],
    &[13, 14, 15],
];
const LAYERS: [&[usize]; 3] = [&[0], &[1, 2, 3, 4], &[5, 6, 7]];
static NODES: OnceLock<[[f32; 3]; SPATIAL]> = OnceLock::new();
static LATTICE: OnceLock<Box<[Spatial]>> = OnceLock::new();

fn nodes() -> &'static [[f32; 3]; SPATIAL] {
    NODES.get_or_init(|| {
        std::array::from_fn(|i| {
            let p = panner::NODES[i].position;
            [
                quantize_axis((p.x + 1.) * 0.5, 310),
                quantize_axis((1. - p.y) * 0.5, 310),
                quantize_axis((p.z + 1.) * 0.5, 150),
            ]
        })
    })
}

fn weights(value: f32, knots: &[f32]) -> [f32; 4] {
    // cos(pi/2) can round slightly below zero at an exact row/layer endpoint.
    pair(value, knots).map(|gain| gain.max(0.))
}

fn basis(axis: usize, value: f32) -> Spatial {
    let mut result = [0.; SPATIAL];
    let nodes = nodes();
    match axis {
        0 => {
            for row in ROWS {
                let mut knots = [0.; 3];
                for (i, c) in row.iter().enumerate() {
                    knots[i] = nodes[*c][0];
                }
                let weights = weights(value, &knots[..row.len()]);
                for (i, c) in row.iter().enumerate() {
                    result[*c] = weights[i];
                }
            }
        }
        1 => {
            for layer in LAYERS {
                let mut knots = [0.; 4];
                for (i, r) in layer.iter().enumerate() {
                    knots[i] = nodes[ROWS[*r][0]][1];
                }
                let weights = weights(value, &knots[..layer.len()]);
                for (i, r) in layer.iter().enumerate() {
                    for c in ROWS[*r] {
                        result[*c] = weights[i];
                    }
                }
            }
        }
        _ => {
            let knots = [nodes[0][2], nodes[3][2], nodes[13][2]];
            let weights = weights(value, &knots);
            result[..3].fill(weights[0]);
            result[3..13].fill(weights[1]);
            result[13..].fill(weights[2]);
        }
    }
    result
}

fn quadrature(axis: usize, p: usize, r: usize) -> Spatial {
    let count = if axis == 2 { 15 } else { 20 };
    let center = repeated(if axis == 2 { 1. / 6. } else { 1. / 34. }, p);
    let radius = repeated(RADIUS_STEP, r);
    let exponent = panner::exponent(radius);
    let mut result = [0.; SPATIAL];
    for sample in 0..count {
        let position = sample as f32 / (count - 1) as f32;
        let mut weight = panner::kernel::<true>(position, center, radius, 0);
        if axis == 2 {
            // Reflect the height weighting about the middle layer for the added floor.
            weight *= libm::cosf(((position * 2. - 1.).abs() * 0.341_477_48) * 4.);
        }
        let gains = basis(axis, position);
        for i in 0..SPATIAL {
            result[i] += panner::power(gains[i] * weight, exponent);
        }
    }
    result
}

fn lattice() -> &'static [Spatial] {
    LATTICE.get_or_init(|| {
        let mut values = Vec::with_capacity(1540);
        for axis in 0..3 {
            for p in 0..if axis == 2 { 7 } else { 35 } {
                for r in 0..20 {
                    values.push(quadrature(axis, p, r));
                }
            }
        }
        values.into_boxed_slice()
    })
}

pub(super) fn prepare() {
    let _ = lattice();
}

fn extent(axis: usize, p: f32, r: f32) -> Spatial {
    let steps = if axis == 2 { 6 } else { 34 };
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
    std::array::from_fn(|i| {
        let mut value = 0.;
        for k in 0..4 {
            value += table[axis * 700 + offsets[k]][i] * (weights[k] / sum);
        }
        value
    })
}

pub(super) fn gains(p: Position, size: f32) -> Gains {
    let ([x, y, _], size) = canonical(p, size);
    // 75 position intervals per half-room, with z=0 exactly on the middle layer.
    let z = quantize_axis(p.z, 150);
    let axes = [basis(0, x), basis(1, y), basis(2, z)];
    let point: Spatial = normalize(std::array::from_fn(|i| {
        (axes[0][i] * axes[1][i]) * axes[2][i]
    }));
    let radius = panner::radius(size);
    let spatial = if size == 0. || radius < 2.5e-5 {
        point
    } else {
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
            panner::kernel::<true>(0., y, radius, 0),
            panner::kernel::<true>(1., y, radius, 0),
            panner::kernel::<true>(1., z, radius, 0) * 0.203_455_92,
            panner::kernel::<true>(0., z, radius, 0) * 0.203_455_92,
        ];
        let faces = [
            basis(0, 0.),
            basis(0, 1.),
            basis(1, 0.),
            basis(1, 1.),
            basis(2, 1.),
            basis(2, 0.),
        ];
        let boundary: Spatial = normalize(std::array::from_fn(|i| {
            let w: [f32; 6] = std::array::from_fn(|j| panner::power(walls[j] * faces[j][i], power));
            let [a, b, c] = [axes[0][i], axes[1][i], axes[2][i]];
            ((((w[4] * (a * b) + (b * c) * w[0]) + (b * c) * w[1]) + (a * c) * w[2])
                + (a * c) * w[3])
                + w[5] * (a * b)
        }));
        let distance = x.min(1. - x).min(y).min(1. - y).min(z).min(1. - z);
        let weight = if distance * 0.25 > radius && distance * 0.25 > 0.05 {
            1.
        } else {
            distance / (radius * 4.) * ((distance * 5.) * (distance * 5.))
        };
        let spread: Spatial = normalize(std::array::from_fn(|i| {
            panner::power(
                ((boundary[i] + weight * volume[i]) * 0.5) * 0.0625,
                1. / power,
            )
        }));
        let amount = ((radius * 4.) * 0.625) * 8.;
        if radius > 0.05 || amount >= super::ROOM_MAX {
            spread
        } else {
            let angle = (amount * std::f32::consts::FRAC_PI_8) * 4.;
            let direct = libm::cosf(angle) * 0.5;
            let size = libm::sinf(angle) * 0.5;
            normalize(std::array::from_fn(|i| {
                direct * point[i] + size * spread[i]
            }))
        }
    };
    let mut result = [0.; 24];
    for (node, gain) in panner::NODES.iter().zip(spatial) {
        result[node.channel] = gain;
    }
    result
}
