//! Exercise the actual patched layer against RustFFT's unchanged Butterfly4.
//! The reference retains the upstream one-column-at-a-time load/multiply order.
use rustfft::{
    Fft, FftDirection, FftNum, algorithm::butterflies::Butterfly4, num_complex::Complex,
};

#[path = "../../../vendor/rustfft/src/algorithm/radix4_columns.rs"]
mod columns;

fn reference<T: FftNum>(data: &mut [Complex<T>], twiddles: &[Complex<T>], direction: FftDirection) {
    let width = data.len() / 4;
    let butterfly = Butterfly4::new(direction);
    for i in 0..width {
        let mut values = [
            data[i],
            data[i + width] * twiddles[i * 3],
            data[i + width * 2] * twiddles[i * 3 + 1],
            data[i + width * 3] * twiddles[i * 3 + 2],
        ];
        butterfly.process_with_scratch(&mut values, &mut []);
        for (row, value) in values.into_iter().enumerate() {
            data[i + row * width] = value;
        }
    }
}

fn next(state: &mut u32) -> u32 {
    *state = state.wrapping_mul(1664525).wrapping_add(1013904223);
    *state
}

macro_rules! bit_test {
    ($name:ident, $float:ty, $bits:expr) => {
        #[test]
        fn $name() {
            let mut seed = 91;
            for width in [1, 2, 3, 4, 5, 7, 8, 9, 16, 32, 64, 128, 256, 1024, 8192] {
                for direction in [FftDirection::Forward, FftDirection::Inverse] {
                    for pattern in 0..12 {
                        let mut value = || {
                            let r = next(&mut seed);
                            let x = match pattern {
                                0 => 0.0,
                                1 => -0.0,
                                2 => {
                                    if r >> 31 == 0 {
                                        0.0
                                    } else {
                                        -0.0
                                    }
                                }
                                3 => {
                                    if r >> 31 == 0 {
                                        1.0
                                    } else {
                                        -1.0
                                    }
                                }
                                _ => (r as i32 as f64) / 2147483648.0,
                            } as $float;
                            // Include cancellation, underflow and a broad exponent range.
                            match pattern {
                                4 => x * <$float>::MIN_POSITIVE,
                                5 => x * (1_u64 << 32) as $float,
                                6 => x / (1_u64 << 32) as $float,
                                _ => x,
                            }
                        };
                        let mut input: Vec<_> = (0..width * 4)
                            .map(|_| Complex::new(value(), value()))
                            .collect();
                        let twiddles: Vec<_> = (0..width * 3)
                            .map(|_| Complex::new(value(), value()))
                            .collect();
                        let mut expected = input.clone();
                        reference(&mut expected, &twiddles, direction);
                        let layer = columns::Layer::new(&twiddles);
                        columns::cross_fft(&mut input, &layer, direction);
                        for (i, (actual, expected)) in input.iter().zip(expected).enumerate() {
                            assert!(actual.re.is_finite() && actual.im.is_finite());
                            assert_eq!(
                                ($bits(actual.re), $bits(actual.im)),
                                ($bits(expected.re), $bits(expected.im)),
                                "width={width}, direction={direction:?}, pattern={pattern}, bin={i}"
                            );
                        }
                    }
                }
            }
        }
    };
}

bit_test!(f32_columns_preserve_scalar_bits, f32, f32::to_bits);
bit_test!(f64_columns_preserve_scalar_bits, f64, f64::to_bits);
