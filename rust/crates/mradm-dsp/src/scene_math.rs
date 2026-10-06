//! Scene arithmetic with the original backend-specific precision and coordinate conventions.
#![allow(clippy::excessive_precision, clippy::approx_constant)]
use crate::{Error, Result};

pub const RAD: f32 = std::f32::consts::PI / 180.;
const DRAD: f64 = std::f64::consts::PI / 180.;
const DEG: f64 = 180. / std::f64::consts::PI;
pub type Vec3 = [f32; 3];
pub const DISK: [[f32; 3]; 17] = [
    [0., 0., 0.],
    [1., 0., 1. / 12.],
    [-1., 0., 1. / 12.],
    [0., 1., 1. / 12.],
    [0., -1., 1. / 12.],
    [0.70710678, 0.70710678, 1. / 12.],
    [-0.70710678, 0.70710678, 1. / 12.],
    [0.70710678, -0.70710678, 1. / 12.],
    [-0.70710678, -0.70710678, 1. / 12.],
    [0.5, 0., 1. / 24.],
    [-0.5, 0., 1. / 24.],
    [0., 0.5, 1. / 24.],
    [0., -0.5, 1. / 24.],
    [0.35355339, 0.35355339, 1. / 24.],
    [-0.35355339, 0.35355339, 1. / 24.],
    [0.35355339, -0.35355339, 1. / 24.],
    [-0.35355339, -0.35355339, 1. / 24.],
];

