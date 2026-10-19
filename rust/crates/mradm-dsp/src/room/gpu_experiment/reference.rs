//! CPU execution of the recorded target/finite-fade schedule for local tests.
use super::super::prepared::ResponseWork;
use super::*;
use crate::{Complex32 as C, fft::RealFft};
pub struct ReferenceFactory;
impl Factory for ReferenceFactory {
    fn create(&self, room: Arc<PreparedRoom>) -> Result<Box<dyn Backend>> {
        Ok(Box::new(Reference::new(room)?))
    }
    fn create_late(&self, room: Arc<PreparedRoom>) -> Result<Option<Box<dyn LateBackend>>> {
        let Some(first) = room.late[0].gpu_tail() else {
            return Ok(None);
        };
        let parts = first.len() / 2049;
        let mut filters = Vec::with_capacity(first.len() * room.late.len());
        for row in &room.late {
            let tail = row
                .gpu_tail()
                .ok_or(Error::Unsupported("Inconsistent GPU late shapes"))?;
            if row.channels != 1 || tail.len() != first.len() {
                return Err(Error::Unsupported("Inconsistent GPU late shapes"));
            }
            filters.extend_from_slice(tail);
        }
        Ok(Some(Box::new(ReferenceLate {
            parts,
            head: parts - 1,
            filters,
            history: vec![C::default(); first.len()],
            sum: vec![C::default(); 2049],
            time: vec![0.; 4096],
            output: vec![0.; room.late.len() * 2048],
            fft: RealFft::with_policy(4096, room.quality.fft())?,
        })))
    }
}
struct ReferenceLate {
    parts: usize,
    head: usize,
    filters: Vec<C>,
    history: Vec<C>,
    sum: Vec<C>,
    time: Vec<f32>,
    output: Vec<f32>,
    fft: RealFft,
}
impl LateBackend for ReferenceLate {
    fn submit(&mut self, frequency: &[C], reset: bool) -> Result<()> {
        if frequency.len() != 2049 {
            return Err(Error::InvalidArgument("Invalid GPU late input"));
        }
        if reset {
            self.history.fill(C::default());
            self.head = self.parts - 1;
        }
        self.head = (self.head + 1) % self.parts;
        self.history[self.head * 2049..(self.head + 1) * 2049].copy_from_slice(frequency);
        for (direction, output) in self.output.as_chunks_mut::<2048>().0.iter_mut().enumerate() {
            self.sum.fill(C::default());
            for part in 0..self.parts {
                let x = (self.head + self.parts - part) % self.parts * 2049;
                let h = (direction * self.parts + part) * 2049;
                for bin in 0..2049 {
                    self.sum[bin] += self.history[x + bin] * self.filters[h + bin];
                }
            }
            self.fft.inverse(&self.sum, &mut self.time)?;
            output.copy_from_slice(&self.time[2048..]);
        }
        Ok(())
    }
    fn finish(&mut self, output: &mut [f32]) -> Result<()> {
        if output.len() != self.output.len() {
            return Err(Error::InvalidArgument("Invalid GPU late output"));
        }
        output.copy_from_slice(&self.output);
        Ok(())
    }
}
struct Reference {
    room: Arc<PreparedRoom>,
    work: ResponseWork,
    fft: RealFft,
    parts: usize,
    history_parts: usize,
    history: Vec<C>,
    filters: Vec<C>,
    sum: Vec<C>,
    time: Vec<f32>,
    output: Vec<f32>,
    voices: usize,
}
impl Reference {
    fn new(room: Arc<PreparedRoom>) -> Result<Self> {
        let parts = room.short_taps.div_ceil(BLOCK);
        let history_parts = room.short_taps.max(room.send_taps).div_ceil(BLOCK);
        Ok(Self {
            work: ResponseWork::new(&room)?,
            fft: RealFft::with_policy(BLOCK * 2, room.quality.fft())?,
            parts,
            history_parts,
            history: vec![C::default(); MAX_VOICES * 2 * history_parts * (BLOCK + 1)],
            filters: vec![C::default(); MAX_VOICES * 2 * SLOTS * 2 * parts * (BLOCK + 1)],
            sum: vec![C::default(); BLOCK + 1],
            time: vec![0.; BLOCK * 2],
            output: vec![0.; MAX_VOICES * 4 * BLOCK],
            voices: 0,
            room,
        })
    }
}
impl Backend for Reference {
    fn begin(&mut self, voices: usize) -> Result<()> {
        if voices > MAX_VOICES {
            return Err(Error::Unsupported("GPU source capacity exceeded"));
        }
        self.voices = voices;
        self.output.fill(0.);
        Ok(())
    }
    fn voice(&mut self, index: usize, frame: &Frame) -> Result<()> {
        let bins = BLOCK + 1;
        if frame.reset {
            self.history[index * 2 * self.history_parts * bins
                ..(index + 1) * 2 * self.history_parts * bins]
                .fill(C::default());
        }
        if frame.active {
            for branch in 0..2 {
                let head = frame.heads[branch] as usize;
                for b in 0..bins {
                    let input = (branch * bins + b) * 2;
                    self.history[((index * 2 + branch) * self.history_parts + head) * bins + b] =
                        C::new(frame.frequency[input], frame.frequency[input + 1]);
                }
            }
        }
        for request in &frame.requests {
            let u = request.update;
            let branch = u.branch as usize;
            self.work.build_branches(
                &self.room,
                &frame.sources[request.sources.clone()],
                &request.rotation,
                branch == 0,
                branch == 1,
            )?;
            for ear in 0..2 {
                for part in 0..u.parts as usize {
                    self.time.fill(0.);
                    let start = part * BLOCK;
                    let count = BLOCK.min(u.support as usize - start);
                    self.time[..count].copy_from_slice(
                        &self.work.impulse[(branch * 2 + ear) * self.room.short_taps + start
                            ..(branch * 2 + ear) * self.room.short_taps + start + count],
                    );
                    let offset = (((index * 2 + branch) * SLOTS + u.slot as usize) * 2 + ear)
                        * self.parts
                        * bins
                        + part * bins;
                    self.fft
                        .forward(&self.time, &mut self.filters[offset..offset + bins])?;
                }
            }
        }
        for task in &frame.tasks {
            let branch = task.branch as usize;
            let head = frame.heads[branch] as usize;
            for ear in 0..2 {
                self.sum.fill(C::default());
                for part in 0..task.parts as usize {
                    let at = (head + self.history_parts - part) % self.history_parts;
                    let x = ((index * 2 + branch) * self.history_parts + at) * bins;
                    let h = (((index * 2 + branch) * SLOTS + task.slot as usize) * 2 + ear)
                        * self.parts
                        * bins
                        + part * bins;
                    for b in 0..bins {
                        self.sum[b] += self.history[x + b] * self.filters[h + b];
                    }
                }
                self.fft.inverse(&self.sum, &mut self.time)?;
                for i in task.begin as usize..(task.begin + task.frames) as usize {
                    self.output[(index * 4 + branch * 2 + ear) * BLOCK + i] += self.time[BLOCK + i]
                        * task.weight(
                            i,
                            super::super::filter_fade(self.room.profile.sample_rate) as u64,
                        );
                }
            }
        }
        Ok(())
    }
    fn submit(&mut self) -> Result<()> {
        Ok(())
    }
    fn finish(&mut self, output: &mut [f32]) -> Result<()> {
        if output.len() != self.voices * 4 * BLOCK {
            return Err(Error::InvalidArgument("Invalid GPU output buffer"));
        }
        output.copy_from_slice(&self.output[..output.len()]);
        Ok(())
    }
}
