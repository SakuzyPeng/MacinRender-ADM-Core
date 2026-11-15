//! Test-only RIFF / PCM author (formerly `tests/support/wav_fixture.h`). It deliberately shares no
//! code with `macin-bwf`, so a fixture cannot inherit a bug of the reader it tests.
use std::io;
use std::path::Path;

/// One `chna` entry.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AudioId {
    pub track: u16,
    pub uid: String,
    pub format: String,
    pub pack: String,
}

impl AudioId {
    pub fn new(track: u16, uid: &str, format: &str, pack: &str) -> Self {
        Self {
            track,
            uid: uid.to_owned(),
            format: format.to_owned(),
            pack: pack.to_owned(),
        }
    }
}

/// A small RIFF WAVE file with integer PCM, optional `chna` and `axml` chunks.
#[derive(Clone, Debug)]
pub struct Wave {
    pub channels: u16,
    pub rate: u32,
    /// 16, 24 or 32.
    pub bits: u16,
    pub chna: Option<Vec<AudioId>>,
    pub axml: Option<String>,
}

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}

fn fixed(out: &mut Vec<u8>, text: &str, width: usize) -> io::Result<()> {
    if text.len() > width {
        return Err(invalid("fixture CHNA identifier too long"));
    }
    out.extend_from_slice(text.as_bytes());
    out.resize(out.len() + width - text.len(), b' ');
    Ok(())
}

/// The sample as the C++ author quantised it: clamped to [-1, 1], scaled in `f64` by
/// `2^(bits-1) - 1` and truncated toward zero.
pub fn quantise(sample: f32, bits: u16) -> io::Result<i32> {
    if sample.is_nan() {
        return Err(invalid("NaN fixture sample"));
    }
    let maximum = ((1u64 << (bits - 1)) - 1) as f64;
    Ok((f64::from(sample.clamp(-1.0, 1.0)) * maximum) as i32)
}

impl Wave {
    pub fn new(channels: u16, rate: u32, bits: u16) -> Self {
        Self {
            channels,
            rate,
            bits,
            chna: None,
            axml: None,
        }
    }

    /// The file bytes of `samples` (interleaved).
    pub fn bytes(&self, samples: &[f32]) -> io::Result<Vec<u8>> {
        let (channels, rate, bits) = (self.channels, self.rate, self.bits);
        if channels == 0 || rate == 0 || !matches!(bits, 16 | 24 | 32) {
            return Err(invalid("invalid fixture WAVE format"));
        }
        let align = u32::from(channels) * u32::from(bits / 8);
        if align > u32::from(u16::MAX) || u64::from(align) * u64::from(rate) > u64::from(u32::MAX) {
            return Err(invalid("fixture WAVE format overflow"));
        }
        if !samples.len().is_multiple_of(usize::from(channels)) {
            return Err(invalid("fixture samples are not whole frames"));
        }
        let mut out = Vec::new();
        out.extend_from_slice(b"RIFF\0\0\0\0WAVEfmt ");
        out.extend_from_slice(&16u32.to_le_bytes());
        out.extend_from_slice(&1u16.to_le_bytes());
        out.extend_from_slice(&channels.to_le_bytes());
        out.extend_from_slice(&rate.to_le_bytes());
        out.extend_from_slice(&(align * rate).to_le_bytes());
        out.extend_from_slice(&(align as u16).to_le_bytes());
        out.extend_from_slice(&bits.to_le_bytes());
        if let Some(chna) = &self.chna {
            let count =
                u16::try_from(chna.len()).map_err(|_| invalid("too many fixture CHNA entries"))?;
            out.extend_from_slice(b"chna");
            out.extend_from_slice(&(4 + u32::from(count) * 40).to_le_bytes());
            let tracks = chna.iter().map(|e| e.track).max().unwrap_or(0);
            out.extend_from_slice(&tracks.to_le_bytes());
            out.extend_from_slice(&count.to_le_bytes());
            for e in chna {
                out.extend_from_slice(&e.track.to_le_bytes());
                fixed(&mut out, &e.uid, 12)?;
                fixed(&mut out, &e.format, 14)?;
                fixed(&mut out, &e.pack, 11)?;
                out.push(0);
            }
        }
        if let Some(axml) = &self.axml {
            let len = u32::try_from(axml.len())
                .ok()
                .filter(|&l| l < u32::MAX)
                .ok_or_else(|| invalid("fixture AXML too large"))?;
            out.extend_from_slice(b"axml");
            out.extend_from_slice(&len.to_le_bytes());
            out.extend_from_slice(axml.as_bytes());
            if len & 1 != 0 {
                out.push(0);
            }
        }
        out.extend_from_slice(b"data\0\0\0\0");
        let start = out.len() as u64;
        let width = usize::from(bits / 8);
        let length = (samples.len() * width) as u64;
        if start + length >= u64::from(u32::MAX) {
            return Err(invalid("fixture author only supports small RIFF files"));
        }
        out.reserve(samples.len() * width + 1);
        for &s in samples {
            out.extend_from_slice(&quantise(s, bits)?.to_le_bytes()[..width]);
        }
        if length & 1 != 0 {
            out.push(0);
        }
        let riff = (start + length + (length & 1) - 8) as u32;
        out[4..8].copy_from_slice(&riff.to_le_bytes());
        let data = start as usize - 4;
        out[data..data + 4].copy_from_slice(&(length as u32).to_le_bytes());
        Ok(out)
    }

    /// Writes the file (replacing an existing one).
    pub fn write(&self, path: &Path, samples: &[f32]) -> io::Result<()> {
        std::fs::write(path, self.bytes(samples)?)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quantisation_truncates_toward_zero() {
        assert_eq!(quantise(1.0, 24).unwrap(), 8_388_607);
        assert_eq!(quantise(-2.0, 24).unwrap(), -8_388_607);
        assert_eq!(quantise(0.5, 16).unwrap(), 16383);
        assert_eq!(quantise(-0.5, 16).unwrap(), -16383);
        assert!(quantise(f32::NAN, 24).is_err());
    }

    #[test]
    fn odd_payloads_are_padded() {
        let mut w = Wave::new(1, 48000, 24);
        w.axml = Some("abc".to_owned());
        let bytes = w.bytes(&[0.0]).unwrap();
        // 12 RIFF/WAVE + 24 fmt + 8 + 3 + 1 axml + 8 + 3 + 1 data.
        assert_eq!(bytes.len(), 60);
        assert_eq!(u32::from_le_bytes(bytes[4..8].try_into().unwrap()), 52);
        assert_eq!(&bytes[48..52], b"data");
        assert_eq!(u32::from_le_bytes(bytes[52..56].try_into().unwrap()), 3);
    }
}
