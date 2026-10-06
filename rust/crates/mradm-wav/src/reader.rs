use crate::{
    Chna, Container, Error, Format, Info, Result, SampleFormat, add, bytes, read_array, read_u16,
    read_u32, read_u64,
};
use std::io::{Read, Seek, SeekFrom};

#[derive(Debug, Clone, Copy)]
pub struct Chunk {
    pub id: [u8; 4],
    pub offset: u64,
    pub size: u64,
}

pub struct Reader<R> {
    source: R,
    info: Info,
    chunks: Vec<Chunk>,
    end: u64,
    start: u64,
    position: u64,
    scratch: Vec<u8>,
    failed: bool,
}
impl<R: Read + Seek> Reader<R> {
    pub fn new(mut source: R) -> Result<Self> {
        let physical_end = source.seek(SeekFrom::End(0))?;
        source.seek(SeekFrom::Start(0))?;
        let container = match &read_array::<4>(&mut source)? {
            b"RIFF" => Container::Riff,
            b"RF64" => Container::Rf64,
            b"BW64" => Container::Bw64,
            _ => return Err(Error::unsupported("不是 RIFF/RF64/BW64 文件")),
        };
        let short_size = read_u32(&mut source)?;
        if &read_array::<4>(&mut source)? != b"WAVE" {
            return Err(Error::unsupported("不是 WAVE 文件"));
        }
        let mut end = add(u64::from(short_size), 8)?;
        let mut at = 12;
        let mut long_data = None;
        let mut table = Vec::new();
        let mut chunks = Vec::new();
        if container != Container::Riff {
            if short_size != u32::MAX || &read_array::<4>(&mut source)? != b"ds64" {
                return Err(Error::io("RF64/BW64 缺少 ds64 或大小标记"));
            }
            let size = u64::from(read_u32(&mut source)?);
            at = add(20, add(size, size & 1)?)?;
            if size < 28 || at > physical_end {
                return Err(Error::io("ds64 长度无效"));
            }
            end = add(read_u64(&mut source)?, 8)?;
            long_data = Some(read_u64(&mut source)?);
            // sampleCount mirrors the optional fact chunk and writers disagree on its unit
            // (frames vs samples); the frame count is derived from data like dr_wav does.
            let _sample_count = read_u64(&mut source)?;
            let count = read_u32(&mut source)?;
            if u64::from(count) * 12 > size - 28 {
                return Err(Error::io("ds64 表被截断"));
            }
            for _ in 0..count {
                table.try_reserve(1).map_err(|_| Error::io("ds64 表过大"))?;
                table.push((read_array::<4>(&mut source)?, read_u64(&mut source)?, false));
            }
            chunks.push(Chunk {
                id: *b"ds64",
                offset: 20,
                size,
            });
        } else if short_size == u32::MAX {
            return Err(Error::io("RIFF 使用了 64 位大小标记"));
        }
        if end < at || end > physical_end {
            return Err(Error::io("WAVE 声明长度超出文件"));
        }
        let mut format = None;
        let mut data = None;
        while at < end {
            if end - at < 8 {
                return Err(Error::io("WAVE chunk 头被截断"));
            }
            source.seek(SeekFrom::Start(at))?;
            let id = read_array::<4>(&mut source)?;
            let short = read_u32(&mut source)?;
            let size = if short != u32::MAX {
                u64::from(short)
            } else if id == *b"data" {
                long_data.ok_or_else(|| Error::io("data 缺少 ds64 长度"))?
            } else {
                let entry = table
                    .iter_mut()
                    .find(|e| e.0 == id && !e.2)
                    .ok_or_else(|| Error::io("chunk 缺少 ds64 表条目"))?;
                entry.2 = true;
                entry.1
            };
            let offset = add(at, 8)?;
            let next = add(offset, add(size, size & 1)?)?;
            if next > end {
                return Err(Error::io("WAVE chunk 超出容器边界"));
            }
            match &id {
                b"fmt " => {
                    if format.is_some() {
                        return Err(Error::io("重复 fmt chunk"));
                    }
                    format = Some(read_format(&mut source, size)?);
                }
                b"data" => {
                    if data.is_some() || format.is_none() {
                        return Err(Error::io("data 重复或位于 fmt 之前"));
                    }
                    if long_data.is_some_and(|n| n != size) {
                        return Err(Error::io("data 与 ds64 长度不一致"));
                    }
                    data = Some((offset, size));
                }
                b"ds64" => return Err(Error::io("ds64 位置无效或重复")),
                _ => {}
            }
            chunks
                .try_reserve(1)
                .map_err(|_| Error::io("WAVE chunk 表过大"))?;
            chunks.push(Chunk { id, offset, size });
            at = next;
        }
        let (format, valid_bits) = format.ok_or_else(|| Error::io("缺少 fmt chunk"))?;
        let (start, data_bytes) = data.ok_or_else(|| Error::io("缺少 data chunk"))?;
        let align = u64::from(format.block_align()?);
        if !data_bytes.is_multiple_of(align) {
            return Err(Error::io("data 长度不是完整音频帧"));
        }
        let frames = data_bytes / align;
        source.seek(SeekFrom::Start(start))?;
        Ok(Self {
            source,
            info: Info {
                container,
                format,
                valid_bits,
                frames,
                data_bytes,
            },
            chunks,
            end,
            start,
            position: 0,
            scratch: Vec::new(),
            failed: false,
        })
    }
    pub fn info(&self) -> Info {
        self.info
    }
    pub fn chunks(&self) -> &[Chunk] {
        &self.chunks
    }
    /// Declared container end (top-level size + 8); never beyond the physical end.
    pub fn end(&self) -> u64 {
        self.end
    }
    pub fn into_inner(self) -> R {
        self.source
    }
    pub fn position(&self) -> u64 {
        self.position
    }
    /// Read one metadata chunk with an explicit caller budget. Never allocate unknown chunks on open.
    pub fn metadata(&mut self, id: [u8; 4], limit: u64) -> Result<Option<Vec<u8>>> {
        let Some(chunk) = self.chunks.iter().find(|chunk| chunk.id == id).copied() else {
            return Ok(None);
        };
        if chunk.size > limit {
            return Err(Error::unsupported("WAVE 元数据超过调用方预算"));
        }
        let mut data = bytes(chunk.size)?;
        self.source.seek(SeekFrom::Start(chunk.offset))?;
        self.source.read_exact(&mut data)?;
        Ok(Some(data))
    }
    /// Copy the first chunk with this id into `output`, which must hold exactly its payload.
    pub fn read_chunk(&mut self, id: [u8; 4], output: &mut [u8]) -> Result<bool> {
        let Some(chunk) = self.chunks.iter().find(|chunk| chunk.id == id).copied() else {
            return Ok(false);
        };
        if chunk.size != output.len() as u64 {
            return Err(Error::invalid("WAVE chunk 缓冲区长度不匹配"));
        }
        self.source.seek(SeekFrom::Start(chunk.offset))?;
        self.source.read_exact(output)?;
        Ok(true)
    }
    pub fn chna(&mut self) -> Result<Option<Chna>> {
        self.metadata(*b"chna", 4 + u64::from(u16::MAX) * 40)?
            .map(|data| {
                let chna = Chna::decode(&data)?;
                if chna.tracks != self.info.format.channels {
                    return Err(Error::io("CHNA 轨道数与 fmt 不一致"));
                }
                Ok(chna)
            })
            .transpose()
    }
    pub fn seek_frame(&mut self, frame: u64) -> Result<u64> {
        if self.failed {
            return Err(Error::io("WAVE reader 已因 I/O 错误失效"));
        }
        let frame = frame.min(self.info.frames);
        let offset = add(
            self.start,
            frame * u64::from(self.info.format.block_align()?),
        )?;
        if let Err(error) = self.source.seek(SeekFrom::Start(offset)) {
            self.failed = true;
            return Err(error.into());
        }
        self.position = frame;
        Ok(frame)
    }
    pub fn read_frames(&mut self, output: &mut [f32]) -> Result<u64> {
        if self.failed {
            return Err(Error::io("WAVE reader 已因 I/O 错误失效"));
        }
        let channels = usize::from(self.info.format.channels);
        if !output.len().is_multiple_of(channels) {
            return Err(Error::invalid("音频缓冲区不是完整帧"));
        }
        let frames = (output.len() / channels) as u64;
        let frames = frames.min(self.info.frames - self.position);
        if frames == 0 {
            return Ok(0);
        }
        let size = usize::try_from(frames * u64::from(self.info.format.block_align()?))
            .map_err(|_| Error::invalid("音频缓冲区过大"))?;
        if size > self.scratch.len() {
            self.scratch
                .try_reserve(size - self.scratch.len())
                .map_err(|_| Error::io("无法分配 PCM 缓冲区"))?;
        }
        self.scratch.resize(size, 0);
        let offset = add(
            self.start,
            self.position * u64::from(self.info.format.block_align()?),
        )?;
        let read = self
            .source
            .seek(SeekFrom::Start(offset))
            .and_then(|_| self.source.read_exact(&mut self.scratch));
        if let Err(error) = read {
            self.failed = true;
            return Err(error.into());
        }
        let width = usize::from(self.info.format.sample_format.bits() / 8);
        for (sample, raw) in output.iter_mut().zip(self.scratch.chunks_exact(width)) {
            *sample = match self.info.format.sample_format {
                SampleFormat::Pcm16 => f32::from(i16::from_le_bytes([raw[0], raw[1]])) / 32768.0,
                SampleFormat::Pcm24 => {
                    (i32::from_le_bytes([0, raw[0], raw[1], raw[2]]) as f32) / 2147483648.0
                }
                SampleFormat::Pcm32 => {
                    (i32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]) as f32) / 2147483648.0
                }
                SampleFormat::Float32 => f32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]),
            };
        }
        self.position += frames;
        Ok(frames)
    }
}

