//! Core Audio Format files with little-endian float32 PCM, moved from the C++ `caf_io.cpp`
//! (PCM consistency 4.8). Chunk headers and metadata are big-endian; samples are little-endian
//! float32 on every host. Messages are the established English texts.
use crate::{Error, Result};
use std::{
    fs::{File, OpenOptions},
    io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write},
    path::Path,
};

/// Apple AudioChannelLayoutTag values, `(tag << 16) | channels`; public integers, no AudioToolbox.
/// BS.2051 calls the 22.2 LFE slots LFE1/LFE2 where CICP_13 says LFE2/LFE3: same slots.
const TAGS: [(&str, u32, u32); 10] = [
    ("0+2+0", (101 << 16) | 2, 2),    // MPEG_2_0 / CICP_2 (L R)
    ("binaural", (106 << 16) | 2, 2), // Binaural
    ("0+5+0", (121 << 16) | 6, 6),    // MPEG_5_1_A / CICP_6 (L R C LFE Ls Rs)
    ("wav71", (189 << 16) | 8, 8),    // WAVE_7_1 (L R C LFE Rls Rrs Ls Rs)
    ("hoa3", (190 << 16) | 16, 16),   // HOA_ACN_SN3D | 16 (ACN 0..15)
    ("2+5+0", (194 << 16) | 8, 8),    // Atmos_5_1_2
    ("4+5+0", (195 << 16) | 10, 10),  // Atmos_5_1_4
    ("4+7+0", (192 << 16) | 12, 12),  // Atmos_7_1_4
    ("9.1.6", (193 << 16) | 16, 16),  // Atmos_9_1_6
    ("9+10+3", (204 << 16) | 24, 24), // CICP_13 (22.2)
];
/// File offset of the data chunk's i64 size: caff 8 + desc 44 + chan 24 + "data" 4.
const DATA_SIZE_OFFSET: u64 = 80;

/// The layout tag for `layout`, checked against the channel count.
pub fn layout_tag(layout: &[u8], channels: u32) -> Result<u32> {
    let text = String::from_utf8_lossy(layout);
    let (_, tag, expected) = TAGS
        .iter()
        .find(|(id, ..)| id.as_bytes() == layout)
        .ok_or_else(|| {
            Error::unsupported(format!("CAF output: no channel layout tag for '{text}'"))
        })?;
    if channels != *expected {
        return Err(Error::invalid(format!(
            "CAF output layout '{text}' expects {expected} channels, got {channels}"
        )));
    }
    Ok(*tag)
}