pub fn length([x, y, z]: Vec3) -> f32 {
    let (x, y, z) = (f64::from(x), f64::from(y), f64::from(z));
    (((x * x) + (y * y)) + (z * z)).sqrt() as f32
}
pub fn cross(a: Vec3, b: Vec3) -> Vec3 {
    [
        (a[1] * b[2]) - (a[2] * b[1]),
        (a[2] * b[0]) - (a[0] * b[2]),
        (a[0] * b[1]) - (a[1] * b[0]),
    ]
}
pub fn add(a: Vec3, b: Vec3) -> Vec3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}
pub fn scale(a: Vec3, v: f32) -> Vec3 {
    [a[0] * v, a[1] * v, a[2] * v]
}
pub fn normalize(v: Vec3, binaural: bool) -> Vec3 {
    let n = length(v);
    if binaural && n <= 1e-8 {
        return [0., 1., 0.];
    }
    let n = if binaural { n } else { max(1e-6, n) };
    [v[0] / n, v[1] / n, v[2] / n]
}
// std::max/min retain their first operand on ties and unordered comparisons.
pub fn max(a: f32, b: f32) -> f32 {
    if a < b { b } else { a }
}
pub fn min(a: f32, b: f32) -> f32 {
    if b < a { b } else { a }
}
pub fn clamp(x: f32, a: f32, b: f32) -> f32 {
    min(max(x, a), b)
}
pub fn cartesian_to_polar([x, y, z]: Vec3) -> Vec3 {
    let (x, y, z) = (f64::from(x), f64::from(y), f64::from(z));
    [
        ((-x).atan2(y) * DEG) as f32,
        (z.atan2(x.hypot(y)) * DEG) as f32,
        (((x * x) + (y * y) + (z * z)).sqrt()) as f32,
    ]
}
pub fn direction(az: f32, el: f32) -> Vec3 {
    let az = f64::from(az) * DRAD;
    let el = f64::from(el) * DRAD;
    let ce = el.cos();
    [
        (-az.sin() * ce) as f32,
        (az.cos() * ce) as f32,
        el.sin() as f32,
    ]
}
pub fn polar(v: Vec3, binaural: bool) -> [f32; 2] {
    if binaural {
        let [x, y, z] = normalize(v, true);
        [
            ((f64::from(-x)).atan2(f64::from(y)) * DEG) as f32,
            (f64::from(z).atan2(f64::from(x).hypot(f64::from(y))) * DEG) as f32,
        ]
    } else {
        let [x, y, z] = v;
        let degrees = 180. / std::f32::consts::PI;
        [(-x).atan2(y) * degrees, z.atan2(x.hypot(y)) * degrees]
    }
}
pub fn distance(a: Vec3, b: Vec3) -> f32 {
    distance_compat(a, b, false)
}
pub fn distance_compat(a: Vec3, b: Vec3, contract: bool) -> f32 {
    let [x, y, z] = [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
    cpp_madd(z, z, cpp_madd(x, x, y * y, contract), contract).sqrt()
}
pub fn wrap(mut az: f32) -> f32 {
    // For the normal finite range use precisely the old repeated additions.
    // Reject non-progressing subtraction rather than entering an unbounded loop.
    if !az.is_finite() {
        return f32::NAN;
    }
    if az > 180. && az - 360. == az || az <= -180. && az + 360. == az {
        return f32::NAN;
    }
    while az > 180. {
        az -= 360.;
    }
    while az <= -180. {
        az += 360.;
    }
    az
}
pub fn remainder_degrees(x: f32) -> f32 {
    let r = x % 360.;
    if r.abs() > 180. || (r.abs() == 180. && (x % 720.).abs() > 360.) {
        r - 360f32.copysign(x)
    } else {
        r
    }
}
fn remainder_double(x: f64) -> f64 {
    let r = x % 360.;
    if r.abs() > 180. || (r.abs() == 180. && (x % 720.).abs() > 360.) {
        r - 360f64.copysign(x)
    } else {
        r
    }
}
// Explicitly preserve the scalar contraction used by the frozen native build.
// The private caller selects it from the existing strict-FP/platform configuration.
pub(crate) fn cpp_madd(a: f32, b: f32, c: f32, contract: bool) -> f32 {
    if contract { a.mul_add(b, c) } else { a * b + c }
}
pub fn radii(width: f32, height: f32, depth: f32, distance: f32, contract: bool) -> [f32; 2] {
    let spread = clamp(1. / max(0.4, distance), 0.5, 2.5);
    let d = max(0., depth) * 20. * spread;
    [
        cpp_madd(max(0., width) * 60., spread, d, contract),
        cpp_madd(max(0., height) * 45., spread, d, contract),
    ]
}
pub fn spread(extent: Vec3, distance: f32, divergence: f32, range: f32, live: bool) -> f32 {
    let spread = clamp(1. / max(0.4, distance), 0.5, 2.5);
    let v = if live {
        extent
    } else {
        extent.map(|x| max(0., x))
    };
    let mut w = v[0] * 60. * spread;
    if !live && divergence > 1e-4 {
        w = max(w, max(0., range) * 0.5);
    }
    min(180., length([w, v[1] * 45. * spread, v[2] * 20. * spread]))
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct CloudPoint {
    pub azimuth: f32,
    pub elevation: f32,
    pub weight: f32,
    pub slot: u32,
}
#[allow(clippy::too_many_arguments)]
pub fn cloud(
    position: Vec3,
    cartesian: bool,
    extent: Vec3,
    gain: f32,
    binaural: bool,
    contract: bool,
    output: &mut [CloudPoint; 17],
    mut trace: Option<&mut [[f32; 14]; 17]>,
) -> usize {
    let p = if cartesian {
        cartesian_to_polar(position)
    } else {
        position
    };
    let [w, h] = radii(extent[0], extent[1], extent[2], p[2], contract);
    if w <= 1e-4 && h <= 1e-4 {
        output[0] = CloudPoint {
            azimuth: p[0],
            elevation: p[1],
            weight: gain,
            slot: 0,
        };
        return 1;
    }
    let center = direction(p[0], p[1]);
    let center = if binaural {
        normalize(center, true)
    } else {
        center
    };
    let mut horizontal = cross([0., 0., 1.], center);
    horizontal = if length(horizontal) < 1e-4 {
        [1., 0., 0.]
    } else {
        normalize(horizontal, binaural)
    };
    let vertical = normalize(cross(center, horizontal), binaural);
    let mut n = 0;
    for (slot, s) in DISK.iter().enumerate() {
        if s[2] <= 0. {
            continue;
        }
        let x = (s[0] * w * RAD).tan();
        let y = (s[1] * h * RAD).tan();
        let expanded = if binaural {
            add(add(center, scale(horizontal, x)), scale(vertical, y))
        } else {
            std::array::from_fn(|i| {
                cpp_madd(
                    vertical[i],
                    y,
                    cpp_madd(horizontal[i], x, center[i], contract),
                    contract,
                )
            })
        };
        let dir = normalize(expanded, binaural);
        let [az, el] = polar(dir, binaural);
        output[n] = CloudPoint {
            azimuth: az,
            elevation: el,
            weight: gain * s[2],
            slot: slot as u32,
        };
        if let Some(t) = trace.as_deref_mut() {
            t[n] = [
                center[0],
                center[1],
                center[2],
                horizontal[0],
                horizontal[1],
                horizontal[2],
                vertical[0],
                vertical[1],
                vertical[2],
                x,
                y,
                az,
                el,
                gain * s[2],
            ];
        }
        n += 1;
    }
    n
}
pub fn divergence(
    position: Vec3,
    cartesian: bool,
    divergence: f32,
    az_range: f32,
    pos_range: f32,
    gain: f32,
    output: &mut [CloudPoint; 3],
) -> usize {
    let p = if cartesian {
        cartesian_to_polar(position)
    } else {
        position
    };
    let d = clamp(divergence, 0., 1.);
    if d <= 1e-4 {
        output[0] = CloudPoint {
            azimuth: p[0],
            elevation: p[1],
            weight: gain,
            slot: 0,
        };
        return 1;
    }
    let angle = if cartesian && pos_range > 0. {
        (f64::from(pos_range).atan2(1e-6f64.max(f64::from(p[2]))) * DEG) as f32
    } else {
        az_range
    };
    let a = clamp(angle, 0., 120.);
    let side = d / (d + 1.);
    let center = (1. - d) / (d + 1.);
    for (i, (offset, weight)) in [(-a, side), (0., center), (a, side)]
        .into_iter()
        .enumerate()
    {
        output[i] = CloudPoint {
            azimuth: wrap(p[0] + offset),
            elevation: p[1],
            weight: gain * weight,
            slot: i as u32,
        };
    }
    3
}
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct Speaker {
    pub azimuth: f32,
    pub elevation: f32,
    pub is_lfe: u32,
}
pub fn nearest(position: Vec3, cartesian: bool, speakers: &[Speaker]) -> Option<(usize, f32)> {
    nearest_compat(position, cartesian, speakers, false)
}
pub fn nearest_compat(
    position: Vec3,
    cartesian: bool,
    speakers: &[Speaker],
    contract: bool,
) -> Option<(usize, f32)> {
    let p = if cartesian {
        cartesian_to_polar(position)
    } else {
        position
    };
    let src = direction(p[0], p[1]);
    let mut best = f32::MAX;
    let mut index = None;
    for (i, s) in speakers.iter().enumerate() {
        if s.is_lfe != 0 {
            continue;
        }
        let d = distance_compat(src, direction(s.azimuth, s.elevation), contract);
        if d < best {
            best = d;
            index = Some(i);
        }
    }
    index.map(|i| (i, best))
}
type Quaternion = [f64; 4];
fn multiply(a: Quaternion, b: Quaternion, contract: bool) -> Quaternion {
    let [w, x, y, z] = a;
    let [v, i, j, k] = b;
    if contract {
        return [
            (-z).mul_add(k, (-y).mul_add(j, w.mul_add(v, -(x * i)))),
            (-z).mul_add(j, y.mul_add(k, w.mul_add(i, x * v))),
            z.mul_add(i, y.mul_add(v, w.mul_add(j, -(x * k)))),
            z.mul_add(v, (-y).mul_add(i, w.mul_add(k, x * j))),
        ];
    }
    [
        w * v - x * i - y * j - z * k,
        w * i + x * v + y * k - z * j,
        w * j - x * k + y * v + z * i,
        w * k + x * j - y * i + z * v,
    ]
}
fn conjugate([w, x, y, z]: Quaternion) -> Quaternion {
    [w, -x, -y, -z]
}
fn axis(axis: [f64; 3], angle: f64) -> Quaternion {
    let h = angle * 0.5;
    let s = h.sin();
    [h.cos(), axis[0] * s, axis[1] * s, axis[2] * s]
}
#[derive(Clone, Copy)]
pub struct Rotation {
    head_to_world: Quaternion,
    contract: bool,
}
impl Rotation {
    pub fn new(pose: Vec3) -> Result<Self> {
        Self::new_compat(pose, false)
    }
    pub fn update(&mut self, pose: Vec3) -> Result<()> {
        *self = Self::new_compat(pose, self.contract)?;
        Ok(())
    }
    pub fn new_compat(pose: Vec3, contract: bool) -> Result<Self> {
        if pose.iter().any(|x| !x.is_finite()) {
            return Err(Error::InvalidArgument("Invalid head orientation"));
        }
        let roll = axis([0., 1., 0.], f64::from(pose[2]) * DRAD);
        let pitch = axis([1., 0., 0.], f64::from(pose[1]) * DRAD);
        let yaw = axis([0., 0., 1.], f64::from(pose[0]) * DRAD);
        Ok(Self {
            head_to_world: multiply(yaw, multiply(pitch, roll, contract), contract),
            contract,
        })
    }
    pub fn apply(&self, az: f32, el: f32, apple: bool) -> [f32; 2] {
        let a = f64::from(az) * DRAD;
        let e = f64::from(el) * DRAD;
        let ce = e.cos();
        let x = if apple { a.sin() * ce } else { -a.sin() * ce };
        let q = if apple {
            self.head_to_world
        } else {
            conjugate(self.head_to_world)
        };
        let [_, x, y, z] = multiply(
            multiply(q, [0., x, a.cos() * ce, e.sin()], self.contract),
            conjugate(q),
            self.contract,
        );
        if apple {
            [
                (x.atan2(y) * DEG) as f32,
                (z.clamp(-1., 1.).asin() * DEG) as f32,
            ]
        } else {
            [
                ((-x).atan2(y) * DEG) as f32,
                (z.atan2(x.hypot(y)) * DEG) as f32,
            ]
        }
    }
}
pub fn pose(input: &[f32], quaternion: bool) -> Result<[f32; 7]> {
    if input.len() != if quaternion { 4 } else { 3 } || input.iter().any(|x| !x.is_finite()) {
        return Err(Error::InvalidArgument("Invalid PoseBridge orientation"));
    }
    let mut q = if quaternion {
        [
            input[0] as f64,
            input[1] as f64,
            input[2] as f64,
            input[3] as f64,
        ]
    } else {
        let yaw = remainder_double(input[0] as f64) * (std::f64::consts::PI / 360.);
        let pitch = remainder_double(input[1] as f64) * (std::f64::consts::PI / 360.);
        let roll = remainder_double(input[2] as f64) * (std::f64::consts::PI / 360.);
        let cy = yaw.cos();
        let sy = yaw.sin();
        let cp = pitch.cos();
        let sp = pitch.sin();
        let cr = roll.cos();
        let sr = roll.sin();
        [
            (cy * sp * cr) + (sy * cp * sr),
            (sy * cp * cr) - (cy * sp * sr),
            (cy * cp * sr) - (sy * sp * cr),
            (cy * cp * cr) + (sy * sp * sr),
        ]
    };
    let n = (q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]);
    if n < 1e-12 {
        return Err(Error::InvalidArgument("Degenerate PoseBridge quaternion"));
    }
    let norm = n.sqrt();
    for x in &mut q {
        *x /= norm;
    }
    let [x, y, z, w] = q;
    let s = (2. * ((w * x) - (y * z))).clamp(-1., 1.);
    let (yaw, roll) = if s.abs() >= 1. - 1e-12 {
        (
            (2. * ((w * y) - (x * z))).atan2(1. - (2. * ((y * y) + (z * z)))),
            0.,
        )
    } else {
        let yaw = (2. * ((x * z) + (w * y))).atan2(1. - (2. * ((x * x) + (y * y))));
        let roll = (2. * ((x * y) + (w * z))).atan2(1. - (2. * ((x * x) + (z * z))));
        (yaw, roll)
    };
    Ok([
        x as f32,
        y as f32,
        z as f32,
        w as f32,
        (yaw * DEG) as f32,
        (s.asin() * DEG) as f32,
        (roll * DEG) as f32,
    ])
}
