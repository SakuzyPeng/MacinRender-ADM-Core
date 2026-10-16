// EBU libear HOA/AllRAD algorithms (Apache-2.0).
use crate::{Error, Layout, Result, geom::*, panner::Panner};
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
    let x = libm::sin(el);
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
        2.0f64.sqrt() * libm::cos(m as f64 * az)
    } else if m < 0 {
        -2.0f64.sqrt() * libm::sin(m as f64 * az)
    } else {
        1.0
    };
    norm(n, k, Normalization::N3d) * p * scale
}
/// Sum of squares of one matrix column in nalgebra's `dotc` order: eight running accumulators
/// over whole groups of eight, combined pairwise, then the remaining elements in order.
fn column_energy(column: &[f64]) -> f64 {
    let (groups, rest) = column.as_chunks::<8>();
    let mut acc = [0.0; 8];
    for group in groups {
        for (a, v) in acc.iter_mut().zip(group) {
            *a += v * v;
        }
    }
    let mut energy = 0.0;
    energy += acc[0] + acc[4];
    energy += acc[1] + acc[5];
    energy += acc[2] + acc[6];
    energy += acc[3] + acc[7];
    for v in rest {
        energy += v * v;
    }
    energy
}
/// `a * b` for column-major `rows x inner` and `inner x cols` matrices, each element summed over
/// `inner` in ascending order with separately rounded operations. This is the order of nalgebra's
/// scalar gemm; its dynamic-matrix path instead calls matrixmultiply, whose runtime-selected
/// AVX/FMA or NEON kernels make the result depend on the host.
fn product(a: &[f64], b: &[f64], rows: usize, inner: usize, cols: usize) -> Vec<f64> {
    let mut out = vec![0.0; rows * cols];
    for (column, b) in out.chunks_exact_mut(rows).zip(b.chunks_exact(inner)) {
        for (r, value) in column.iter_mut().enumerate() {
            let mut sum = a[r] * b[0];
            for k in 1..inner {
                sum += a[k * rows + r] * b[k];
            }
            *value = sum;
        }
    }
    out
}
pub struct Decoder {
    angles: Vec<[f64; 2]>,
    /// Column-major speaker x sampling-point gains.
    virtual_gains: Vec<f64>,
    speakers: usize,
}
impl Decoder {
    pub fn new(panner: &Panner) -> Result<Self> {
        let text = include_str!("../assets/Design_5200_100_random.dat");
        let mut angles = Vec::new();
        let mut virtual_gains = Vec::new();
        for line in text.lines().filter(|l| !l.trim().is_empty()) {
            let mut values = line
                .split_whitespace()
                .map(|s| s.parse::<f64>().expect("checked-in HOA point"));
            let phi = values.next().unwrap();
            let theta = values.next().unwrap();
            let p = Vec3::new(
                libm::sin(theta) * libm::cos(phi),
                libm::sin(theta) * libm::sin(phi),
                libm::cos(theta),
            );
            angles.push([
                -libm::atan2(p.x, p.y),
                libm::atan2(p.z, libm::hypot(p.x, p.y)),
            ]);
            virtual_gains.extend(panner.gains(p)?);
        }
        Ok(Self {
            angles,
            virtual_gains,
            speakers: panner.count(),
        })
    }
    pub fn calculate(
        &self,
        layout: &Layout,
        orders: &[i32],
        degrees: &[i32],
        normalization: Normalization,
    ) -> Result<Vec<f64>> {
        let (count, channels, speakers) = (self.angles.len(), orders.len(), self.speakers);
        // Column-major channel x point spherical harmonics, and its transpose divided by the
        // point count (the pseudo-inverse of the uniform t-design sampling).
        let mut y = vec![0.0; channels * count];
        let mut sampling = vec![0.0; count * channels];
        for (p, angle) in self.angles.iter().enumerate() {
            for i in 0..channels {
                let value = harmonic(orders[i], degrees[i], angle[0], angle[1]);
                y[p * channels + i] = value;
                sampling[i * count + p] = value / count as f64;
            }
        }
        // AllRAD: virtual-speaker gains times the sampling matrix, scaled so that the decoded
        // sampling directions carry the energy of `count` unit sources.
        let mut d = product(&self.virtual_gains, &sampling, speakers, count, channels);
        let decoded = product(&d, &y, speakers, channels, count);
        let energy: f64 = decoded
            .chunks_exact(speakers)
            .map(column_energy)
            .fold(0.0, |sum, e| sum + e);
        let scale = (count as f64).sqrt() / energy.sqrt();
        for v in &mut d {
            *v *= scale;
        }
        let mut out = Vec::with_capacity(orders.len() * layout.channels.len());
        for i in 0..orders.len() {
            let conversion = norm(orders[i], degrees[i].abs(), Normalization::N3d)
                / norm(orders[i], degrees[i].abs(), normalization);
            let mut speaker = 0;
            for c in &layout.channels {
                if c.lfe {
                    out.push(0.0);
                } else {
                    out.push(d[i * speakers + speaker] * conversion);
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
