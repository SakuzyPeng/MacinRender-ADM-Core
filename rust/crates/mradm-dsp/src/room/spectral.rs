//! Independent frequency bins in planar storage with safe SIMD dispatch. The scalar expression
//! tree and vertex/source accumulation order remain unchanged (no FMA).
use crate::Complex32;
use pulp::{Arch, Simd, WithSimd};

pub struct PlanarBank {
    bins: usize,
    stride: usize,
    values: Vec<f32>,
}
impl PlanarBank {
    pub fn new(input: &[Complex32], bins: usize) -> Self {
        let stride = bins.next_multiple_of(16);
        let mut values = vec![0.; input.len() / bins * stride * 2];
        for (row, spectrum) in input.chunks_exact(bins).enumerate() {
            for (bin, value) in spectrum.iter().enumerate() {
                values[row * stride * 2 + bin] = value.re;
                values[row * stride * 2 + stride + bin] = value.im;
            }
        }
        Self {
            bins,
            stride,
            values,
        }
    }
    pub fn stride(&self) -> usize {
        self.stride
    }
    pub fn row(&self, index: usize) -> (&[f32], &[f32]) {
        self.values[index * self.stride * 2..(index + 1) * self.stride * 2].split_at(self.stride)
    }
    pub fn storage_bytes(&self) -> usize {
        self.values.len() * size_of::<f32>()
    }
    pub fn fingerprint(&self) -> String {
        super::resources::hash_f32((0..self.values.len() / (self.stride * 2)).flat_map(|row| {
            let (re, im) = self.row(row);
            (0..self.bins).flat_map(move |i| [re[i], im[i]])
        }))
    }
}
type Pair<'a> = (&'a [f32], &'a [f32]);
fn rows<S: Simd>((r, i): Pair<'_>) -> impl Iterator<Item = (&S::f32s, &S::f32s)> {
    S::as_simd_f32s(r).0.iter().zip(S::as_simd_f32s(i).0)
}
struct AlignedAdd<'a> {
    destination: (&'a mut [f32], &'a mut [f32]),
    source: Pair<'a>,
    low: Pair<'a>,
    high: Pair<'a>,
    phase: Pair<'a>,
    alpha: f32,
    weight: f32,
}
impl WithSimd for AlignedAdd<'_> {
    type Output = ();
    #[inline(always)]
    fn with_simd<S: Simd>(self, simd: S) {
        let output = S::as_mut_simd_f32s(self.destination.0)
            .0
            .iter_mut()
            .zip(S::as_mut_simd_f32s(self.destination.1).0);
        let alpha = simd.splat_f32s(self.alpha);
        let weight = simd.splat_f32s(self.weight);
        for (((((dr, di), source), low), high), phase) in output
            .zip(rows::<S>(self.source))
            .zip(rows::<S>(self.low))
            .zip(rows::<S>(self.high))
            .zip(rows::<S>(self.phase))
        {
            let kr = simd.add_f32s(*low.0, simd.mul_f32s(simd.sub_f32s(*high.0, *low.0), alpha));
            let ki = simd.add_f32s(*low.1, simd.mul_f32s(simd.sub_f32s(*high.1, *low.1), alpha));
            let br = simd.sub_f32s(simd.mul_f32s(*source.0, kr), simd.mul_f32s(*source.1, ki));
            let bi = simd.add_f32s(simd.mul_f32s(*source.0, ki), simd.mul_f32s(*source.1, kr));
            *dr = simd.add_f32s(
                *dr,
                simd.mul_f32s(
                    simd.sub_f32s(simd.mul_f32s(br, *phase.0), simd.mul_f32s(bi, *phase.1)),
                    weight,
                ),
            );
            *di = simd.add_f32s(
                *di,
                simd.mul_f32s(
                    simd.add_f32s(simd.mul_f32s(br, *phase.1), simd.mul_f32s(bi, *phase.0)),
                    weight,
                ),
            );
        }
    }
}
pub fn aligned_add(
    destination: (&mut [f32], &mut [f32]),
    source: Pair<'_>,
    low: Pair<'_>,
    high: Pair<'_>,
    phase: Pair<'_>,
    alpha: f32,
    weight: f32,
) {
    static ARCH: std::sync::OnceLock<Arch> = std::sync::OnceLock::new();
    ARCH.get_or_init(Arch::new).dispatch(AlignedAdd {
        destination,
        source,
        low,
        high,
        phase,
        alpha,
        weight,
    });
}

