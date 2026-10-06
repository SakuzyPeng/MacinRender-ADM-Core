use crate::{Chna, Container, Error, Format, Result, SampleFormat, add, write_chunk};
use std::io::{Seek, SeekFrom, Write};

#[derive(Debug, Clone, Default)]
pub struct WriterOptions {
    /// None starts as RIFF and promotes integer PCM to BW64, float32 to RF64.
    pub container: Option<Container>,
    pub axml: Option<Vec<u8>>,
    pub chna: Option<Chna>,
}

/// Drop never finalizes a writer. Call finish and observe its result before installing output.
pub struct Writer<W> {
    sink: W,
    format: Format,
    container: Option<Container>,
    data_start: u64,
    data_bytes: u64,
    fact: Option<u64>,
    scratch: Vec<u8>,
    failed: bool,
}
impl<W: Write + Seek> Writer<W> {
    pub fn new(mut sink: W, format: Format, options: WriterOptions) -> Result<Self> {
        let align = format.block_align()?;
        let byte_rate = format.byte_rate()?;
        if options.container == Some(Container::Bw64) && !format.sample_format.is_integer() {
            return Err(Error::unsupported("BW64 输出仅支持整数 PCM"));
        }
        if options
            .chna
            .as_ref()
            .is_some_and(|chna| chna.tracks != format.channels)
        {
            return Err(Error::invalid("CHNA 轨道数与 fmt 不一致"));
        }
        let chna = options.chna.as_ref().map(Chna::encode).transpose()?;
        if options
            .axml
            .as_ref()
            .is_some_and(|data| data.len() >= u32::MAX as usize)
        {
            return Err(Error::unsupported("AXML chunk 超出 uint32"));
        }
        if sink.seek(SeekFrom::End(0))? != 0 {
            return Err(Error::invalid("WAVE writer 需要空的输出流"));
        }
        sink.seek(SeekFrom::Start(0))?;
        sink.write_all(b"RIFF\0\0\0\0WAVE")?;
        write_chunk(&mut sink, *b"JUNK", &[0; 28])?;
        let tag = if format.sample_format.is_integer() {
            1u16
        } else {
            3u16
        };
        let mut fmt = Vec::with_capacity(40);
        fmt.extend_from_slice(
            &if format.channel_mask.is_some() {
                0xfffeu16
            } else {
                tag
            }
            .to_le_bytes(),
        );
        fmt.extend_from_slice(&format.channels.to_le_bytes());
        fmt.extend_from_slice(&format.sample_rate.to_le_bytes());
        fmt.extend_from_slice(&byte_rate.to_le_bytes());
        fmt.extend_from_slice(&align.to_le_bytes());
        fmt.extend_from_slice(&format.sample_format.bits().to_le_bytes());
        if let Some(mask) = format.channel_mask {
            fmt.extend_from_slice(&22u16.to_le_bytes());
            fmt.extend_from_slice(&format.sample_format.bits().to_le_bytes());
            fmt.extend_from_slice(&mask.to_le_bytes());
            fmt.extend_from_slice(&tag.to_le_bytes());
            fmt.extend_from_slice(&[0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113]);
        }
        write_chunk(&mut sink, *b"fmt ", &fmt)?;
        let fact = if format.sample_format == SampleFormat::Float32 {
            let offset = add(sink.stream_position()?, 8)?;
            write_chunk(&mut sink, *b"fact", &[0; 4])?;
            Some(offset)
        } else {
            None
        };
        if let Some(data) = chna {
            write_chunk(&mut sink, *b"chna", &data)?;
        }
        if let Some(data) = options.axml {
            write_chunk(&mut sink, *b"axml", &data)?;
        }
        sink.write_all(b"data\0\0\0\0")?;
        let data_start = sink.stream_position()?;
        Ok(Self {
            sink,
            format,
            container: options.container,
            data_start,
            data_bytes: 0,
            fact,
            scratch: Vec::new(),
            failed: false,
        })
    }
    pub fn format(&self) -> Format {
        self.format
    }
    pub fn write_frames(&mut self, samples: &[f32]) -> Result<u64> {
        if self.failed {
            return Err(Error::io("WAVE writer 已因 I/O 错误失效"));
        }
        if !samples
            .len()
            .is_multiple_of(usize::from(self.format.channels))
        {
            return Err(Error::invalid("音频缓冲区不是完整帧"));
        }
        if self.format.sample_format.is_integer() && samples.iter().any(|sample| sample.is_nan()) {
            return Err(Error::invalid("NaN 无法编码为整数 PCM"));
        }
        let width = usize::from(self.format.sample_format.bits() / 8);
        let size = samples
            .len()
            .checked_mul(width)
            .ok_or_else(|| Error::invalid("PCM 缓冲区长度溢出"))?;
        let next = add(self.data_bytes, size as u64)?;
        let end = add(self.data_start, add(next, next & 1)?)?;
        if self.container == Some(Container::Riff) && end - 8 >= u64::from(u32::MAX) {
            return Err(Error::unsupported("固定 RIFF 输出超过大小上限"));
        }
        if size > self.scratch.len() {
            self.scratch
                .try_reserve(size - self.scratch.len())
                .map_err(|_| Error::io("无法分配 PCM 缓冲区"))?;
        }
        self.scratch.resize(size, 0);
        for (sample, raw) in samples.iter().zip(self.scratch.chunks_exact_mut(width)) {
            if self.format.sample_format == SampleFormat::Float32 {
                raw.copy_from_slice(&sample.to_le_bytes());
            } else {
                // Preserve libbw64 0.10.0: f32 clipping, f64 scale, truncate toward zero.
                let maximum = match self.format.sample_format {
                    SampleFormat::Pcm16 => 32767.0,
                    SampleFormat::Pcm24 => 8388607.0,
                    _ => 2147483647.0,
                };
                let value = (f64::from(sample.clamp(-1.0, 1.0)) * maximum) as i32;
                raw.copy_from_slice(&value.to_le_bytes()[..width]);
            }
        }
        if let Err(error) = self.sink.write_all(&self.scratch) {
            self.failed = true;
            return Err(error.into());
        }
        self.data_bytes = next;
        Ok((samples.len() / usize::from(self.format.channels)) as u64)
    }
    /// Returns the finalized stream. Failure consumes the writer without claiming a usable file.
    pub fn finish(mut self) -> Result<W> {
        if self.failed {
            return Err(Error::io("不能完成已失败的 WAVE writer"));
        }
        if self.data_bytes & 1 != 0 {
            self.sink.write_all(&[0])?;
        }
        let end = add(self.data_start, add(self.data_bytes, self.data_bytes & 1)?)?;
        let riff_size = end - 8;
        let frames = self.data_bytes / u64::from(self.format.block_align()?);
        let container = self.container.unwrap_or_else(|| {
            if riff_size < u64::from(u32::MAX) {
                Container::Riff
            } else if self.format.sample_format.is_integer() {
                Container::Bw64
            } else {
                Container::Rf64
            }
        });
        if container == Container::Riff && riff_size >= u64::from(u32::MAX) {
            return Err(Error::unsupported("固定 RIFF 输出超过大小上限"));
        }
        self.sink.seek(SeekFrom::Start(0))?;
        self.sink.write_all(&container.id())?;
        self.sink.write_all(
            &if container == Container::Riff {
                riff_size as u32
            } else {
                u32::MAX
            }
            .to_le_bytes(),
        )?;
        if container != Container::Riff {
            self.sink.seek(SeekFrom::Start(12))?;
            let mut ds64 = Vec::with_capacity(28);
            ds64.extend_from_slice(&riff_size.to_le_bytes());
            ds64.extend_from_slice(&self.data_bytes.to_le_bytes());
            ds64.extend_from_slice(&frames.to_le_bytes());
            ds64.extend_from_slice(&0u32.to_le_bytes());
            write_chunk(&mut self.sink, *b"ds64", &ds64)?;
        }
        self.sink.seek(SeekFrom::Start(self.data_start - 4))?;
        self.sink.write_all(
            &if container == Container::Riff {
                self.data_bytes as u32
            } else {
                u32::MAX
            }
            .to_le_bytes(),
        )?;
        if let Some(fact) = self.fact {
            self.sink.seek(SeekFrom::Start(fact))?;
            self.sink
                .write_all(&(frames.min(u64::from(u32::MAX)) as u32).to_le_bytes())?;
        }
        self.sink.seek(SeekFrom::Start(end))?;
        self.sink.flush()?;
        Ok(self.sink)
    }
}
