//! Binaural kernels with prepared storage. Live overlap-save and batch
//! overlap-add deliberately retain their different filter-transition contracts.
//! Spectra are [L.re, L.im, R.re, R.im] per bin, independent of host complex ABI.
use crate::{Complex32, Error, Result, fft::RealFft};

fn validate_spectrum(hrtf: &[f32], bins: usize) -> Result<()> {
    if hrtf.len() != bins * 4 || hrtf.iter().any(|v| !v.is_finite()) {
        return Err(Error::InvalidArgument("Invalid binaural spectrum"));
    }
    Ok(())
}

fn bin(hrtf: &[f32], band: usize, ear: usize) -> Complex32 {
    let i = band * 4 + ear * 2;
    Complex32::new(hrtf[i], hrtf[i + 1])
}

fn validate_audio(input: &[f32], left: &[f32], right: &[f32], maximum: usize) -> Result<()> {
    if input.len() > maximum || left.len() != input.len() || right.len() != input.len() {
        return Err(Error::InvalidArgument(
            "Invalid binaural audio buffer lengths",
        ));
    }
    if input.iter().any(|v| !v.is_finite()) {
        return Err(Error::InvalidArgument("Binaural input must be finite"));
    }
    Ok(())
}

/// Per-element input history and audible filter. No references to the workspace;
/// states may outlive it and may be reused with an identically configured one.
pub struct LiveState {
    history: Vec<f32>,
    current_filter: Vec<Complex32>,
    target_filter: Vec<Complex32>,
    target_hrtf: Vec<f32>,
    maximum_frames: usize,
    fade_frames: usize,
    fade_remaining: usize,
    tail_remaining: usize,
    initialized: bool,
}

impl LiveState {
    pub fn initialized(&self) -> bool {
        self.initialized
    }
    pub fn tail_remaining(&self) -> usize {
        self.tail_remaining
    }

    /// Discard history and controls without releasing prepared storage.
    pub fn reset(&mut self) {
        self.history.fill(0.0);
        self.fade_remaining = 0;
        self.tail_remaining = 0;
        self.initialized = false;
    }
}

/// Shared serial workspace; each source keeps its own LiveState.
pub struct LiveConvolver {
    hrtf_fft: RealFft,
    fft: RealFft,
    maximum_frames: usize,
    fade_frames: usize,
    hrtf_ear: Vec<Complex32>,
    impulse: Vec<f32>,
    input: Vec<f32>,
    source_fd: Vec<Complex32>,
    output_fd: Vec<Complex32>,
    output: Vec<f32>,
}

impl LiveConvolver {
    pub fn new(hrtf_length: usize, maximum_frames: usize, sample_rate: u32) -> Result<Self> {
        if maximum_frames == 0 || sample_rate == 0 {
            return Err(Error::InvalidArgument(
                "Invalid live convolution configuration",
            ));
        }
        let fft_length = hrtf_length
            .checked_add(maximum_frames - 1)
            .and_then(usize::checked_next_power_of_two)
            .ok_or(Error::InvalidArgument("Convolution size overflow"))?;
        // Check both lengths before allocating either plan.
        if hrtf_length < 2 || !hrtf_length.is_power_of_two() || fft_length > (1 << 24) {
            return Err(Error::InvalidArgument(
                "Invalid live convolution FFT lengths",
            ));
        }
        let hrtf_fft = RealFft::new(hrtf_length)?;
        let fft = RealFft::new(fft_length)?;
        let bins = fft.bins();
        Ok(Self {
            hrtf_ear: vec![Complex32::default(); hrtf_fft.bins()],
            hrtf_fft,
            fft,
            maximum_frames,
            fade_frames: (sample_rate as usize / 100).max(1),
            impulse: vec![0.0; fft_length],
            input: vec![0.0; fft_length],
            source_fd: vec![Complex32::default(); bins],
            output_fd: vec![Complex32::default(); bins],
            output: vec![0.0; fft_length],
        })
    }

    pub fn spectrum_len(&self) -> usize {
        self.hrtf_fft.bins() * 4
    }
    pub fn maximum_frames(&self) -> usize {
        self.maximum_frames
    }
    pub fn tail_frames(&self) -> usize {
        self.hrtf_fft.len() - 1
    }

    pub fn make_state(&self) -> LiveState {
        LiveState {
            history: vec![0.0; self.tail_frames()],
            current_filter: vec![Complex32::default(); self.fft.bins() * 2],
            target_filter: vec![Complex32::default(); self.fft.bins() * 2],
            target_hrtf: vec![0.0; self.spectrum_len()],
            maximum_frames: self.maximum_frames,
            fade_frames: self.fade_frames,
            fade_remaining: 0,
            tail_remaining: 0,
            initialized: false,
        }
    }