struct ProductAdd<'a> {
    destination: (&'a mut [f32], &'a mut [f32]),
    source: Pair<'a>,
    transfer: Pair<'a>,
}
impl WithSimd for ProductAdd<'_> {
    type Output = ();
    #[inline(always)]
    fn with_simd<S: Simd>(self, simd: S) {
        let output = S::as_mut_simd_f32s(self.destination.0)
            .0
            .iter_mut()
            .zip(S::as_mut_simd_f32s(self.destination.1).0);
        for (((dr, di), (xr, xi)), (hr, hi)) in output
            .zip(rows::<S>(self.source))
            .zip(rows::<S>(self.transfer))
        {
            *dr = simd.add_f32s(
                *dr,
                simd.sub_f32s(simd.mul_f32s(*xr, *hr), simd.mul_f32s(*xi, *hi)),
            );
            *di = simd.add_f32s(
                *di,
                simd.add_f32s(simd.mul_f32s(*xr, *hi), simd.mul_f32s(*xi, *hr)),
            );
        }
    }
}
/// Planes are padded to a multiple of 16 bins. Each path retains its original
/// addition order; SIMD only runs independent frequencies together.
pub fn product_add(destination: (&mut [f32], &mut [f32]), source: Pair<'_>, transfer: Pair<'_>) {
    static ARCH: std::sync::OnceLock<Arch> = std::sync::OnceLock::new();
    ARCH.get_or_init(Arch::new).dispatch(ProductAdd {
        destination,
        source,
        transfer,
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn independent_bins_match_scalar_operations_bit_for_bit() {
        for count in [1usize, 3, 4, 5, 257, 513, 2049] {
            let values = |seed| {
                (0..count)
                    .map(|i| {
                        Complex32::new(
                            (((i * 73 + seed) % 157) as f32 - 78.) / 32.,
                            (((i * 13 + seed) % 59) as f32 - 29.) / 16.,
                        )
                    })
                    .collect::<Vec<_>>()
            };
            let source = values(3);
            let low = values(5);
            let high = values(7);
            let phase = values(11);
            let mut expected = values(13);
            let original = expected.clone();
            for i in 0..count {
                let kernel = low[i] + (high[i] - low[i]) * 0.37;
                expected[i] += ((source[i] * kernel) * phase[i]) * 0.29;
            }
            let a = PlanarBank::new(&source, count);
            let b = PlanarBank::new(&low, count);
            let c = PlanarBank::new(&high, count);
            let d = PlanarBank::new(&phase, count);
            let mut output = PlanarBank::new(&original, count);
            let stride = output.stride;
            aligned_add(
                output.values.split_at_mut(stride),
                a.row(0),
                b.row(0),
                c.row(0),
                d.row(0),
                0.37,
                0.29,
            );
            let (re, im) = output.row(0);
            for i in 0..count {
                assert_eq!(
                    [re[i].to_bits(), im[i].to_bits()],
                    [expected[i].re.to_bits(), expected[i].im.to_bits()]
                );
            }
            assert_eq!(
                output.fingerprint(),
                super::super::resources::hash_complex(&expected)
            );
            for i in 0..count {
                expected[i] += source[i] * phase[i];
            }
            product_add(output.values.split_at_mut(stride), a.row(0), d.row(0));
            assert_eq!(
                output.fingerprint(),
                super::super::resources::hash_complex(&expected)
            );
        }
    }
}
