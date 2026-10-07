#![forbid(unsafe_code)]
// Preserve this patch's Rust 1.63 MSRV; as_chunks_mut stabilized in 1.88.
#![allow(clippy::chunks_exact_to_as_chunks)]

use super::{Complex, FftDirection, FftNum};

// Four independent columns, not four partial sums of one column. Keeping real
// and imaginary lanes separate lets LLVM use SSE/NEON without changing any
// column's expression tree. No fast-math or fused multiply-add is used.
#[derive(Clone, Copy)]
struct Columns4<T> {
    re: [T; 4],
    im: [T; 4],
}
impl<T: FftNum> Columns4<T> {
    #[inline(always)]
    fn load(values: &[Complex<T>]) -> Self {
        Self::gather(|i| values[i])
    }
    #[inline(always)]
    fn gather(value: impl FnMut(usize) -> Complex<T>) -> Self {
        let v: [_; 4] = std::array::from_fn(value);
        Self {
            re: v.map(|x| x.re),
            im: v.map(|x| x.im),
        }
    }
    #[inline(always)]
    fn add(self, other: Self) -> Self {
        Self {
            re: std::array::from_fn(|i| self.re[i] + other.re[i]),
            im: std::array::from_fn(|i| self.im[i] + other.im[i]),
        }
    }
    #[inline(always)]
    fn sub(self, other: Self) -> Self {
        Self {
            re: std::array::from_fn(|i| self.re[i] - other.re[i]),
            im: std::array::from_fn(|i| self.im[i] - other.im[i]),
        }
    }
    #[inline(always)]
    fn mul(self, other: Self) -> Self {
        Self {
            re: std::array::from_fn(|i| self.re[i] * other.re[i] - self.im[i] * other.im[i]),
            im: std::array::from_fn(|i| self.re[i] * other.im[i] + self.im[i] * other.re[i]),
        }
    }
    #[inline(always)]
    fn rotate(self, direction: FftDirection) -> Self {
        match direction {
            FftDirection::Forward => Self {
                re: self.im,
                im: self.re.map(|x| -x),
            },
            FftDirection::Inverse => Self {
                re: self.im.map(|x| -x),
                im: self.re,
            },
        }
    }
    #[inline(always)]
    // Keep the fixed four-lane store visible to LLVM; iterator rewrites need a
    // performance comparison as well as the raw-bit tests.
    #[allow(clippy::needless_range_loop)]
    fn store(self, values: &mut [Complex<T>]) {
        for i in 0..4 {
            values[i] = Complex::new(self.re[i], self.im[i]);
        }
    }
}

// Prepared coefficient lanes have the exact original bits; only their layout
// changes. Each group contains four columns of each of the three multipliers.
pub(super) struct Layer<T> {
    pub(super) columns: usize,
    groups: Box<[[Columns4<T>; 3]]>,
    tail: Box<[Complex<T>]>,
}
impl<T: FftNum> Layer<T> {
    pub(super) fn new(twiddles: &[Complex<T>]) -> Self {
        assert_eq!(twiddles.len() % 3, 0);
        let mut chunks = twiddles.chunks_exact(12);
        let groups = chunks
            .by_ref()
            .map(|t| std::array::from_fn(|k| Columns4::gather(|i| t[i * 3 + k])))
            .collect::<Vec<_>>()
            .into_boxed_slice();
        Self {
            columns: twiddles.len() / 3,
            groups,
            tail: chunks.remainder().into(),
        }
    }
}

#[inline(never)]
pub(super) fn cross_fft<T: FftNum>(
    data: &mut [Complex<T>],
    layer: &Layer<T>,
    direction: FftDirection,
) {
    let columns = layer.columns;
    let (row0, rest) = data.split_at_mut(columns);
    let (row1, rest) = rest.split_at_mut(columns);
    let (row2, row3) = rest.split_at_mut(columns);
    let packed = columns / 4 * 4;
    let rows = row0[..packed]
        .chunks_exact_mut(4)
        .zip(row1[..packed].chunks_exact_mut(4))
        .zip(row2[..packed].chunks_exact_mut(4))
        .zip(row3[..packed].chunks_exact_mut(4));
    for ((((a, b), c), d), t) in rows.zip(layer.groups.iter()) {
        let x0 = Columns4::load(a);
        let x1 = Columns4::load(b).mul(t[0]);
        let x2 = Columns4::load(c).mul(t[1]);
        let x3 = Columns4::load(d).mul(t[2]);
        let sum02 = x0.add(x2);
        let diff02 = x0.sub(x2);
        let sum13 = x1.add(x3);
        let diff13 = x1.sub(x3).rotate(direction);
        sum02.add(sum13).store(a);
        diff02.add(diff13).store(b);
        sum02.sub(sum13).store(c);
        diff02.sub(diff13).store(d);
    }
    // new_with_base also supports base lengths that are not multiples of four.
    for i in packed..columns {
        let x0 = row0[i];
        let x1 = row1[i] * layer.tail[(i - packed) * 3];
        let x2 = row2[i] * layer.tail[(i - packed) * 3 + 1];
        let x3 = row3[i] * layer.tail[(i - packed) * 3 + 2];
        let sum02 = x0 + x2;
        let diff02 = x0 - x2;
        let sum13 = x1 + x3;
        let difference = x1 - x3;
        let diff13 = match direction {
            FftDirection::Forward => Complex::new(difference.im, -difference.re),
            FftDirection::Inverse => Complex::new(-difference.im, difference.re),
        };
        row0[i] = sum02 + sum13;
        row1[i] = diff02 + diff13;
        row2[i] = sum02 - sum13;
        row3[i] = diff02 - diff13;
    }
}
