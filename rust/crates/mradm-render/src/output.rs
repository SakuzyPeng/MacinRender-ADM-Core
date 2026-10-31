//! Final float PCM writers (PCM consistency 4.8): WAVE (always RF64) or CAF, chosen by the caller.
//! Every writer takes the next number of one process-wide sequence when it opens, so the
//! `writer/pass{N}-s{frame}.80-pcm.f32` checkpoints of WAVE and CAF outputs never share a name.
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_wav::{Result, caf};
use std::{
    fs::File,
    io::BufWriter,
    path::Path,
    sync::atomic::{AtomicU64, Ordering},
};

static NEXT_TRACE: AtomicU64 = AtomicU64::new(0);

/// Unit tests that create output writers take turns so sequence assertions remain isolated.
#[cfg(test)]
pub(crate) static TEST_WRITER_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

enum Sink {
    Wav(mradm_wav::Writer<File>),
    Caf(caf::Writer<BufWriter<File>>),
}

/// A float32 output file. Drop finishes on a best-effort basis; `finish` reports failure.
pub struct Writer {
    sink: Option<Sink>,
    finished: Option<Result<()>>,
    trace: u64,
    position: u64,
    channels: usize,
}
impl Writer {
    /// `layout` selects CAF output and its channel layout tag; `None` writes RF64 WAVE.
    pub fn create(path: &Path, channels: u32, rate: u32, layout: Option<&[u8]>) -> Result<Self> {
        let sink = match layout {
            Some(layout) => Sink::Caf(caf::Writer::create(path, channels, rate, layout)?),
            None => Sink::Wav(mradm_wav::create_file_writer(
                path, channels, rate, 32, true, false,
            )?),
        };
        Ok(Self {
            sink: Some(sink),
            finished: None,
            trace: NEXT_TRACE.fetch_add(1, Ordering::Relaxed),
            position: 0,
            channels: channels as usize,
        })
    }
    /// The process-wide writer number of this output.
    pub fn trace(&self) -> u64 {
        self.trace
    }
    /// Writes whole interleaved frames, all or nothing; returns the frame count.
    pub fn write(&mut self, samples: &[f32]) -> Result<u64> {
        let frames = match self.sink.as_mut() {
            Some(Sink::Wav(writer)) => writer.write_frames(samples)?,
            Some(Sink::Caf(writer)) => writer.write_frames(samples)?,
            None => return Err(mradm_wav::Error::invalid("输出 writer 已关闭")),
        };
        if ENABLED && frames != 0 {
            let name = format!("writer/pass{}-s{}.80-pcm.f32", self.trace, self.position);
            let _ = process(
                &name,
                Words::F32(&samples[..frames as usize * self.channels]),
            );
        }
        self.position += frames;
        Ok(frames)
    }
    /// Finalizes once; repeated calls return the first result, including a failure.
    pub fn finish(&mut self) -> Result<()> {
        if let Some(sink) = self.sink.take() {
            self.finished = Some(match sink {
                Sink::Wav(writer) => writer.finish().map(drop),
                Sink::Caf(writer) => writer.finish().map(drop),
            });
        }
        self.finished.clone().unwrap_or(Ok(()))
    }
}
impl Drop for Writer {
    fn drop(&mut self) {
        let _ = self.finish();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wave_and_caf_writers_share_one_sequence_and_failed_opens_take_no_number() {
        let _turn = TEST_WRITER_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let directory = std::env::temp_dir().join(format!("mradm-output-{}", std::process::id()));
        std::fs::create_dir_all(&directory).unwrap();
        let wav = Writer::create(&directory.join("a.wav"), 2, 48000, None).unwrap();
        assert!(Writer::create(&directory.join("b.caf"), 2, 48000, Some(b"5.1")).is_err());
        assert!(Writer::create(&directory.join("c.caf"), 3, 48000, Some(b"0+2+0")).is_err());
        assert!(Writer::create(&directory.join("d.wav"), 2, 48000, Some(b"")).is_err());
        assert!(
            Writer::create(&directory.join("e.wav"), 2, 48000, None)
                .and(Writer::create(
                    &directory.join("missing/f.wav"),
                    2,
                    48000,
                    None
                ))
                .is_err()
        );
        let caf = Writer::create(&directory.join("g.caf"), 2, 48000, Some(b"binaural")).unwrap();
        // a.wav, e.wav (opened, then dropped) and g.caf take consecutive numbers.
        assert_eq!(caf.trace(), wav.trace() + 2);
        assert!(!directory.join("b.caf").exists() && !directory.join("c.caf").exists());
        drop((wav, caf));
        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn finish_runs_once_and_closes_the_writer() {
        let _turn = TEST_WRITER_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let path = std::env::temp_dir().join(format!("mradm-output-{}.caf", std::process::id()));
        let mut writer = Writer::create(&path, 2, 48000, Some(b"0+2+0")).unwrap();
        assert_eq!(writer.write(&[0.5, -0.5, 0.25, -0.25]).unwrap(), 2);
        assert!(writer.write(&[0.5]).is_err());
        writer.finish().unwrap();
        writer.finish().unwrap();
        assert!(writer.write(&[0.0, 0.0]).is_err());
        let reader = caf::Reader::open(&path).unwrap();
        assert_eq!(reader.frame_count(), 2);
        std::fs::remove_file(&path).unwrap();
    }
}
