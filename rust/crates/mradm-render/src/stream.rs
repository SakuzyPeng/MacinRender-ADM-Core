//! File streaming over a prepared plan (PCM consistency 4.5), moved from the C++ `VbapStream`
//! and `HoaStream`: blocks of `max(1024, smoothing)` frames go through the same `PcmMixer` or HOA
//! encoder as the offline driver into a FIFO served at any frame count, so a gap-free run from
//! frame 0 is bit-identical to the offline render. Live gain pre-scales each input channel (the
//! render is linear) through a 20 ms de-zipper; `seek` resets the mixer cursors or the HOA
//! diffuse delay lines but not the de-zipper.
use crate::offline::{Driver, Engine, Error, HOA_SHORT_READ, Kernel, Location, Result};
use crate::routing::{self, Override};
use crate::vbap::Binding;
use mradm_dsp::gain::GainBank;
use mradm_wav::Reader;
use std::fs::File;
use std::io::BufReader;
use std::sync::Arc;

pub struct Settings {
    pub inputs: usize,
    pub outputs: usize,
    pub frames: u64,
    pub sample_rate: u32,
    pub default_interp_ms: u32,
    pub smoothing_frames: u64,
}

pub struct FileStream {
    reader: Reader<BufReader<File>>,
    engine: Engine,
    bindings: Arc<[Binding]>,
    inputs: usize,
    outputs: usize,
    frames: u64,
    block_size: u64,
    input: Vec<f32>,
    fifo: Vec<f32>,
    fifo_len: usize,
    fifo_read: usize,
    done: u64,
    targets: Vec<f32>,
    smoother: GainBank,
}

fn failed(e: mradm_dsp::Error) -> Error {
    Error::dsp(&e, Location::None)
}

impl FileStream {
    /// Opens `path` as the offline driver does; reader errors carry `Location::Path`.
    pub fn open(
        path: &str,
        kernel: &Kernel,
        bindings: Arc<[Binding]>,
        s: Settings,
    ) -> Result<Self> {
        let reader = Driver::open_reader(path)?;
        let block_size = s.smoothing_frames.max(1024);
        let default_interp = u64::from(s.sample_rate) * u64::from(s.default_interp_ms) / 1000;
        let engine = Engine::new(kernel, block_size, default_interp, s.smoothing_frames > 0)
            .map_err(|e| {
                Error::dsp(
                    &e,
                    if matches!(kernel, Kernel::Hoa(_)) {
                        Location::HoaDsp
                    } else {
                        Location::None
                    },
                )
            })?;
        Ok(Self {
            reader,
            engine,
            bindings,
            inputs: s.inputs,
            outputs: s.outputs,
            frames: s.frames,
            block_size,
            input: vec![0.; s.inputs * block_size as usize],
            fifo: vec![0.; s.outputs * block_size as usize],
            fifo_len: 0,
            fifo_read: 0,
            done: 0,
            targets: vec![1.; s.inputs],
            smoother: GainBank::new(s.inputs, s.sample_rate, 20).map_err(failed)?,
        })
    }

    /// Fills `output` (interleaved) with up to `frames` frames; fewer only at the end.
    pub fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize> {
        if output.len() < frames.saturating_mul(self.outputs) {
            return Err(failed(mradm_dsp::Error::InvalidArgument(
                if self.engine.is_hoa() {
                    "HOA output buffer is too small"
                } else {
                    "Invalid stream output buffer"
                },
            )));
        }
        let mut produced = 0;
        while produced < frames {
            if self.fifo_read >= self.fifo_len {
                if self.done >= self.frames {
                    break;
                }
                self.render_block()?;
                if self.fifo_read >= self.fifo_len {
                    break;
                }
            }
            let available = (self.fifo_len - self.fifo_read) / self.outputs;
            let take = (frames - produced).min(available);
            let n = take * self.outputs;
            output[produced * self.outputs..][..n]
                .copy_from_slice(&self.fifo[self.fifo_read..self.fifo_read + n]);
            self.fifo_read += n;
            produced += take;
        }
        Ok(produced)
    }

    pub fn seek(&mut self, frame: u64) -> Result<()> {
        self.done = frame.min(self.frames);
        self.engine.reset(self.done);
        self.reader.seek_frame(self.done).map_err(Error::wav)?;
        self.fifo_len = 0;
        self.fifo_read = 0;
        Ok(())
    }

    /// Each input channel follows its owning object's override; a DirectSpeakers channel also
    /// matches by its speaker label.
    pub fn set_overrides(&mut self, overrides: &[Override]) -> Result<()> {
        self.targets.fill(1.);
        for b in self.bindings.iter() {
            let channel = usize::from(b.input_channel);
            if channel >= self.targets.len() {
                continue;
            }
            if let Some(g) = routing::live_channel_gain(overrides, &b.object_id, &b.label_key) {
                self.targets[channel] = g;
            }
        }
        self.smoother.set_targets(&self.targets).map_err(failed)
    }

    fn render_block(&mut self) -> Result<()> {
        let frames = self.block_size.min(self.frames - self.done) as usize;
        let input = &mut self.input[..frames * self.inputs];
        let read = self.reader.read_frames(input).map_err(Error::wav)?;
        if read != frames as u64 {
            return Err(Error {
                code: 3,
                message: if self.engine.is_hoa() {
                    HOA_SHORT_READ
                } else {
                    "short render input read"
                }
                .to_owned(),
                location: Location::None,
            });
        }
        self.smoother.apply(input).map_err(failed)?;
        self.fifo_len = self.outputs * frames;
        self.fifo_read = 0;
        let fifo = &mut self.fifo[..self.fifo_len];
        fifo.fill(0.);
        let location = self.engine.location();
        self.engine
            .process(input, fifo, self.done, frames)
            .map_err(|e| Error::dsp(&e, location))?;
        self.done += frames as u64;
        Ok(())
    }
}
