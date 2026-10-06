// EBU libear HOA/AllRAD algorithms (Apache-2.0).
use crate::{Error, Layout, Result, geom::*, panner::Panner};
use nalgebra::DMatrix;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Normalization {
    N3d,
    Sn3d,
    Fuma,
}
pub fn validate(n: &[i32], m: &[i32], norm: Normalization) -> Result<()> {
    if n.is_empty()
        || n.len() != m.len()
        || n.iter()
            .zip(m)
            .any(|(&n, &m)| n < 0 || m.unsigned_abs() > n as u32)
    {
        return Err(Error::InvalidArgument("Invalid HOA orders/degrees"));
    }
    // libear uses double factorials (finite through 170) and FuMa through order 3.
    if n.iter().zip(m).any(|(&n, &m)| {
        n as u64 + m.unsigned_abs() as u64 > 170 || (norm == Normalization::Fuma && n > 3)
    }) {
        return Err(Error::Unsupported("Unsupported HOA normalization/order"));
    }
    Ok(())
}
fn norm(n: i32, m: i32, kind: Normalization) -> f64 {
    let factorial = |k: i32| (1..=k).map(|i| i as f64).product::<f64>();
    let ratio = factorial(n - m) / factorial(n + m);
    match kind {
        Normalization::N3d => ((2 * n + 1) as f64 * ratio).sqrt(),
        Normalization::Sn3d => ratio.sqrt(),
        Normalization::Fuma => {
            ratio.sqrt()
                * match (n, m) {
                    (0, 0) => 1.0 / 2.0f64.sqrt(),
                    (2, 1 | 2) => 2.0 / 3.0f64.sqrt(),
                    (3, 1) => (45.0f64 / 32.0).sqrt(),
                    (3, 2) => 3.0 / 5.0f64.sqrt(),
                    (3, 3) => (8.0f64 / 5.0).sqrt(),
                    _ => 1.0,
                }
        }
    }
}
fn harmonic(n: i32, m: i32, az: f64, el: f64) -> f64 {
    let k = m.abs();
    let x = el.sin();
    let mut p = 1.0;
    let s = ((1.0 - x) * (1.0 + x)).sqrt();
    for i in 1..=k {
        p *= (2 * i - 1) as f64 * s;
    }
    if n > k {
        let mut previous = p;
        p *= x * (2 * k + 1) as f64;
        for i in k + 2..=n {
            let next =
                ((2 * i - 1) as f64 * x * p - (i + k - 1) as f64 * previous) / (i - k) as f64;
            previous = p;
            p = next;
        }
    }
    let scale = if m > 0 {
        2.0f64.sqrt() * (m as f64 * az).cos()
    } else if m < 0 {
        -2.0f64.sqrt() * (m as f64 * az).sin()
    } else {
        1.0
    };
    norm(n, k, Normalization::N3d) * p * scale
}
pub struct Decoder {
    angles: Vec<[f64; 2]>,
    virtual_gains: DMatrix<f64>,
}
impl Decoder {
    pub fn new(panner: &Panner) -> Result<Self> {
        let text = include_str!("../assets/Design_5200_100_random.dat");
        let mut angles = Vec::new();
        let mut columns = Vec::new();
        for line in text.lines().filter(|l| !l.trim().is_empty()) {
            let mut values = line
                .split_whitespace()
                .map(|s| s.parse::<f64>().expect("checked-in HOA point"));
            let phi = values.next().unwrap();
            let theta = values.next().unwrap();
            let p = Vec3::new(
                theta.sin() * phi.cos(),
                theta.sin() * phi.sin(),
                theta.cos(),
            );
            angles.push([-p.x.atan2(p.y), p.z.atan2(p.x.hypot(p.y))]);
            columns.extend(panner.gains(p)?);
        }
        let virtual_gains = DMatrix::from_column_slice(panner.count(), angles.len(), &columns);
        Ok(Self {
            angles,
            virtual_gains,
        })
    }
    pub fn calculate(
        &self,
        layout: &Layout,
        orders: &[i32],
        degrees: &[i32],
        normalization: Normalization,
    ) -> Result<Vec<f64>> {
        let count = self.angles.len();
        let y = DMatrix::from_fn(orders.len(), count, |i, p| {
            harmonic(orders[i], degrees[i], self.angles[p][0], self.angles[p][1])
        });
        let mut d = &self.virtual_gains * (y.transpose() / count as f64);
        let scale = (count as f64).sqrt() / (&d * &y).norm();
        d *= scale;
        let mut out = Vec::with_capacity(orders.len() * layout.channels.len());
        for i in 0..orders.len() {
            let conversion = norm(orders[i], degrees[i].abs(), Normalization::N3d)
                / norm(orders[i], degrees[i].abs(), normalization);
            let mut speaker = 0;
            for c in &layout.channels {
                if c.lfe {
                    out.push(0.0);
                } else {
                    out.push(d[(speaker, i)] * conversion);
                    speaker += 1;
                }
            }
        }
        if out.iter().any(|x| !x.is_finite()) {
            return Err(Error::RenderFailed("Non-finite HOA decoder"));
        }
        Ok(out)
    }
}
