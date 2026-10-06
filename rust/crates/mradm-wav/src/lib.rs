//! Streaming RIFF/RF64/BW64 I/O and container-level edits. ADM XML interpretation remains
//! outside this crate.
#![forbid(unsafe_code)]

mod chna;
mod edit;
mod reader;
mod writer;

pub use chna::{Chna, ChnaEntry};
pub use edit::{
    AMBI_HOA3, BEXT_SIZE, BextFields, ChnaTrack, LayoutOptions, LayoutRewriter, append_bext,
    bext_payload, has_chunk, replace_chunk,
};
pub use reader::{Chunk, Reader};
pub use writer::{Writer, WriterOptions};

use std::io::{Read, Write};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    pub code: i32,
    pub message: String,
}
impl Error {
    pub fn invalid(message: impl Into<String>) -> Self {
        Self {
            code: 1,
            message: message.into(),
        }
    }
    pub fn unsupported(message: impl Into<String>) -> Self {
        Self {
            code: 2,
            message: message.into(),
        }
    }
    pub fn io(message: impl Into<String>) -> Self {
        Self {
            code: 3,
            message: message.into(),
        }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.message.fmt(f)
    }
}
impl std::error::Error for Error {}
impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self::io(error.to_string())
    }
}
pub type Result<T> = std::result::Result<T, Error>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Container {
    Riff,
    Rf64,
    Bw64,
}
impl Container {
    fn id(self) -> [u8; 4] {
        match self {
            Self::Riff => *b"RIFF",
            Self::Rf64 => *b"RF64",
            Self::Bw64 => *b"BW64",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SampleFormat {
    Pcm16,
    Pcm24,
    Pcm32,
    Float32,
}
impl SampleFormat {
    pub fn bits(self) -> u16 {
        match self {
            Self::Pcm16 => 16,
            Self::Pcm24 => 24,
            Self::Pcm32 | Self::Float32 => 32,
        }
    }
    pub fn is_integer(self) -> bool {
        self != Self::Float32
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Format {
    pub channels: u16,
    pub sample_rate: u32,
    pub sample_format: SampleFormat,
    /// None uses WAVEFORMAT; Some(mask) uses WAVE_FORMAT_EXTENSIBLE, including mask 0.
    pub channel_mask: Option<u32>,
}
impl Format {
    pub fn block_align(self) -> Result<u16> {
        if self.channels == 0 || self.sample_rate == 0 {
            return Err(Error::invalid("WAVE 声道数和采样率必须大于零"));
        }
        self.channels
            .checked_mul(self.sample_format.bits() / 8)
            .ok_or_else(|| Error::unsupported("WAVE block alignment 超出 uint16"))
    }
    fn byte_rate(self) -> Result<u32> {
        self.sample_rate
            .checked_mul(u32::from(self.block_align()?))
            .ok_or_else(|| Error::unsupported("WAVE byte rate 超出 uint32"))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Info {
    pub container: Container,
    pub format: Format,
    /// Effective precision. Reduced-precision extensible PCM is left-aligned in its container.
    pub valid_bits: u16,
    pub frames: u64,
    pub data_bytes: u64,
}

fn add(a: u64, b: u64) -> Result<u64> {
    a.checked_add(b)
        .ok_or_else(|| Error::io("WAVE 长度或偏移溢出"))
}
fn bytes(length: u64) -> Result<Vec<u8>> {
    let length = usize::try_from(length).map_err(|_| Error::io("WAVE 缓冲区长度溢出"))?;
    let mut data = Vec::new();
    data.try_reserve_exact(length)
        .map_err(|_| Error::io("无法分配 WAVE 缓冲区"))?;
    data.resize(length, 0);
    Ok(data)
}
fn read_array<const N: usize>(source: &mut impl Read) -> Result<[u8; N]> {
    let mut data = [0; N];
    source.read_exact(&mut data)?;
    Ok(data)
}
fn read_u16(source: &mut impl Read) -> Result<u16> {
    Ok(u16::from_le_bytes(read_array(source)?))
}
fn read_u32(source: &mut impl Read) -> Result<u32> {
    Ok(u32::from_le_bytes(read_array(source)?))
}
fn read_u64(source: &mut impl Read) -> Result<u64> {
    Ok(u64::from_le_bytes(read_array(source)?))
}
fn write_chunk(out: &mut impl Write, id: [u8; 4], data: &[u8]) -> Result<()> {
    let size =
        u32::try_from(data.len()).map_err(|_| Error::unsupported("元数据 chunk 超过 uint32"))?;
    if size == u32::MAX {
        return Err(Error::unsupported("元数据大小使用保留标记"));
    }
    out.write_all(&id)?;
    out.write_all(&size.to_le_bytes())?;
    out.write_all(data)?;
    if size & 1 != 0 {
        out.write_all(&[0])?;
    }
    Ok(())
}