/// Streaming float32 writer. Drop never finalizes: call `finish` to patch the data size.
pub struct Writer<W> {
    sink: W,
    channels: usize,
    pcm_bytes: u64,
    scratch: Vec<u8>,
    failed: bool,
}
impl Writer<BufWriter<File>> {
    /// Replaces `path`; the layout is checked before the file is opened.
    pub fn create(path: &Path, channels: u32, rate: u32, layout: &[u8]) -> Result<Self> {
        let tag = layout_tag(layout, channels)?;
        let file =
            File::create(path).map_err(|_| Error::io("failed to open CAF file for writing"))?;
        Self::with_tag(BufWriter::new(file), channels, rate, tag)
    }
}
impl<W: Write + Seek> Writer<W> {
    pub fn new(sink: W, channels: u32, rate: u32, layout: &[u8]) -> Result<Self> {
        let tag = layout_tag(layout, channels)?;
        Self::with_tag(sink, channels, rate, tag)
    }
    fn with_tag(mut sink: W, channels: u32, rate: u32, tag: u32) -> Result<Self> {
        let mut head = Vec::with_capacity(DATA_SIZE_OFFSET as usize + 12);
        head.extend_from_slice(b"caff");
        head.extend_from_slice(&1u16.to_be_bytes()); // version
        head.extend_from_slice(&0u16.to_be_bytes()); // flags
        head.extend_from_slice(b"desc");
        head.extend_from_slice(&32i64.to_be_bytes());
        head.extend_from_slice(&f64::from(rate).to_bits().to_be_bytes());
        head.extend_from_slice(b"lpcm");
        for word in [3, channels.wrapping_mul(4), 1, channels, 32] {
            // IsFloat | IsLittleEndian, bytes per packet, frames per packet, channels, bits
            head.extend_from_slice(&u32::to_be_bytes(word));
        }
        head.extend_from_slice(b"chan");
        head.extend_from_slice(&12i64.to_be_bytes());
        for word in [tag, 0, 0] {
            // tag, unused bitmap, no channel descriptions
            head.extend_from_slice(&u32::to_be_bytes(word));
        }
        head.extend_from_slice(b"data");
        head.extend_from_slice(&(-1i64).to_be_bytes()); // unknown until finish
        head.extend_from_slice(&0u32.to_be_bytes()); // edit count
        sink.write_all(&head)?;
        Ok(Self {
            sink,
            channels: channels as usize,
            pcm_bytes: 0,
            scratch: Vec::new(),
            failed: false,
        })
    }
    /// Writes whole interleaved frames, all or nothing; returns the frame count.
    pub fn write_frames(&mut self, samples: &[f32]) -> Result<u64> {
        if self.failed {
            return Err(Error::io("CAF writer 已因 I/O 错误失效"));
        }
        if self.channels == 0 || !samples.len().is_multiple_of(self.channels) {
            return Err(Error::invalid("音频缓冲区不是完整帧"));
        }
        self.scratch.clear();
        self.scratch
            .try_reserve(samples.len() * 4)
            .map_err(|_| Error::io("无法分配 PCM 缓冲区"))?;
        self.scratch.extend(
            samples
                .iter()
                .flat_map(|sample| sample.to_bits().to_le_bytes()),
        );
        if let Err(error) = self.sink.write_all(&self.scratch) {
            self.failed = true;
            return Err(error.into());
        }
        self.pcm_bytes += self.scratch.len() as u64;
        Ok((samples.len() / self.channels) as u64)
    }
    /// Patches the data size (edit count + PCM bytes) and flushes.
    pub fn finish(mut self) -> Result<W> {
        if self.failed {
            return Err(Error::io("不能完成已失败的 CAF writer"));
        }
        self.sink.seek(SeekFrom::Start(DATA_SIZE_OFFSET))?;
        self.sink
            .write_all(&(4 + self.pcm_bytes as i64).to_be_bytes())?;
        self.sink.flush()?;
        Ok(self.sink)
    }
}

