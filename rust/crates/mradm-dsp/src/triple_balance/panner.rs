//! Fixed room geometry and scalar arithmetic from the verified Triple Balance kernels.
#![allow(clippy::excessive_precision, clippy::needless_range_loop)]
use super::{Layout, Position, invalid};
use crate::{Error, Result};
use std::f32::consts::FRAC_PI_2;

pub type Gains = [f32; 24];
pub type SizeGains = [f32; 11];
#[derive(Clone, Copy, Default)]
pub struct Mix {
    pub direct: f32,
    pub spread: Gains,
}
#[derive(Clone, Copy)]
pub struct Node {
    pub channel: usize,
    pub position: Position,
    pub filter: i32,
    pub sign: f32,
}
const A: f32 = 80. / 155.;
const W: f32 = 105. / 155.;
const fn node(c: usize, x: f32, y: f32, z: f32, filter: i32, sign: f32) -> Node {
    Node {
        channel: c,
        position: Position { x, y, z },
        filter,
        sign,
    }
}
pub const NODES: [Node; 22] = [
    node(22, -A, A, -1., 2, 1.),
    node(21, 0., A, -1., -1, 0.),
    node(23, A, A, -1., 2, -1.),
    node(4, -1., -1., 0., 2, 1.),
    node(8, 0., -1., 0., -1, 0.),
    node(5, 1., -1., 0., 2, -1.),
    node(10, -1., 0., 0., 1, 1.),
    node(11, 1., 0., 0., 1, -1.),
    node(0, -1., W, 0., 1, 1.),
    node(1, 1., W, 0., 1, -1.),
    node(6, -1., 1., 0., 0, 1.),
    node(2, 0., 1., 0., -1, 0.),
    node(7, 1., 1., 0., 0, -1.),
    node(16, -A, -A, 1., 0, 1.),
    node(20, 0., -A, 1., -1, 0.),
    node(17, A, -A, 1., 0, -1.),
    node(18, -A, 0., 1., 0, 1.),
    node(15, 0., 0., 1., -1, 0.),
    node(19, A, 0., 1., 0, -1.),
    node(12, -A, A, 1., 3, 1.),
    node(14, 0., A, 1., -1, 0.),
    node(13, A, A, 1., 3, -1.),
];
fn pair<const PORTABLE: bool>(f: f64) -> (f64, f64) {
    let angle = f.clamp(0., 1.) * std::f64::consts::PI * 0.5;
    (cos::<PORTABLE>(angle), sin::<PORTABLE>(angle))
}
fn row<const PORTABLE: bool>(x: f64, nodes: &[(f64, usize)]) -> [f64; 24] {
    let mut g = [0.; 24];
    if x <= nodes[0].0 {
        g[nodes[0].1] = 1.;
    } else if x >= nodes[nodes.len() - 1].0 {
        g[nodes[nodes.len() - 1].1] = 1.;
    } else {
        for n in nodes.windows(2) {
            if x <= n[1].0 {
                let (a, b) = pair::<PORTABLE>((x - n[0].0) / (n[1].0 - n[0].0));
                g[n[0].1] = a;
                g[n[1].1] = b;
                break;
            }
        }
    }
    g
}
type Row<'a> = (f64, &'a [(f64, usize)]);
fn layer<const PORTABLE: bool>(x: f64, y: f64, rows: &[Row<'_>], channels: usize) -> [f64; 24] {
    if y <= rows[0].0 {
        return row::<PORTABLE>(x, rows[0].1);
    }
    if y >= rows[rows.len() - 1].0 {
        return row::<PORTABLE>(x, rows[rows.len() - 1].1);
    }
    for r in rows.windows(2) {
        if y <= r[1].0 {
            let mut low = row::<PORTABLE>(x, r[0].1);
            let high = row::<PORTABLE>(x, r[1].1);
            let (a, b) = pair::<PORTABLE>((y - r[0].0) / (r[1].0 - r[0].0));
            for c in 0..channels {
                low[c] = a * low[c] + b * high[c];
            }
            return low;
        }
    }
    [0.; 24]
}
fn point_impl<const PORTABLE: bool>(
    position: Position,
    gain: f32,
    layout: Layout,
) -> Result<Gains> {
    if !gain.is_finite() {
        return Err(invalid());
    }
    if layout == Layout::Room222 {
        let mut g = room_impl::<PORTABLE>(position, 0.)?;
        for x in &mut g {
            *x *= gain;
        }
        return Ok(g);
    }
    validate(position, 0., false)?;
    let x = (f64::from(position.x) * 155. + 0.5).floor();
    let y = -(-f64::from(position.y) * 155. + 0.5).floor();
    let z = (f64::from(position.z) * 75. + 0.5).floor();
    let (a, b) = pair::<PORTABLE>((z / 75.).clamp(0., 1.));
    let mut middle_rows: [Row<'_>; 4] = [
        (-155., &[(-155., 6), (155., 7)]),
        (0., &[(-155., 4), (155., 5)]),
        (105., &[(-155., 8), (155., 9)]),
        (155., &[(-155., 0), (0., 2), (155., 1)]),
    ];
    let middle = if layout == Layout::Seven {
        middle_rows[2] = middle_rows[3];
        layer::<PORTABLE>(x, y, &middle_rows[..3], 12)
    } else {
        layer::<PORTABLE>(x, y, &middle_rows, 16)
    };
    let upper = if layout == Layout::Seven {
        layer::<PORTABLE>(
            x,
            y,
            &[
                (-80., &[(-80., 10), (80., 11)]),
                (80., &[(-80., 8), (80., 9)]),
            ],
            12,
        )
    } else {
        layer::<PORTABLE>(
            x,
            y,
            &[
                (-80., &[(-80., 14), (80., 15)]),
                (0., &[(-80., 12), (80., 13)]),
                (80., &[(-80., 10), (80., 11)]),
            ],
            16,
        )
    };
    let mut g = [0.; 24];
    for c in 0..layout.channels() {
        g[c] = ((a * middle[c] + b * upper[c]) * f64::from(gain)) as f32;
    }
    Ok(g)
}
fn validate(p: Position, size: f32, extended: bool) -> Result<()> {
    if ![p.x, p.y, p.z, size].iter().all(|v| v.is_finite())
        || p.x.abs() > 1.
        || p.y.abs() > 1.
        || p.z > 1.
        || p.z < if extended { -1. } else { 0. }
        || !(0. ..=1.).contains(&size)
    {
        return Err(Error::Unsupported(
            "Triple Balance requires finite room coordinates and size",
        ));
    }
    Ok(())
}
fn power_weights<const PORTABLE: bool>(knots: &[f64], lo: f64, hi: f64) -> [f64; 4] {
    let mut w = [0.; 4];
    let n = knots.len();
    if n == 1 {
        w[0] = 1.;
        return w;
    }
    if hi - lo < 1e-10 {
        let x = (lo + hi) * 0.5;
        if x <= knots[0] {
            w[0] = 1.;
        } else if x >= knots[n - 1] {
            w[n - 1] = 1.;
        } else {
            for i in 1..n {
                if x <= knots[i] {
                    let a =
                        (x - knots[i - 1]) / (knots[i] - knots[i - 1]) * std::f64::consts::PI / 2.;
                    w[i - 1] = cos::<PORTABLE>(a).powi(2);
                    w[i] = sin::<PORTABLE>(a).powi(2);
                    break;
                }
            }
        }
        return w;
    }
    w[0] += (hi.min(knots[0]) - lo).max(0.);
    w[n - 1] += (hi - lo.max(knots[n - 1])).max(0.);
    for i in 1..n {
        let l = lo.max(knots[i - 1]);
        let r = hi.min(knots[i]);
        if l >= r {
            continue;
        }
        let span = knots[i] - knots[i - 1];
        let integral = |x: f64| {
            let t = x - knots[i - 1];
            t * 0.5
                + span * sin::<PORTABLE>(std::f64::consts::PI * t / span)
                    / (2. * std::f64::consts::PI)
        };
        let before = integral(r) - integral(l);
        w[i - 1] += before;
        w[i] += r - l - before;
    }
    for v in &mut w[..n] {
        *v = (*v / (hi - lo)).max(0.);
    }
    w
}
fn room_impl<const PORTABLE: bool>(p: Position, size: f32) -> Result<Gains> {
    validate(p, size, true)?;
    let interval = |c: f32| {
        (
            (-1f64).max(f64::from(c) - f64::from(size)),
            1f64.min(f64::from(c) + f64::from(size)),
        )
    };
    let (xlo, xhi) = interval(p.x);
    let (ylo, yhi) = interval(p.y);
    let (zlo, zhi) = interval(p.z);
    let vertical = power_weights::<PORTABLE>(&[-1., 0., 1.], zlo, zhi);
    let ranges: [&[(usize, usize)]; 3] = [
        &[(0, 3)],
        &[(3, 6), (6, 8), (8, 10), (10, 13)],
        &[(13, 16), (16, 19), (19, 22)],
    ];
    let mut power = [0f64; 24];
    for i in 0..3 {
        let rows = ranges[i];
        let mut depths = [0.; 4];
        for (j, &(a, _)) in rows.iter().enumerate() {
            depths[j] = f64::from(NODES[a].position.y);
        }
        let row_weights = power_weights::<PORTABLE>(&depths[..rows.len()], ylo, yhi);
        for (j, &(a, b)) in rows.iter().enumerate() {
            let mut widths = [0.; 3];
            for (k, n) in NODES[a..b].iter().enumerate() {
                widths[k] = f64::from(n.position.x);
            }
            let horizontal = power_weights::<PORTABLE>(&widths[..b - a], xlo, xhi);
            for (k, n) in NODES[a..b].iter().enumerate() {
                power[n.channel] += vertical[i] * row_weights[j] * horizontal[k];
            }
        }
    }
    Ok(power.map(|x| x.sqrt() as f32))
}
fn room_mix_impl<const PORTABLE: bool>(spatial: Gains, size: f32) -> Mix {
    mix_impl::<PORTABLE>(spatial, size, true)
}
fn mix_impl<const PORTABLE: bool>(spatial: Gains, size: f32, extended: bool) -> Mix {
    let phase = if extended {
        (size / 0.2).clamp(0., 1.) * std::f32::consts::PI / 2.
    } else {
        (size / 0.2).clamp(0., 1.) * FRAC_PI_2
    };
    let sine = sinf::<PORTABLE>(phase);
    let cosine = cosf::<PORTABLE>(phase);
    let mut result = Mix {
        direct: if size >= 0.2 { 0. } else { cosine * cosine },
        spread: [0.; 24],
    };
    let mut front = 0.;
    let mut rest = 0.;
    let is_front = |i: usize| {
        if extended {
            i == 2 || i == 6 || i == 7
        } else {
            i < 3
        }
    };
    let n = if extended { 24 } else { 11 };
    for i in 0..n {
        result.spread[i] = spatial[i] * (sine * sine);
        if is_front(i) {
            front += result.spread[i] * result.spread[i];
        } else {
            rest += result.spread[i] * result.spread[i];
        }
    }
    if front + rest > 1e-6 {
        let attenuation = if PORTABLE {
            libm::powf(10., size * -1.2 / 20.)
        } else {
            10f32.powf(size * -1.2 / 20.)
        };
        let square = attenuation * attenuation;
        let common = (((front + rest) * square) / (front + rest * square)).sqrt();
        for i in 0..n {
            result.spread[i] *= if is_front(i) {
                common
            } else {
                attenuation * common
            };
        }
    }
    if !extended {
        for gain in &mut result.spread {
            if *gain < 1e-6 {
                *gain = 0.;
            }
        }
    }
    result
}
const ROOM_MAX: f32 = 32767. / 32768.;
const RADIUS_STEP: f32 = 0.7 / 19.;
const SPEAKERS: [Position; 11] = [
    Position {
        x: 0.,
        y: 0.,
        z: 0.,
    },
    Position {
        x: ROOM_MAX,
        y: 0.,
        z: 0.,
    },
    Position {
        x: 0.5,
        y: 0.,
        z: 0.,
    },
    Position {
        x: 0.,
        y: 0.5,
        z: 0.,
    },
    Position {
        x: ROOM_MAX,
        y: 0.5,
        z: 0.,
    },
    Position {
        x: 0.,
        y: ROOM_MAX,
        z: 0.,
    },
    Position {
        x: ROOM_MAX,
        y: ROOM_MAX,
        z: 0.,
    },
    Position {
        x: 0.,
        y: 0.,
        z: ROOM_MAX,
    },
    Position {
        x: ROOM_MAX,
        y: 0.,
        z: ROOM_MAX,
    },
    Position {
        x: 0.,
        y: ROOM_MAX,
        z: ROOM_MAX,
    },
    Position {
        x: ROOM_MAX,
        y: ROOM_MAX,
        z: ROOM_MAX,
    },
];
const ROWS: [[usize; 3]; 5] = [[0, 2, 1], [3, 4, 4], [5, 6, 6], [7, 8, 8], [9, 10, 10]];
const COUNTS: [usize; 5] = [3, 2, 2, 2, 2];
// frexp for positive finite binary32, including subnormals. No unsafe or libm dependency.
fn frexp(v: f32) -> (f32, i32) {
    let mut value = v;
    let mut correction = 0;
    if value < f32::MIN_POSITIVE {
        value *= 16777216.;
        correction = -24;
    }
    let bits = value.to_bits();
    (
        f32::from_bits((bits & 0x7fffff) | (126 << 23)),
        ((bits >> 23) & 255) as i32 - 126 + correction,
    )
}
fn log2_approx(value: f32) -> f32 {
    let (m, e) = frexp(value);
    ((m * 4.079345703125 - 2.693115234375) + (m * m) * -1.38623046875) + e as f32
}
fn exp2_approx(value: f32) -> f32 {
    let exponent = value.floor();
    let f = value - exponent;
    let square = f * f;
    let p =
        (f * square) * 0.05889892578125 + (square * 0.25482177734375 + (f * 0.686279296875 + 1.));
    let e = exponent as i32;
    if e < -150 {
        0.
    } else if e > 127 {
        f32::INFINITY
    } else {
        (f64::from(p) * f64::from_bits(((e + 1023) as u64) << 52)) as f32
    }
}
fn power(v: f32, e: f32) -> f32 {
    if v > 0. {
        exp2_approx(log2_approx(v) * e)
    } else {
        0.
    }
}
fn repeated(step: f32, n: u32) -> f32 {
    let mut r = 0.;
    for _ in 0..n {
        r += step;
    }
    r
}
fn normalize(v: &mut SizeGains) {
    let mut sum = 0f32;
    for x in v.iter() {
        sum += x * x;
    }
    let norm = sum.sqrt().max(1e-5);
    for x in v {
        *x /= norm;
    }
}
fn exponent(radius: f32) -> f32 {
    if radius <= 0.125 {
        6.
    } else {
        radius * -6.956521987915039 + 6.869565010070801
    }
}
fn radius(size: f32) -> f32 {
    let sizes = [0., 0.2, 0.5, 0.75, 1.];
    let radii = [0., 0.075, 0.25, 0.45, 0.7];
    for i in 1..5 {
        if size <= sizes[i] {
            return radii[i - 1]
                + (size - sizes[i - 1]) / (sizes[i] - sizes[i - 1]) * (radii[i] - radii[i - 1]);
        }
    }
    1.
}
fn kernel<const PORTABLE: bool>(point: f32, center: f32, radius: f32, axis: usize) -> f32 {
    if radius < 2.5e-5 {
        return 0.;
    }
    let delta = (point - center) * 0.25;
    if delta.abs() > radius {
        return 0.;
    }
    let ratio = delta / radius;
    let square = ratio * ratio;
    let mut r = exp2_approx((square * square * -0.6328125) * 26.575424194335938);
    if axis == 2 {
        r *= cosf::<PORTABLE>((point * 0.34147748351097107) * 4.);
    }
    r
}
fn axis_basis<const PORTABLE: bool>(axis: usize, p: f32) -> SizeGains {
    let mut r = [0.; 11];
    if axis == 0 {
        for row in 0..5 {
            let n = ROWS[row];
            let count = COUNTS[row];
            if p <= SPEAKERS[n[0]].x {
                r[n[0]] = 1.;
            } else if p >= SPEAKERS[n[count - 1]].x {
                r[n[count - 1]] = 1.;
            } else {
                for i in 1..count {
                    let l = n[i - 1];
                    let h = n[i];
                    if p <= SPEAKERS[h].x {
                        let f = (p - SPEAKERS[l].x) / (SPEAKERS[h].x - SPEAKERS[l].x);
                        r[l] = cosf::<PORTABLE>(f * FRAC_PI_2);
                        r[h] = sinf::<PORTABLE>(f * FRAC_PI_2);
                        break;
                    }
                }
            }
        }
    } else if axis == 1 {
        for layer in 0..2 {
            let start = if layer == 0 { 0 } else { 3 };
            let count = if layer == 0 { 3 } else { 2 };
            let mut g = [0.; 3];
            if p <= 0. {
                g[0] = 1.;
            } else if p >= ROOM_MAX {
                g[count - 1] = 1.;
            } else {
                for i in 1..count {
                    let lo = SPEAKERS[ROWS[start + i - 1][0]].y;
                    let hi = SPEAKERS[ROWS[start + i][0]].y;
                    if p <= hi {
                        let f = (p - lo) / (hi - lo);
                        g[i - 1] = cosf::<PORTABLE>(f * FRAC_PI_2);
                        g[i] = sinf::<PORTABLE>(f * FRAC_PI_2);
                        break;
                    }
                }
            }
            for i in 0..count {
                for j in 0..COUNTS[start + i] {
                    r[ROWS[start + i][j]] = g[i];
                }
            }
        }
    } else {
        let lo = if p >= ROOM_MAX {
            0.
        } else {
            cosf::<PORTABLE>(p * FRAC_PI_2)
        };
        let hi = if p >= ROOM_MAX {
            1.
        } else {
            sinf::<PORTABLE>(p * FRAC_PI_2)
        };
        for i in 0..11 {
            r[i] = if i < 7 { lo } else { hi };
        }
    }
    r
}
// The integration lattice is fixed. Share its ~64 KiB of immutable values across
// Scene sessions; preparation happens before the allocation-free process boundary.
static LIVE_LATTICE: std::sync::OnceLock<Vec<SizeGains>> = std::sync::OnceLock::new();
pub(crate) fn prepare_live() {
    LIVE_LATTICE.get_or_init(|| {
        let mut values = Vec::with_capacity(1480);
        for axis in 0..3 {
            for p in 0..if axis == 2 { 4 } else { 35 } {
                for r in 0..20 {
                    values.push(quadrature_uncached::<true>(axis, p, r));
                }
            }
        }
        values
    });
}
fn quadrature<const PORTABLE: bool>(axis: usize, p: u32, r: u32) -> SizeGains {
    if PORTABLE {
        LIVE_LATTICE
            .get()
            .expect("Scene lattice prepared at creation")[axis * 700 + p as usize * 20 + r as usize]
    } else {
        quadrature_uncached::<false>(axis, p, r)
    }
}
fn quadrature_uncached<const PORTABLE: bool>(axis: usize, p: u32, r: u32) -> SizeGains {
    let count = if axis == 2 { 8 } else { 20 };
    let step = if axis == 2 { 1. / 7. } else { 1. / 19. };
    let center = repeated(if axis == 2 { 1. / 3. } else { 1. / 34. }, p);
    let radius = repeated(RADIUS_STEP, r);
    let exp = exponent(radius);
    let mut result = [0.; 11];
    let mut position = 0.;
    for _ in 0..count {
        let basis = axis_basis::<PORTABLE>(axis, position);
        let w = kernel::<PORTABLE>(position, center, radius, axis);
        for i in 0..11 {
            result[i] += power(basis[i] * w, exp);
        }
        position += step;
    }
    result
}
fn extent<const PORTABLE: bool>(axis: usize, coordinate: f32, radius: f32) -> SizeGains {
    let steps = if axis == 2 { 3 } else { 34 };
    let step = 1. / steps as f32;
    let a = ((coordinate * steps as f32).floor() as u32).min(steps);
    let b = (a + 1).min(steps);
    let x = if a == b {
        0.
    } else {
        ((coordinate - step * a as f32) / step).max(0.)
    };
    let c = (((radius * (1. / 1.4) + radius * (1. / 1.4)) * 19.).floor() as u32).min(19);
    let d = (c + 1).min(19);
    let y = if c == d {
        0.
    } else {
        ((radius - RADIUS_STEP * c as f32) / RADIUS_STEP).max(0.)
    };
    let weights = [
        1. - x.max(y),
        1. - (1. - x).max(y),
        1. - x.max(1. - y),
        1. - (1. - x).max(1. - y),
    ];
    let sum = weights[0] + weights[1] + weights[2] + weights[3];
    let corners = [
        quadrature::<PORTABLE>(axis, a, c),
        quadrature::<PORTABLE>(axis, b, c),
        quadrature::<PORTABLE>(axis, a, d),
        quadrature::<PORTABLE>(axis, b, d),
    ];
    let mut result = [0.; 11];
    for i in 0..11 {
        for k in 0..4 {
            result[i] += (weights[k] / sum) * corners[k][i];
        }
    }
    result
}
pub fn quantize(p: Position, size: f32) -> Result<[i32; 4]> {
    let values = [p.x, p.y, p.z, size];
    if !values
        .iter()
        .all(|v| v.is_finite() && (0. ..=1.).contains(v))
    {
        return Err(invalid());
    }
    Ok(values.map(|v| ((v * 32768. + 0.5).floor() as i32).min(32767)))
}
fn raw_impl<const PORTABLE: bool>(q: [i32; 4]) -> Result<SizeGains> {
    if !q.iter().all(|v| (0..=32767).contains(v)) {
        return Err(invalid());
    }
    let [x, y, z, size] = q.map(|v| v as f32 / 32768.);
    let r = radius(size);
    if r < 2.5e-5 {
        let px = axis_basis::<PORTABLE>(0, x);
        let py = axis_basis::<PORTABLE>(1, y);
        let pz = axis_basis::<PORTABLE>(2, z);
        let mut result = [0.; 11];
        for i in 0..11 {
            result[i] = px[i] * py[i] * pz[i];
        }
        return Ok(result);
    }
    let exp = exponent(r);
    let ax = extent::<PORTABLE>(0, x, r);
    let ay = extent::<PORTABLE>(1, y, r);
    let az = extent::<PORTABLE>(2, z, r);
    let wall = [
        kernel::<PORTABLE>(0., x, r, 0),
        kernel::<PORTABLE>(1., x, r, 0),
        kernel::<PORTABLE>(0., y, r, 1),
        kernel::<PORTABLE>(1., y, r, 1),
        kernel::<PORTABLE>(1., z, r, 0) * 0.20345592498779297,
    ];
    let mut volume = [0.; 11];
    let mut boundary = [0.; 11];
    for i in 0..11 {
        let s = SPEAKERS[i];
        volume[i] = (ax[i] * ay[i]) * az[i];
        let roof = if s.z > 0.5 { power(wall[4], exp) } else { 0. };
        let left = if s.x == 0. { power(wall[0], exp) } else { 0. };
        let right = if s.x == ROOM_MAX {
            power(wall[1], exp)
        } else {
            0.
        };
        let front = if s.y == 0. { power(wall[2], exp) } else { 0. };
        let rear = if s.y == ROOM_MAX {
            power(wall[3], exp)
        } else {
            0.
        };
        boundary[i] = ((roof * (ax[i] * ay[i]) + (ay[i] * az[i]) * left) + (ay[i] * az[i]) * right)
            + (ax[i] * az[i]) * front
            + (ax[i] * az[i]) * rear;
    }
    normalize(&mut volume);
    normalize(&mut boundary);
    let distance = x.min(1. - x).min(y).min(1. - y).min(1. - z);
    let mut w = 1.;
    if distance * 0.25 <= r || distance * 0.25 <= 0.05 {
        w = (distance / (r * 4.)) * ((distance * 5.) * (distance * 5.));
    }
    let mut result = [0.; 11];
    for i in 0..11 {
        result[i] = power(((boundary[i] + w * volume[i]) * 0.5) * 0.0625, 1. / exp);
    }
    normalize(&mut result);
    Ok(result)
}
fn size_mix_impl<const PORTABLE: bool>(raw: SizeGains, size: f32) -> Mix {
    let mut spatial = [0.; 24];
    spatial[..11].copy_from_slice(&raw);
    mix_impl::<PORTABLE>(spatial, size, false)
}

fn sin<const P: bool>(v: f64) -> f64 {
    if P { libm::sin(v) } else { v.sin() }
}
fn cos<const P: bool>(v: f64) -> f64 {
    if P { libm::cos(v) } else { v.cos() }
}
fn sinf<const P: bool>(v: f32) -> f32 {
    if P { libm::sinf(v) } else { v.sin() }
}
fn cosf<const P: bool>(v: f32) -> f32 {
    if P { libm::cosf(v) } else { v.cos() }
}

pub fn point(p: Position, gain: f32, layout: Layout) -> Result<Gains> {
    point_impl::<false>(p, gain, layout)
}
pub fn room(p: Position, size: f32) -> Result<Gains> {
    room_impl::<false>(p, size)
}
pub fn room_mix(spatial: Gains, size: f32) -> Mix {
    room_mix_impl::<false>(spatial, size)
}
pub fn raw(q: [i32; 4]) -> Result<SizeGains> {
    raw_impl::<false>(q)
}
pub fn size_mix(raw: SizeGains, size: f32) -> Mix {
    size_mix_impl::<false>(raw, size)
}
pub(crate) fn live_point(p: Position, gain: f32, layout: Layout) -> Result<Gains> {
    point_impl::<true>(p, gain, layout)
}
pub(crate) fn live_room(p: Position, size: f32) -> Result<Gains> {
    room_impl::<true>(p, size)
}
pub(crate) fn live_room_mix(spatial: Gains, size: f32) -> Mix {
    room_mix_impl::<true>(spatial, size)
}
pub(crate) fn live_raw(q: [i32; 4]) -> Result<SizeGains> {
    raw_impl::<true>(q)
}
pub(crate) fn live_size_mix(raw: SizeGains, size: f32) -> Mix {
    size_mix_impl::<true>(raw, size)
}
