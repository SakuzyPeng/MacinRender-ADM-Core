//! Shared by the consistency tools `consistency_render`, `consistency_stream` and
//! `consistency_scene`: the failure type and the canonical PCM wire data (formerly
//! `tests/tools/phase2_io.h`).
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

pub type Failure = String;

pub fn require(ok: bool, message: &str) -> Result<(), Failure> {
    if ok { Ok(()) } else { Err(message.to_owned()) }
}

/// The former C++ `phase2::pcm`: measurement-only canonical wire data, creating the directory.
pub fn pcm(path: &Path, channels: u32, rate: u32, values: &[f32]) -> Result<(), Failure> {
    require(
        channels != 0 && rate != 0 && values.len().is_multiple_of(channels as usize),
        "invalid PCM shape",
    )?;
    let io = |e: std::io::Error| e.to_string();
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        std::fs::create_dir_all(parent).map_err(io)?;
    }
    let mut out = BufWriter::new(File::create(path).map_err(io)?);
    out.write_all(b"MRPB").map_err(io)?;
    out.write_all(&1u32.to_le_bytes()).map_err(io)?;
    out.write_all(&channels.to_le_bytes()).map_err(io)?;
    out.write_all(&rate.to_le_bytes()).map_err(io)?;
    out.write_all(&((values.len() / channels as usize) as u64).to_le_bytes())
        .map_err(io)?;
    for value in values {
        require(value.is_finite(), "non-finite replay PCM")?;
        out.write_all(&value.to_bits().to_le_bytes()).map_err(io)?;
    }
    out.flush().map_err(io)
}
