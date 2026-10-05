//! Scalar HOA3 ACN/SN3D preparation, preserving the established float/double boundaries.
#![allow(clippy::needless_range_loop, clippy::excessive_precision)]
use super::{Coefficients, Source, Trace, invalid};
use crate::Result;
type Vec3 = [f32; 3];
const RAD: f32 = std::f32::consts::PI / 180.;
const OUTER: f32 = 1. / 12.;
const INNER: f32 = 1. / 24.;
// Keep the exact decimal binary32 samples shared by the existing C++ backends.
#[allow(clippy::approx_constant)]
pub const DISK: [[f32; 3]; 17] = [
    [0., 0., 0.],
    [1., 0., OUTER],
    [-1., 0., OUTER],
    [0., 1., OUTER],
    [0., -1., OUTER],
    [0.70710678, 0.70710678, OUTER],
    [-0.70710678, 0.70710678, OUTER],
    [0.70710678, -0.70710678, OUTER],
    [-0.70710678, -0.70710678, OUTER],
    [0.5, 0., INNER],
    [-0.5, 0., INNER],
    [0., 0.5, INNER],
    [0., -0.5, INNER],
    [0.35355339, 0.35355339, INNER],
    [-0.35355339, 0.35355339, INNER],
    [0.35355339, -0.35355339, INNER],
    [-0.35355339, -0.35355339, INNER],
];
pub fn length([x, y, z]: Vec3) -> f32 {
    let (x, y, z) = (f64::from(x), f64::from(y), f64::from(z));
    (((x * x) + (y * y)) + (z * z)).sqrt() as f32
}
fn normalize(v: Vec3) -> Vec3 {
    let len = 1e-6f32.max(length(v));
    [v[0] / len, v[1] / len, v[2] / len]
}
fn cross(a: Vec3, b: Vec3) -> Vec3 {
    [
        (a[1] * b[2]) - (a[2] * b[1]),
        (a[2] * b[0]) - (a[0] * b[2]),
        (a[0] * b[1]) - (a[1] * b[0]),
    ]
}
fn add(a: Vec3, b: Vec3) -> Vec3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}
fn scale(v: Vec3, s: f32) -> Vec3 {
    [v[0] * s, v[1] * s, v[2] * s]
}
fn sh([x, y, z]: Vec3) -> [f32; 16] {
    let sqrt3 = 3f32.sqrt();
    let sqrt15 = 15f32.sqrt();
    let sqrt5_8 = (5f32 / 8.).sqrt();
    let sqrt3_8 = (3f32 / 8.).sqrt();
    [
        1.,
        y,
        z,
        x,
        sqrt3 * x * y,
        sqrt3 * y * z,
        0.5 * (3. * z * z - 1.),
        sqrt3 * x * z,
        0.5 * sqrt3 * (x * x - y * y),
        sqrt5_8 * y * (3. * x * x - y * y),
        sqrt15 * x * y * z,
        sqrt3_8 * y * (5. * z * z - 1.),
        0.5 * z * (5. * z * z - 3.),
        sqrt3_8 * x * (5. * z * z - 1.),
        0.5 * sqrt15 * z * (x * x - y * y),
        sqrt5_8 * x * (x * x - 3. * y * y),
    ]
}
fn polar(az_deg: f32, el_deg: f32, trace: &mut Trace) -> Vec3 {
    let az = az_deg * RAD;
    let el = el_deg * RAD;
    let cos_el = el.cos();
    if trace.flags & 1 == 0 {
        trace.polar = [az_deg, el_deg, az, el, cos_el, az.cos(), az.sin(), el.sin()];
        trace.flags |= 1;
    }
    [cos_el * az.cos(), cos_el * az.sin(), el.sin()]
}
fn direction(source: &Source, trace: &mut Trace) -> Vec3 {
    let [x, y, z] = source.position;
    if source.cartesian == 1 {
        normalize([y, -x, z])
    } else {
        polar(x, y, trace)
    }
}
fn encode(v: Vec3, trace: &mut Trace) -> [f32; 16] {
    let n = normalize(v);
    let result = sh(n);
    if trace.flags & 2 == 0 {
        trace.direction = v;
        trace.normalized = n;
        trace.coefficients = result;
        trace.flags |= 2;
    }
    result
}
// std::max(0.0F, value) keeps positive zero on a signed-zero tie.
fn nonnegative(value: f32) -> f32 {
    if value > 0. { value } else { 0. }
}
pub fn extent(source: &Source, trace: &mut Trace) -> [f32; 16] {
    let distance = if source.cartesian == 1 {
        length(source.position)
    } else {
        source.position[2]
    };
    let spread_scale = (1. / 0.4f32.max(distance)).clamp(0.5, 2.5);
    let depth = nonnegative(source.depth) * 20. * spread_scale;
    let width = nonnegative(source.width) * 60. * spread_scale + depth;
    let height = nonnegative(source.height) * 45. * spread_scale + depth;
    if width <= 1e-4 && height <= 1e-4 {
        return encode(direction(source, trace), trace);
    }
    let center = direction(source, trace);
    let mut horizontal = cross([0., 0., 1.], center);
    if length(horizontal) < 1e-4 {
        horizontal = [1., 0., 0.];
    } else {
        horizontal = normalize(horizontal);
    }
    let vertical = normalize(cross(center, horizontal));
    let mut result = [0.; 16];
    for sample in DISK {
        let h = (sample[0] * width * RAD).tan();
        let v = (sample[1] * height * RAD).tan();
        let dir = normalize(add(add(center, scale(horizontal, h)), scale(vertical, v)));
        let coeff = encode(dir, trace);
        for i in 0..16 {
            result[i] += coeff[i] * sample[2];
        }
    }
    result
}
pub fn compile(
    kind: u32,
    sources: &[Source],
    object_gain: f32,
    trace: &mut Trace,
) -> Result<Coefficients> {
    if kind > 2
        || !object_gain.is_finite()
        || !matches!(sources.len(), 1 | 3)
        || (kind != 0 && sources.len() != 1)
    {
        return Err(invalid());
    }
    for s in sources {
        if s.cartesian > 1
            || s.position.iter().any(|v| !v.is_finite())
            || ![s.width, s.height, s.depth, s.gain, s.diffuse]
                .iter()
                .all(|v| v.is_finite())
        {
            return Err(invalid());
        }
    }
    let mut result = Coefficients::default();
    if kind == 0 {
        for (index, s) in sources.iter().enumerate() {
            let source_sh = extent(s, trace);
            let combined = s.gain * object_gain;
            let diffuse = s.diffuse.clamp(0., 1.);
            let direct = combined * (1. - diffuse).sqrt();
            let diffuse_gain = combined * diffuse.sqrt();
            let slot = if sources.len() == 3 { index } else { 1 };
            let mut diffuse_source = *s;
            diffuse_source.width = diffuse_source.width.max(1.);
            diffuse_source.height = diffuse_source.height.max(1.);
            let diffuse_sh = extent(&diffuse_source, trace);
            for i in 0..16 {
                result.direct[i] += source_sh[i] * direct;
                result.diffuse[slot][i] += diffuse_sh[i] * diffuse_gain;
            }
        }
    } else {
        let s = &sources[0];
        if kind == 2 {
            result.direct[0] = 1.;
        } else {
            if s.cartesian != 0 {
                return Err(invalid());
            }
            result.direct = encode(polar(s.position[0], s.position[1], trace), trace);
        }
        let gain = s.gain * object_gain;
        for c in &mut result.direct {
            *c *= gain;
        }
    }
    if result
        .direct
        .iter()
        .chain(result.diffuse.iter().flatten())
        .any(|v| !v.is_finite())
    {
        return Err(invalid());
    }
    Ok(result)
}
