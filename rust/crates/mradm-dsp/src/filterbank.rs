//! Fixed afSTFT configuration used by the OM spreader: 128-sample hops,
//! ten-hop prototype and seven-hop hybrid history, 133 output bands.
//! Derived from afSTFT, copyright 2015 Juha Vilkamo, MIT. See assets/NOTICE.txt.
use crate::{Complex32 as C, Error, Result, data, fft::RealFft};

pub const HOP: usize = 128;
pub const BANDS: usize = 133;
pub const FRAME: usize = 512;
pub const SLOTS: usize = 4;
pub const DELAY: usize = 1536;
const BINS: usize = 129;
const LENGTH: usize = 1280;

pub struct FilterBank {
    inputs: usize,
    outputs: usize,
    analysis: Vec<f32>,
    synthesis: Vec<f32>,
    history: Vec<C>,
    input_hop: usize,
    output_hop: usize,
    hybrid_hop: usize,
    prototype: [f32; LENGTH],
    time: [f32; 256],
    spectrum: [C; BINS],
    fft: RealFft,
}
impl FilterBank {
    pub fn new(inputs: usize, outputs: usize) -> Result<Self> {
        if inputs > 8 || outputs > 8 {
            return Err(Error::InvalidArgument("Too many filterbank channels"));
        }
        let raw = data::floats(data::AFSTFT_PROTOTYPE);
        let equalization = 2.0f32 / 5.487_604_f32.sqrt();
        let prototype = std::array::from_fn(|i| raw[(LENGTH - i - 1) * 8] * equalization);
        Ok(Self {
            inputs,
            outputs,
            analysis: vec![0.; inputs * LENGTH],
            synthesis: vec![0.; outputs * LENGTH],
            history: vec![C::default(); inputs * 7 * BINS],
            input_hop: 0,
            output_hop: 0,
            hybrid_hop: 0,
            prototype,
            time: [0.; 256],
            spectrum: [C::default(); BINS],
            fft: RealFft::new(256)?,
        })
    }
    pub fn reset(&mut self) {
        self.analysis.fill(0.);
        self.synthesis.fill(0.);
        self.history.fill(C::default());
        self.input_hop = 0;
        self.output_hop = 0;
        self.hybrid_hop = 0;
    }
    /// Input [channel][128], output [band][channel].
    pub fn analysis(&mut self, input: &[f32], output: &mut [C]) -> Result<()> {
        if input.len() != self.inputs * HOP || output.len() != self.inputs * BANDS {
            return Err(Error::InvalidArgument("Filterbank analysis dimensions"));
        }
        self.hybrid_hop = (self.hybrid_hop + 1) % 7;
        let next = (self.input_hop + 1) % 10;
        for ch in 0..self.inputs {
            let buffer = &mut self.analysis[ch * LENGTH..(ch + 1) * LENGTH];
            buffer[self.input_hop * HOP..(self.input_hop + 1) * HOP]
                .copy_from_slice(&input[ch * HOP..(ch + 1) * HOP]);
            self.time.fill(0.);
            for k in 0..10 {
                for i in 0..HOP {
                    self.time[(k % 2) * HOP + i] +=
                        buffer[((next + k) % 10) * HOP + i] * self.prototype[k * HOP + i];
                }
            }
            self.fft.forward(&self.time, &mut self.spectrum)?;
            let history = &mut self.history[ch * 7 * BINS..(ch + 1) * 7 * BINS];
            history[self.hybrid_hop * BINS..(self.hybrid_hop + 1) * BINS]
                .copy_from_slice(&self.spectrum);
            let delay = (self.hybrid_hop + 4) % 7;
            output[ch] = history[delay * BINS];
            for band in 1..5 {
                let center = history[delay * BINS + band] * 0.5;
                let indices: [usize; 7] = std::array::from_fn(|i| (self.hybrid_hop + 1 + i) % 7);
                let difference = history[indices[6] * BINS + band] * 0.031_273_14
                    + history[indices[4] * BINS + band] * 0.281_273_13
                    - history[indices[2] * BINS + band] * 0.281_273_13
                    - history[indices[0] * BINS + band] * 0.031_273_14;
                let shifted = C::new(-difference.im, difference.re);
                let sign = if band % 2 == 1 { -1. } else { 1. };
                output[(2 * band - 1) * self.inputs + ch] = center + shifted * sign;
                output[(2 * band) * self.inputs + ch] = center - shifted * sign;
            }
            for band in 5..BINS {
                output[(band + 4) * self.inputs + ch] = history[delay * BINS + band];
            }
        }
        self.input_hop = next;
        Ok(())
    }
    /// Input [band][channel], output [channel][128].
    pub fn synthesis(&mut self, input: &[C], output: &mut [f32]) -> Result<()> {
        if input.len() != self.outputs * BANDS || output.len() != self.outputs * HOP {
            return Err(Error::InvalidArgument("Filterbank synthesis dimensions"));
        }
        let next = (self.output_hop + 1) % 10;
        for ch in 0..self.outputs {
            self.spectrum[0] = input[ch];
            for band in 1..5 {
                self.spectrum[band] = input[(2 * band - 1) * self.outputs + ch]
                    + input[(2 * band) * self.outputs + ch];
            }
            for band in 5..BINS {
                self.spectrum[band] = input[(band + 4) * self.outputs + ch];
            }
            self.fft.inverse(&self.spectrum, &mut self.time)?;
            let buffer = &mut self.synthesis[ch * LENGTH..(ch + 1) * LENGTH];
            buffer[self.output_hop * HOP..(self.output_hop + 1) * HOP].fill(0.);
            for k in 0..10 {
                for i in 0..HOP {
                    buffer[((next + k) % 10) * HOP + i] +=
                        self.prototype[k * HOP + i] * self.time[(k % 2) * HOP + i];
                }
            }
            output[ch * HOP..(ch + 1) * HOP].copy_from_slice(&buffer[next * HOP..(next + 1) * HOP]);
        }
        self.output_hop = next;
        Ok(())
    }
}