/// Reader for the float32 little-endian files this writer produces. Scans every chunk, so any
/// order (a trailing `info` included) is accepted; a later `desc` or `data` replaces an earlier one.
pub struct Reader<R> {
    source: R,
    channels: u32,
    rate: u32,
    frames: u64,
    frames_read: u64,
    bytes: Vec<u8>,
}
impl Reader<BufReader<File>> {
    pub fn open(path: &Path) -> Result<Self> {
        let file = File::open(path).map_err(|_| Error::io("cannot open CAF file for reading"))?;
        Self::new(BufReader::new(file))
    }
}
/// Reads up to `out.len()` bytes like `fread`: stops early only at end of file.
fn fill<R: Read>(source: &mut R, out: &mut [u8]) -> Result<usize> {
    let mut done = 0;
    while done < out.len() {
        match source.read(&mut out[done..]) {
            Ok(0) => break,
            Ok(n) => done += n,
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error.into()),
        }
    }
    Ok(done)
}
fn exact<R: Read, const N: usize>(source: &mut R) -> Result<Option<[u8; N]>> {
    let mut out = [0; N];
    Ok((fill(source, &mut out)? == N).then_some(out))
}
impl<R: Read + Seek> Reader<R> {
    pub fn new(mut source: R) -> Result<Self> {
        let truncated_header = || Error::io("truncated CAF file header");
        let magic = exact::<_, 4>(&mut source)?.ok_or_else(truncated_header)?;
        if &magic != b"caff" {
            return Err(Error::io("not a CAF file (bad magic)"));
        }
        // Version and flags are not interpreted.
        exact::<_, 4>(&mut source)?.ok_or_else(truncated_header)?;
        let mut channels = 0u32;
        let mut rate = 0u32;
        let mut desc = false;
        let mut data_start: Option<u64> = None;
        let mut data_bytes = 0u64;
        while let Some(kind) = exact::<_, 4>(&mut source)? {
            let size = i64::from_be_bytes(
                exact::<_, 8>(&mut source)?
                    .ok_or_else(|| Error::io("truncated CAF chunk header"))?,
            );
            let payload = source.stream_position()?;
            // C++ fseek(payload + size), result unchecked: a target the file cannot seek to
            // (negative, or past the largest offset) leaves the position where it was.
            let skip = |source: &mut R| -> Result<()> {
                let target = (payload as i64).wrapping_add(size);
                if target >= 0 {
                    let _ = source.seek(SeekFrom::Start(target as u64));
                }
                Ok(())
            };
            match &kind {
                b"desc" => {
                    let raw = exact::<_, 32>(&mut source)?
                        .ok_or_else(|| Error::io("truncated CAF desc chunk"))?;
                    let word = |at: usize| u32::from_be_bytes(raw[at..at + 4].try_into().unwrap());
                    let sample_rate =
                        f64::from_bits(u64::from_be_bytes(raw[..8].try_into().unwrap()));
                    // flags IsFloat | IsLittleEndian, one frame per packet, 32 bits per channel
                    if &raw[8..12] != b"lpcm"
                        || word(12) & 3 != 3
                        || word(20) != 1
                        || word(28) != 32
                    {
                        return Err(Error::unsupported(
                            "CAF file is not float32 LE lpcm — cannot read back",
                        ));
                    }
                    channels = word(24);
                    // Saturating where the C++ conversion was undefined (NaN, negative, > u32).
                    rate = sample_rate as u32;
                    desc = true;
                    skip(&mut source)?;
                }
                b"data" => {
                    let start = source.seek(SeekFrom::Current(4))?; // edit count
                    data_start = Some(start);
                    if size == -1 {
                        // Unknown size: the data runs to the end of the file.
                        let end = source.seek(SeekFrom::End(0))?;
                        data_bytes = end.wrapping_sub(start);
                    } else {
                        data_bytes = (size as u64).wrapping_sub(4);
                        skip(&mut source)?;
                    }
                }
                _ if size >= 0 => skip(&mut source)?,
                // An unneeded chunk of unknown size ends the scan.
                _ => break,
            }
        }
        let Some(start) = data_start.filter(|_| desc && channels != 0) else {
            return Err(Error::io("CAF file missing valid desc or data chunk"));
        };
        source.seek(SeekFrom::Start(start))?;
        Ok(Self {
            source,
            channels,
            rate,
            frames: data_bytes / (u64::from(channels) * 4),
            frames_read: 0,
            bytes: Vec::new(),
        })
    }
    pub fn channels(&self) -> u32 {
        self.channels
    }
    pub fn sample_rate(&self) -> u32 {
        self.rate
    }
    pub fn frame_count(&self) -> u64 {
        self.frames
    }
    /// Reads up to `out.len() / channels` frames; fewer only at the end of the file.
    pub fn read(&mut self, out: &mut [f32]) -> Result<u64> {
        let channels = self.channels as usize;
        let wanted = ((out.len() / channels) as u64).min(self.frames - self.frames_read);
        if wanted == 0 {
            return Ok(0);
        }
        let samples = wanted as usize * channels;
        self.bytes.resize(samples * 4, 0);
        let got = fill(&mut self.source, &mut self.bytes)? / 4 / channels;
        for (sample, raw) in out
            .iter_mut()
            .zip(self.bytes.as_chunks::<4>().0)
            .take(got * channels)
        {
            *sample = f32::from_bits(u32::from_le_bytes(*raw));
        }
        self.frames_read += got as u64;
        Ok(got as u64)
    }
}

/// Appends an `info` chunk: entry count (u32 BE) and `key\0value\0` pairs for the non-empty
/// encoder, date and comments values.
pub fn append_info(path: &Path, encoder: &[u8], date: &[u8], comments: &[u8]) -> Result<()> {
    let mut file = OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .map_err(|_| Error::io("cannot open CAF for metadata write"))?;
    if exact::<_, 4>(&mut file)? != Some(*b"caff") {
        return Err(Error::io("not a CAF file"));
    }
    let pairs: Vec<(&[u8], &[u8])> = [
        (&b"encodingapplication"[..], encoder),
        (b"date", date),
        (b"comments", comments),
    ]
    .into_iter()
    .filter(|(_, value)| !value.is_empty())
    .collect();
    let mut payload = (pairs.len() as u32).to_be_bytes().to_vec();
    for (key, value) in pairs {
        payload.extend_from_slice(key);
        payload.push(0);
        payload.extend_from_slice(value);
        payload.push(0);
    }
    let mut chunk = b"info".to_vec();
    chunk.extend_from_slice(&(payload.len() as i64).to_be_bytes());
    chunk.extend_from_slice(&payload);
    file.seek(SeekFrom::End(0))?;
    file.write_all(&chunk)?;
    file.flush()?;
    Ok(())
}
