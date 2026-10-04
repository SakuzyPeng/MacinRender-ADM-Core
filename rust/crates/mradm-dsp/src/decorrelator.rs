//! Stateful two-ear lattice allpass decorrelator, adapted from SAF
//! (Leo McCormack, ISC; see assets/NOTICE.txt), with per-source PCG state.
use crate::{
    Complex32 as C, data,
    filterbank::{BANDS, HOP, SLOTS},
    rng::Pcg32,
};
struct Band {
    coefficients: [f32; 20],
    order: usize,
    memory: [C; 20],
    delay: Vec<C>,
    read: usize,
    write: usize,
    input_energy: f32,
    output_energy: f32,
}
pub struct Decorrelator {
    bands: Vec<Band>,
}
impl Decorrelator {
    pub fn new(rate: u32, frequencies: &[f32; BANDS], seed: u64) -> Self {
        let mut rng = Pcg32::new(seed, 0x4445434f52);
        let c20 = data::floats(data::LATTICE_20);
        let c15 = data::floats(data::LATTICE_15);
        let c6 = data::floats(data::LATTICE_6);
        let maximum_ms = 80.0f32.min(11. * HOP as f32 / rate as f32 * 1000.);
        let mut bands = Vec::with_capacity(BANDS * 2);
        for &frequency in frequencies {
            let high = 7.0f32.max(maximum_ms.min(50000. / (frequency + 2.23e-9)));
            let low = 3.0f32.max(20.0f32.min(10000. / (frequency + 2.23e-9)));
            let mut fractions = [rng.unit() * 0.5, 0.5 + rng.unit() * 0.5];
            if rng.bounded(2) != 0 {
                fractions.swap(0, 1);
            }
            for (ear, fraction) in fractions.iter().enumerate() {
                let delay = (((fraction * (high - low) + low) / 1000. * rate as f32 / HOP as f32
                    + 0.5) as usize)
                    .saturating_sub(1);
                let (order, table) = if frequency < 900. {
                    (20, &c20)
                } else if frequency < 6800. {
                    (15, &c15)
                } else if frequency < 24000. {
                    (6, &c6)
                } else {
                    (0, &c6)
                };
                let mut coefficients = [0.; 20];
                coefficients[..order].copy_from_slice(&table[ear * order..(ear + 1) * order]);
                bands.push(Band {
                    coefficients,
                    order,
                    memory: [C::default(); 20],
                    delay: vec![C::default(); delay + 1],
                    read: 0,
                    write: delay,
                    input_energy: 0.,
                    output_energy: 0.,
                });
            }
        }
        Self { bands }
    }
    pub fn apply(&mut self, input: &[C], output: &mut [C]) {
        assert_eq!(input.len(), BANDS * 2 * SLOTS);
        assert_eq!(output.len(), input.len());
        for (band, state) in self.bands.iter_mut().enumerate() {
            for t in 0..SLOTS {
                let index = band * SLOTS + t;
                let original = input[index];
                state.delay[state.write] = original;
                let x = state.delay[state.read];
                state.write = (state.write + 1) % state.delay.len();
                state.read = (state.read + 1) % state.delay.len();
                if state.order == 0 {
                    output[index] = x;
                    continue;
                }
                state.input_energy = 0.25 * original.norm_sqr() + 0.75 * state.input_energy;
                let y = x * state.coefficients[0] + state.memory[0];
                state.output_energy = 0.25 * y.norm_sqr() + 0.75 * state.output_energy;
                output[index] = y
                    * (state.input_energy / (state.output_energy + 2.23e-9))
                        .sqrt()
                        .min(1.);
                for i in 0..state.order - 1 {
                    state.memory[i] = state.memory[i + 1] + x * state.coefficients[i + 1]
                        - y * state.coefficients[state.order - i - 2];
                }
            }
        }
    }
}
