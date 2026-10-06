//! libear random-phase FIR design (Apache-2.0). MT19937 seed/conversion are exact.
use crate::Layout;
use rustfft::{FftPlanner, num_complex::Complex};
pub const TAPS: usize = 512;
pub const DELAY: usize = 255;
struct Mt19937 {
    state: [u32; 624],
    index: usize,
}
impl Mt19937 {
    fn new(seed: u32) -> Self {
        let mut state = [0; 624];
        state[0] = seed;
        for i in 1..624 {
            state[i] = 1812433253u32
                .wrapping_mul(state[i - 1] ^ (state[i - 1] >> 30))
                .wrapping_add(i as u32);
        }
        Self { state, index: 624 }
    }
    fn next(&mut self) -> u32 {
        if self.index == 624 {
            for i in 0..624 {
                let x = (self.state[i] & 0x80000000) | (self.state[(i + 1) % 624] & 0x7fffffff);
                self.state[i] = self.state[(i + 397) % 624]
                    ^ (x >> 1)
                    ^ if x & 1 == 0 { 0 } else { 0x9908b0df };
            }
            self.index = 0;
        }
        let mut y = self.state[self.index];
        self.index += 1;
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680;
        y ^= (y << 15) & 0xefc60000;
        y ^= y >> 18;
        y
    }
}
pub fn filters(layout: &Layout) -> Vec<f32> {
    let fft = FftPlanner::<f64>::new().plan_fft_inverse(TAPS);
    let mut out = Vec::with_capacity(layout.channels.len() * TAPS);
    for c in &layout.channels {
        let seed = layout
            .channels
            .iter()
            .filter(|other| other.name < c.name)
            .count() as u32;
        let mut rng = Mt19937::new(seed);
        let mut spectrum = vec![Complex::new(0.0, 0.0); TAPS];
        spectrum[0] = Complex::new(1.0, 0.0);
        spectrum[TAPS / 2] = Complex::new(1.0, 0.0);
        for i in 1..TAPS / 2 {
            let angle = std::f64::consts::TAU * (rng.next() as f64 / 4294967296.0);
            spectrum[i] = Complex::new(angle.cos(), angle.sin());
            spectrum[TAPS - i] = spectrum[i].conj();
        }
        fft.process(&mut spectrum);
        out.extend(spectrum.iter().map(|v| (v.re / TAPS as f64) as f32));
    }
    out
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn standard_mt_sequence() {
        let mut r = Mt19937::new(5489);
        for v in [3499211612, 581869302, 3890346734, 3586334585, 545404204] {
            assert_eq!(r.next(), v);
        }
    }
}
