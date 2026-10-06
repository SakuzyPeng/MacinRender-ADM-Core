// EBU libear polar_extent.cpp / polar_extent_scalar.cpp (Apache-2.0).
use crate::{Result, geom::*, panner::Panner};
use std::f64::consts::PI;

pub struct Extent {
    points: Vec<[f32; 3]>,
    gains: Vec<Vec<f32>>,
}
impl Extent {
    pub fn new(panner: &Panner) -> Result<Self> {
        let mut points = Vec::new();
        let mut gains = Vec::new();
        for row in 0..37 {
            let el = -90.0 + row as f64 * 5.0;
            let n = (el.to_radians().cos() * 72.0).round().max(1.0) as usize;
            for i in 0..n {
                let p = cart([i as f64 * (360.0 / n as f64), el, 1.0]);
                points.push([p.x as f32, p.y as f32, p.z as f32]);
                gains.push(panner.gains(p)?.into_iter().map(|g| g as f32).collect());
            }
        }
        Ok(Self { points, gains })
    }
    fn spread(&self, panner: &Panner, position: Vec3, width: f64, height: f64) -> Result<Vec<f64>> {
        let spread = interp(width.max(height), &[0.0, 10.0], &[0.0, 1.0]);
        let mut out = vec![0.0; panner.count()];
        if 1.0 - spread > 1e-10 {
            for (o, g) in out.iter_mut().zip(panner.gains(position)?) {
                *o += (1.0 - spread) * g * g;
            }
        }
        if spread > 1e-10 {
            let weight = Weight::new(position, width.max(5.0), height.max(5.0));
            let mut sum = vec![0.0f32; out.len()];
            for (p, gains) in self.points.iter().zip(&self.gains) {
                let w = weight.weight(*p);
                if w == 1.0 {
                    for (s, g) in sum.iter_mut().zip(gains) {
                        *s += g;
                    }
                } else if w != 0.0 {
                    for (s, g) in sum.iter_mut().zip(gains) {
                        *s += w * g;
                    }
                }
            }
            let inv = 1.0 / sum.iter().map(|v| v * v).sum::<f32>().sqrt();
            for (o, s) in out.iter_mut().zip(sum) {
                let v = s * inv;
                *o += spread * (v * v) as f64;
            }
        }
        for o in &mut out {
            *o = o.sqrt();
        }
        Ok(out)
    }
    pub fn gains(
        &self,
        panner: &Panner,
        position: Vec3,
        width: f64,
        height: f64,
        depth: f64,
    ) -> Result<Vec<f64>> {
        let distance = position.norm();
        let calc = |d| {
            self.spread(
                panner,
                position,
                extent_mod(width, d),
                extent_mod(height, d),
            )
        };
        if depth == 0.0 {
            calc(distance)
        } else {
            let a = calc((distance - depth / 2.0).max(0.0))?;
            let b = calc((distance + depth / 2.0).max(0.0))?;
            Ok(a.iter()
                .zip(b)
                .map(|(a, b)| ((a * a + b * b) / 2.0).sqrt())
                .collect())
        }
    }
}
fn extent_mod(extent: f64, distance: f64) -> f64 {
    let size = interp(extent, &[0.0, 360.0], &[0.2, 1.0]);
    let e1 = 4.0 * size.atan2(1.0).to_degrees();
    interp(
        4.0 * size.atan2(distance).to_degrees(),
        &[0.0, e1, 360.0],
        &[0.0, extent, 360.0],
    )
}
struct Weight {
    basis: [[f32; 3]; 3],
    circle: [f32; 2],
    test: [f32; 2],
    circular: bool,
    cos_start: f32,
    cos_end: f32,
    sin_start: f32,
    sin_end: f32,
    m: f32,
    c: f32,
}
fn dot(a: [f32; 3], b: [f32; 3]) -> f32 {
    (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]
}
impl Weight {
    fn new(mut position: Vec3, width: f64, height: f64) -> Self {
        position = if position.norm() < 1e-10 {
            Vec3::new(0.0, 1.0, 0.0)
        } else {
            position.normalize()
        };
        let el = elevation(position);
        let az = if el.abs() > 90.0 - 1e-5 {
            0.0
        } else {
            azimuth(position)
        };
        let mut basis = [
            cart([az - 90.0, 0.0, 1.0]),
            cart([az, el, 1.0]),
            cart([az, el + 90.0, 1.0]),
        ];
        let (mut w, mut h) = (width.to_radians() / 2.0, height.to_radians() / 2.0);
        if h > w {
            std::mem::swap(&mut h, &mut w);
            basis = [basis[2], basis[1], -basis[0]];
        }
        let modified = interp(w, &[0.0, PI / 2.0, PI], &[0.0, PI / 2.0, PI + h]);
        w = interp(
            h,
            &[0.0, PI / 4.0, PI / 2.0, PI],
            &[modified, modified, w, w],
        );
        let cp = w - h;
        let end = h + 10.0f64.to_radians();
        let m = (1.0 / (h - end)) as f32;
        Self {
            basis: basis.map(|p| [p.x as f32, p.y as f32, p.z as f32]),
            circle: [cp.sin() as f32, cp.cos() as f32],
            test: [-cp.cos() as f32, cp.sin() as f32],
            circular: w - h < 1e-6,
            cos_start: if h < PI { h.cos() as f32 } else { -1.0 },
            cos_end: if end < PI {
                end.cos() as f32
            } else {
                -(1.0 + 1e-6)
            },
            sin_start: if h < PI / 2.0 { h.sin() as f32 } else { 1.0 },
            sin_end: if end < PI / 2.0 {
                end.sin() as f32
            } else {
                1.0 + 1e-6
            },
            m,
            c: (-(m as f64) * end) as f32,
        }
    }
    fn cosine(&self, x: f32) -> f32 {
        if x >= self.cos_start {
            1.0
        } else if x <= self.cos_end {
            0.0
        } else {
            self.m * x.acos() + self.c
        }
    }
    fn weight(&self, p: [f32; 3]) -> f32 {
        if self.circular {
            return self.cosine(dot(p, self.basis[1]));
        }
        let t = self.basis.map(|row| dot(p, row));
        let x = t[0].abs();
        if x * self.test[0] + t[1] * self.test[1] >= 0.0 {
            let s = t[2].abs();
            if s <= self.sin_start {
                1.0
            } else if s >= self.sin_end {
                0.0
            } else {
                self.m * s.asin() + self.c
            }
        } else {
            self.cosine(x * self.circle[0] + t[1] * self.circle[1])
        }
    }
}