pub fn frequencies(sample_rate: u32) -> [f32; BANDS] {
    let step = sample_rate as f32 / 256.;
    let low = [
        0.,
        0.7501,
        1.2499,
        2. * 0.8751,
        2. * 1.1249,
        3. * 0.9167,
        3. * 1.0833,
        4. * 0.9375,
        4. * 1.0625,
    ];
    std::array::from_fn(|band| {
        step * if band < 9 {
            low[band]
        } else {
            (band - 4) as f32
        }
    })
}

/// HRIR [direction][ear][tap] -> [band][ear][direction]. Each analysis starts
/// from zero state, but plans/scratch are reused for all measurement directions.
pub fn fir_coefficients(ir: &[f32], directions: usize, taps: usize) -> Result<Vec<C>> {
    if directions == 0
        || taps == 0
        || directions.checked_mul(2).and_then(|n| n.checked_mul(taps)) != Some(ir.len())
    {
        return Err(Error::InvalidArgument("Invalid filterbank HRIR dimensions"));
    }
    let slots = (taps.max(HOP) + 1024).div_ceil(HOP);
    let mut delay = 0.;
    for ear in 0..2 {
        let mut maximum = 2.23e-13;
        let mut index = 0;
        for (i, &value) in ir[ear * taps..(ear + 1) * taps].iter().enumerate() {
            if value > maximum {
                maximum = value;
                index = i;
            }
        }
        delay += index as f32;
    }
    let center_index = (delay / 2. + 1.5) as usize;
    let mut center_bank = FilterBank::new(1, 0)?;
    let mut center_input = [0.; HOP];
    let mut center_frame = [C::default(); BANDS];
    let mut center = vec![C::default(); BANDS * slots];
    let mut center_energy = [0.; BANDS];
    for slot in 0..slots {
        center_input.fill(0.);
        if center_index / HOP == slot {
            center_input[center_index % HOP] = 1.;
        }
        center_bank.analysis(&center_input, &mut center_frame)?;
        for band in 0..BANDS {
            let value = center_frame[band];
            center[band * slots + slot] = value;
            center_energy[band] += value.norm_sqr();
        }
    }
    let mut bank = FilterBank::new(2, 0)?;
    let mut input = [0.; HOP * 2];
    let mut frame = [C::default(); BANDS * 2];
    let mut output = vec![C::default(); BANDS * 2 * directions];
    for dir in 0..directions {
        bank.reset();
        let mut energy = [0.; BANDS * 2];
        let mut cross = [C::default(); BANDS * 2];
        for slot in 0..slots {
            input.fill(0.);
            let start = slot * HOP;
            let count = taps.saturating_sub(start).min(HOP);
            if count > 0 {
                for ear in 0..2 {
                    input[ear * HOP..ear * HOP + count].copy_from_slice(
                        &ir[(dir * 2 + ear) * taps + start..(dir * 2 + ear) * taps + start + count],
                    );
                }
            }
            bank.analysis(&input, &mut frame)?;
            for band in 0..BANDS {
                for ear in 0..2 {
                    let index = band * 2 + ear;
                    energy[index] += frame[index].norm_sqr();
                    cross[index] += frame[index] * center[band * slots + slot].conj();
                }
            }
        }
        for (band, center_power) in center_energy.iter().enumerate() {
            for ear in 0..2 {
                let i = band * 2 + ear;
                let gain = (energy[i] / center_power.max(2.23e-8)).sqrt();
                output[i * directions + dir] = C::from_polar(gain, cross[i].arg());
            }
        }
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn identity_has_declared_delay_and_near_perfect_reconstruction() {
        let mut bank = FilterBank::new(1, 1).unwrap();
        let mut spectrum = [C::default(); BANDS];
        let mut output = [0.; HOP];
        let mut all = Vec::new();
        for block in 0..40 {
            let mut input = [0.; HOP];
            if block == 0 {
                input[0] = 1.;
            }
            bank.analysis(&input, &mut spectrum).unwrap();
            bank.synthesis(&spectrum, &mut output).unwrap();
            all.extend_from_slice(&output);
        }
        let peak = (0..all.len())
            .max_by(|&a, &b| all[a].abs().total_cmp(&all[b].abs()))
            .unwrap();
        assert_eq!(peak, DELAY);
        assert!((all[peak] - 1.).abs() < 1e-3);
        let residual: f32 = all
            .iter()
            .enumerate()
            .filter(|(i, _)| *i != DELAY)
            .map(|(_, x)| x * x)
            .sum();
        assert!(residual.sqrt() < 1e-3);
        bank.reset();
        let mut input = [0.; HOP];
        input[0] = 1.;
        bank.analysis(&input, &mut spectrum).unwrap();
        bank.synthesis(&spectrum, &mut output).unwrap();
        assert_eq!(&all[..HOP], &output);
    }
}