fn read_format(source: &mut impl Read, size: u64) -> Result<(Format, u16)> {
    if size < 16 {
        return Err(Error::io("fmt 被截断"));
    }
    let mut tag = read_u16(source)?;
    let channels = read_u16(source)?;
    let sample_rate = read_u32(source)?;
    let byte_rate = read_u32(source)?;
    let block_align = read_u16(source)?;
    let bits = read_u16(source)?;
    let mut valid_bits = bits;
    let mut channel_mask = None;
    if tag == 0xfffe {
        if size < 40 {
            return Err(Error::io("WAVE_FORMAT_EXTENSIBLE 被截断"));
        }
        let extension = read_u16(source)?;
        let valid = read_u16(source)?;
        if extension < 22 || u64::from(extension) + 18 > size {
            return Err(Error::io("fmt 扩展长度无效"));
        }
        if valid > bits {
            return Err(Error::io("有效位深超出 PCM 容器位深"));
        }
        valid_bits = if valid == 0 { bits } else { valid };
        channel_mask = Some(read_u32(source)?);
        let guid = read_array::<16>(source)?;
        if guid[2..] != [0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113] {
            return Err(Error::unsupported("不支持的 WAVE 子格式 GUID"));
        }
        tag = u16::from_le_bytes([guid[0], guid[1]]);
    }
    let sample_format = match (tag, bits) {
        (1, 16) => SampleFormat::Pcm16,
        (1, 24) => SampleFormat::Pcm24,
        (1, 32) => SampleFormat::Pcm32,
        (3, 32) => SampleFormat::Float32,
        _ => return Err(Error::unsupported("不支持的 WAVE 编码或位深")),
    };
    if sample_format == SampleFormat::Float32 && valid_bits != bits {
        return Err(Error::unsupported("浮点 WAVE 的有效位深必须等于容器位深"));
    }
    let format = Format {
        channels,
        sample_rate,
        sample_format,
        channel_mask,
    };
    if format.block_align()? != block_align || format.byte_rate()? != byte_rate {
        return Err(Error::io("fmt block alignment 或 byte rate 不一致"));
    }
    Ok((format, valid_bits))
}