    fn validate_state(&self, state: &LiveState) -> Result<()> {
        if state.history.len() != self.tail_frames()
            || state.maximum_frames != self.maximum_frames
            || state.fade_frames != self.fade_frames
        {
            return Err(Error::InvalidArgument(
                "Incompatible live convolution state",
            ));
        }
        Ok(())
    }

    pub fn initialize(&mut self, state: &mut LiveState, hrtf: &[f32]) -> Result<()> {
        self.validate_state(state)?;
        validate_spectrum(hrtf, self.hrtf_fft.bins())?;
        self.initialize_inner(state, hrtf)
    }

    fn initialize_inner(&mut self, state: &mut LiveState, hrtf: &[f32]) -> Result<()> {
        self.expand_filter(hrtf, &mut state.target_filter)?;
        state.target_hrtf.copy_from_slice(hrtf);
        state.current_filter.copy_from_slice(&state.target_filter);
        state.fade_remaining = 0;
        state.initialized = true;
        Ok(())
    }

    fn expand_filter(&mut self, hrtf: &[f32], output: &mut [Complex32]) -> Result<()> {
        // Magnitude/phase interpolation can occupy every inverse-FFT tap, even
        // beyond the measured HRIR length. Preserve the complete causal FIR.
        for ear in 0..2 {
            for (band, value) in self.hrtf_ear.iter_mut().enumerate() {
                *value = bin(hrtf, band, ear);
            }
            self.impulse.fill(0.0);
            self.hrtf_fft
                .inverse(&self.hrtf_ear, &mut self.impulse[..self.hrtf_fft.len()])?;
            self.fft.forward(&self.impulse, &mut self.output_fd)?;
            for (band, value) in self.output_fd.iter().enumerate() {
                output[band * 2 + ear] = *value;
            }
        }
        Ok(())
    }

    fn filter_ear(&mut self, filter: &[Complex32], ear: usize) -> Result<()> {
        for (band, value) in self.output_fd.iter_mut().enumerate() {
            *value = self.source_fd[band] * filter[band * 2 + ear];
        }
        self.fft.inverse(&self.output_fd, &mut self.output)
    }

    /// Overwrite both outputs. Ramps reach their endpoint at the next segment;
    /// discontinuous targets use a persistent 10 ms fade from the audible filter.
    pub fn process(
        &mut self,
        state: &mut LiveState,
        hrtf: &[f32],
        input: &[f32],
        left: &mut [f32],
        right: &mut [f32],
        follows_ramp: bool,
    ) -> Result<()> {
        self.validate_state(state)?;
        validate_spectrum(hrtf, self.hrtf_fft.bins())?;
        validate_audio(input, left, right, self.maximum_frames)?;
        if input.is_empty() {
            return Ok(());
        }
        if !state.initialized {
            self.initialize_inner(state, hrtf)?;
        } else if hrtf != state.target_hrtf {
            self.expand_filter(hrtf, &mut state.target_filter)?;
            state.target_hrtf.copy_from_slice(hrtf);
            state.fade_remaining = if follows_ramp {
                input.len()
            } else {
                self.fade_frames
            };
        }
        let history = state.history.len();
        self.input.fill(0.0);
        self.input[..history].copy_from_slice(&state.history);
        self.input[history..history + input.len()].copy_from_slice(input);
        self.fft.forward(&self.input, &mut self.source_fd)?;
        for (ear, destination) in [left, right].into_iter().enumerate() {
            self.filter_ear(&state.current_filter, ear)?;
            destination.copy_from_slice(&self.output[history..history + input.len()]);
            if state.fade_remaining != 0 {
                self.filter_ear(&state.target_filter, ear)?;
                for (i, value) in destination.iter_mut().enumerate() {
                    let alpha = (i as f32 / state.fade_remaining as f32).min(1.0);
                    *value += alpha * (self.output[history + i] - *value);
                }
            }
        }
        if state.fade_remaining != 0 {
            let advanced = input.len().min(state.fade_remaining);
            let alpha = advanced as f32 / state.fade_remaining as f32;
            for (current, target) in state.current_filter.iter_mut().zip(&state.target_filter) {
                *current += alpha * (*target - *current);
            }
            state.fade_remaining -= advanced;
            if state.fade_remaining == 0 {
                state.current_filter.copy_from_slice(&state.target_filter);
            }
        }
        state
            .history
            .copy_from_slice(&self.input[input.len()..input.len() + history]);
        state.tail_remaining = if input.iter().any(|v| *v != 0.0) {
            self.tail_frames()
        } else {
            state.tail_remaining.saturating_sub(input.len())
        };
        Ok(())
    }
}

