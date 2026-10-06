//! Scene worker transitions; source-rate backend fades and output-rate generation declicks.
use crate::{Error, Result};
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct Status {
    pub backend_position: u64,
    pub generation_position: u64,
    pub generation_remaining: u64,
}
#[derive(Clone)]
pub struct Transitions {
    channels: usize,
    backend_frames: u64,
    generation_frames: u64,
    status: Status,
    last: Vec<f32>,
    anchor: Vec<f32>,
}
fn invalid() -> Error {
    Error::InvalidArgument("Invalid Scene transition buffers or counters")
}
impl Transitions {
    pub fn new(channels: usize, rate: u32, backend_frames: u64) -> Result<Self> {
        if channels == 0 || channels > isize::MAX as usize / 8 || rate == 0 || backend_frames == 0 {
            return Err(invalid());
        }
        Ok(Self {
            channels,
            backend_frames,
            generation_frames: (u64::from(rate) * 10 / 1000).max(1),
            status: Status::default(),
            last: vec![0.; channels],
            anchor: vec![0.; channels],
        })
    }
    pub fn status(&self) -> Status {
        self.status
    }
    pub fn reset(&mut self) {
        self.status = Status::default();
        self.last.fill(0.);
        self.anchor.fill(0.);
    }
    pub fn reset_backend(&mut self) {
        self.status.backend_position = 0;
    }
    pub fn begin_generation(&mut self) {
        self.anchor.copy_from_slice(&self.last);
        self.status.generation_position = 0;
        self.status.generation_remaining = self.generation_frames;
    }
    fn samples(&self, frames: usize, length: usize) -> Result<usize> {
        let n = frames.checked_mul(self.channels).ok_or_else(invalid)?;
        if n > length || !length.is_multiple_of(self.channels) {
            return Err(invalid());
        }
        Ok(n)
    }
    pub fn mix(&mut self, old: &mut [f32], new: &[f32], frames: usize) -> Result<bool> {
        let n = self.samples(frames, old.len())?;
        self.samples(frames, new.len())?;
        let end = self
            .status
            .backend_position
            .checked_add(frames as u64)
            .ok_or_else(invalid)?;
        for (old, new) in old[..n]
            .chunks_exact_mut(self.channels)
            .zip(new[..n].chunks_exact(self.channels))
        {
            let position = (self.status.backend_position + 1).min(self.backend_frames);
            let incoming = position as f32 / self.backend_frames as f32;
            let outgoing = 1. - incoming;
            for (a, b) in old.iter_mut().zip(new) {
                *a = (*a * outgoing) + (*b * incoming);
            }
            self.status.backend_position += 1;
        }
        debug_assert_eq!(self.status.backend_position, end);
        Ok(self.status.backend_position >= self.backend_frames)
    }
    pub fn process_output(
        &mut self,
        pcm: &mut [f32],
        frames: usize,
        force_silence: bool,
    ) -> Result<()> {
        let n = self.samples(frames, pcm.len())?;
        self.status
            .generation_position
            .checked_add(self.status.generation_remaining)
            .ok_or_else(invalid)?;
        for frame in pcm[..n].chunks_exact_mut(self.channels) {
            if force_silence {
                if self.status.generation_remaining > 0 {
                    self.status.generation_position += 1;
                    self.status.generation_remaining -= 1;
                }
                frame.fill(0.);
                self.last.fill(0.);
                continue;
            }
            if self.status.generation_remaining > 0 {
                let total = self.status.generation_position + self.status.generation_remaining;
                let alpha = (self.status.generation_position + 1) as f32 / total as f32;
                for (value, anchor) in frame.iter_mut().zip(&self.anchor) {
                    *value = (*anchor * (1. - alpha)) + (*value * alpha);
                }
                self.status.generation_position += 1;
                self.status.generation_remaining -= 1;
            }
            self.last.copy_from_slice(frame);
        }
        Ok(())
    }
    pub fn snapshot(&self, last: &mut [f32], anchor: &mut [f32]) -> Result<Status> {
        if last.len() != self.channels || anchor.len() != self.channels {
            return Err(invalid());
        }
        last.copy_from_slice(&self.last);
        anchor.copy_from_slice(&self.anchor);
        Ok(self.status)
    }
}