/// Batch/legacy stream OLA. Both paths share this owner, including overlap,
/// silence advancement and endpoint-inclusive block crossfades. The declared
/// overlap remains the measured HRIR tail, as in the existing batch renderer.
pub struct OlaConvolver {
    fft: RealFft,
    maximum_frames: usize,
    overlap: [Vec<f32>; 2],
    input: Vec<f32>,
    source_fd: Vec<Complex32>,
    output_fd: Vec<Complex32>,
    output: Vec<f32>,
    start_output: Vec<f32>,
}

impl OlaConvolver {
    pub fn new(fft_length: usize, overlap: usize, maximum_frames: usize) -> Result<Self> {
        if maximum_frames == 0 || overlap >= fft_length || maximum_frames > fft_length - overlap {
            return Err(Error::InvalidArgument(
                "Invalid OLA convolution configuration",
            ));
        }
        let fft = RealFft::new(fft_length)?;
        let bins = fft.bins();
        Ok(Self {
            fft,
            maximum_frames,
            overlap: std::array::from_fn(|_| vec![0.0; overlap]),
            input: vec![0.0; fft_length],
            source_fd: vec![Complex32::default(); bins],
            output_fd: vec![Complex32::default(); bins],
            output: vec![0.0; fft_length],
            start_output: vec![0.0; maximum_frames],
        })
    }
    pub fn spectrum_len(&self) -> usize {
        self.fft.bins() * 4
    }
    pub fn maximum_frames(&self) -> usize {
        self.maximum_frames
    }
    pub fn reset(&mut self) {
        for ear in &mut self.overlap {
            ear.fill(0.0);
        }
    }

    fn filter_ear(&mut self, hrtf: &[f32], gain: f32, ear: usize) -> Result<()> {
        for (band, value) in self.output_fd.iter_mut().enumerate() {
            // Preserve the batch gain-before-filter multiplication order.
            *value = (gain * self.source_fd[band]) * bin(hrtf, band, ear);
        }
        self.fft.inverse(&self.output_fd, &mut self.output)
    }

    /// Accumulate into both outputs. A crossfade shares the incoming overlap,
    /// then retains the end arm's tail (also for a one-sample segment).
    #[allow(clippy::too_many_arguments)]
    pub fn process(
        &mut self,
        input: &[f32],
        start_hrtf: &[f32],
        start_gain: f32,
        end: Option<(&[f32], f32)>,
        left: &mut [f32],
        right: &mut [f32],
    ) -> Result<()> {
        validate_audio(input, left, right, self.maximum_frames)?;
        validate_spectrum(start_hrtf, self.fft.bins())?;
        if !start_gain.is_finite() || end.is_some_and(|(_, gain)| !gain.is_finite()) {
            return Err(Error::InvalidArgument("Convolution gain must be finite"));
        }
        if let Some((hrtf, _)) = end {
            validate_spectrum(hrtf, self.fft.bins())?;
        }
        let frames = input.len();
        if frames == 0 {
            return Ok(());
        }
        self.input.fill(0.0);
        self.input[..frames].copy_from_slice(input);
        self.fft.forward(&self.input, &mut self.source_fd)?;
        for (ear, destination) in [left, right].into_iter().enumerate() {
            self.filter_ear(start_hrtf, start_gain, ear)?;
            if let Some((hrtf, gain)) = end {
                for (i, value) in self.start_output[..frames].iter_mut().enumerate() {
                    *value = self.output[i] + self.overlap[ear].get(i).copied().unwrap_or(0.0);
                }
                self.filter_ear(hrtf, gain, ear)?;
            }
            for (i, value) in destination.iter_mut().enumerate() {
                let sample = self.output[i] + self.overlap[ear].get(i).copied().unwrap_or(0.0);
                *value += if end.is_some() {
                    let alpha = if frames > 1 {
                        i as f32 / (frames - 1) as f32
                    } else {
                        0.0
                    };
                    self.start_output[i] * (1.0 - alpha) + sample * alpha
                } else {
                    sample
                };
            }
            let overlap = &mut self.overlap[ear];
            for i in 0..overlap.len() {
                let residual = overlap.get(frames + i).copied().unwrap_or(0.0);
                overlap[i] = self.output[frames + i] + residual;
            }
        }
        Ok(())
    }

    pub fn advance_silence(&mut self, left: &mut [f32], right: &mut [f32]) -> Result<()> {
        if left.len() != right.len() || left.len() > self.maximum_frames {
            return Err(Error::InvalidArgument("Invalid OLA silence buffers"));
        }
        for (overlap, destination) in self.overlap.iter_mut().zip([left, right]) {
            let frames = destination.len();
            for (value, tail) in destination.iter_mut().zip(overlap.iter()) {
                *value += tail;
            }
            let advance = frames.min(overlap.len());
            overlap.copy_within(advance.., 0);
            let remaining = overlap.len() - advance;
            overlap[remaining..].fill(0.0);
        }
        Ok(())
    }
}
