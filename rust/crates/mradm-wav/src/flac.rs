//! Bounded FLAC container input for the native decoder's integrity pass.
//! Ogg pages are checked before their FLAC packet bytes reach libFLAC, including
//! builds whose libFLAC provider was compiled without libogg.
use std::{
    fs::File,
    io::{self, BufReader, Read, Seek, SeekFrom},
};

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

pub enum Input {
    Native(BufReader<File>),
    Ogg(Ogg<BufReader<File>>),
}

impl Input {
    pub fn open(path: &str) -> io::Result<Self> {
        let mut file = BufReader::new(File::open(path)?);
        let mut magic = [0; 4];
        file.read_exact(&mut magic)?;
        file.seek(SeekFrom::Start(0))?;
        match &magic {
            b"fLaC" => Ok(Self::Native(file)),
            b"OggS" => Ok(Self::Ogg(Ogg::new(file))),
            _ => Err(invalid("not a native or Ogg FLAC stream")),
        }
    }
    pub fn ogg_samples(&self) -> Option<u64> {
        match self {
            Self::Native(_) => None,
            Self::Ogg(ogg) => ogg.samples,
        }
    }
}

impl Read for Input {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        match self {
            Self::Native(input) => input.read(out),
            Self::Ogg(input) => input.read(out),
        }
    }
}

pub struct Ogg<R> {
    input: R,
    body: Vec<u8>,
    offset: usize,
    serial: Option<u32>,
    sequence: u32,
    continued: bool,
    ended: bool,
    samples: Option<u64>,
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = 0_u32;
    for &byte in bytes {
        crc ^= u32::from(byte) << 24;
        for _ in 0..8 {
            crc = (crc << 1)
                ^ if crc & 0x8000_0000 != 0 {
                    0x04c1_1db7
                } else {
                    0
                };
        }
    }
    crc
}

impl<R: Read> Ogg<R> {
    fn new(input: R) -> Self {
        Self {
            input,
            body: Vec::new(),
            offset: 0,
            serial: None,
            sequence: 0,
            continued: false,
            ended: false,
            samples: None,
        }
    }

    fn page(&mut self) -> io::Result<()> {
        let mut header = [0_u8; 27];
        self.input.read_exact(&mut header)?;
        if &header[..4] != b"OggS" || header[4] != 0 || header[5] & !7 != 0 {
            return Err(invalid("invalid Ogg page header"));
        }
        let serial = u32::from_le_bytes(header[14..18].try_into().unwrap());
        let sequence = u32::from_le_bytes(header[18..22].try_into().unwrap());
        let first = self.serial.is_none();
        if self.serial.is_some_and(|id| id != serial)
            || sequence != self.sequence
            || (header[5] & 2 != 0) != first
            || (header[5] & 1 != 0) != self.continued
        {
            return Err(invalid("missing, reordered or multiplexed Ogg pages"));
        }
        self.serial = Some(serial);
        self.sequence = self.sequence.wrapping_add(1);
        let mut lacing = vec![0_u8; usize::from(header[26])];
        self.input.read_exact(&mut lacing)?;
        let size: usize = lacing.iter().map(|&n| usize::from(n)).sum();
        // A page is at most 27 + 255 + 255*255 bytes, independent of file length.
        self.body.resize(size, 0);
        self.input.read_exact(&mut self.body)?;
        let expected = u32::from_le_bytes(header[22..26].try_into().unwrap());
        header[22..26].fill(0);
        let mut page = Vec::with_capacity(header.len() + lacing.len() + size);
        page.extend_from_slice(&header);
        page.extend_from_slice(&lacing);
        page.extend_from_slice(&self.body);
        if crc32(&page) != expected {
            return Err(invalid("Ogg page CRC mismatch"));
        }
        if let Some(&last) = lacing.last() {
            self.continued = last == 255;
        }
        self.ended = header[5] & 4 != 0;
        if self.ended {
            if self.continued {
                return Err(invalid("incomplete final Ogg packet"));
            }
            let samples = u64::from_le_bytes(header[6..14].try_into().unwrap());
            if samples == u64::MAX {
                return Err(invalid("missing final Ogg granule position"));
            }
            self.samples = Some(samples);
        }
        self.offset = 0;
        if first {
            if lacing != [51]
                || !self.body.starts_with(b"\x7fFLAC\x01\x00")
                || &self.body[9..13] != b"fLaC"
                || self.body[13] & 127 != 0
                || self.body[14..17] != [0, 0, 34]
            {
                return Err(invalid("invalid Ogg FLAC identification packet"));
            }
            self.offset = 9; // Strip the mapping header, retaining native fLaC + STREAMINFO.
        }
        Ok(())
    }
}

impl<R: Read> Read for Ogg<R> {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() {
            return Ok(0);
        }
        while self.offset == self.body.len() {
            if self.ended {
                let mut extra = [0];
                if self.input.read(&mut extra)? != 0 {
                    return Err(invalid("data after final Ogg page"));
                }
                return Ok(0);
            }
            self.page()?;
        }
        let count = out.len().min(self.body.len() - self.offset);
        out[..count].copy_from_slice(&self.body[self.offset..self.offset + count]);
        self.offset += count;
        Ok(count)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn page(body: &[u8], flags: u8, sequence: u32) -> Vec<u8> {
        assert!(body.len() < 255);
        let mut bytes = vec![0; 28];
        bytes[..4].copy_from_slice(b"OggS");
        bytes[5] = flags;
        bytes[18..22].copy_from_slice(&sequence.to_le_bytes());
        bytes[26] = 1;
        bytes[27] = body.len() as u8;
        bytes.extend_from_slice(body);
        let crc = crc32(&bytes);
        bytes[22..26].copy_from_slice(&crc.to_le_bytes());
        bytes
    }

    fn fixture() -> Vec<u8> {
        let mut mapping = b"\x7fFLAC\x01\x00\x00\x00fLaC\x80\x00\x00\x22".to_vec();
        mapping.resize(51, 0);
        page(&mapping, 6, 0)
    }

    #[test]
    fn checks_ogg_pages_and_exact_end() {
        let valid = fixture();
        let mut out = Vec::new();
        Ogg::new(Cursor::new(&valid)).read_to_end(&mut out).unwrap();
        assert_eq!(out.len(), 42);
        assert_eq!(&out[..4], b"fLaC");
        for cut in 0..valid.len() {
            assert!(
                Ogg::new(Cursor::new(&valid[..cut]))
                    .read_to_end(&mut Vec::new())
                    .is_err()
            );
        }
        let mut corrupt = valid.clone();
        *corrupt.last_mut().unwrap() ^= 1;
        assert!(
            Ogg::new(Cursor::new(corrupt))
                .read_to_end(&mut Vec::new())
                .is_err()
        );
        let mut extra = valid.clone();
        extra.push(0);
        assert!(
            Ogg::new(Cursor::new(extra))
                .read_to_end(&mut Vec::new())
                .is_err()
        );
        let mut reordered = valid;
        reordered[18] = 1;
        assert!(
            Ogg::new(Cursor::new(reordered))
                .read_to_end(&mut Vec::new())
                .is_err()
        );
    }

    #[test]
    fn handles_continued_packets_without_buffering_the_whole_packet() {
        let mut identification = fixture();
        identification[5] = 2;
        identification[22..26].fill(0);
        let crc = crc32(&identification);
        identification[22..26].copy_from_slice(&crc.to_le_bytes());
        let mut fragment = page(&[1; 254], 0, 1);
        fragment.push(1);
        fragment[27] = 255;
        fragment[22..26].fill(0);
        let crc = crc32(&fragment);
        fragment[22..26].copy_from_slice(&crc.to_le_bytes());
        let final_page = page(&[2, 3], 5, 2);
        let mut data = identification.clone();
        data.extend_from_slice(&fragment);
        data.extend_from_slice(&final_page);
        let mut reader = Ogg::new(Cursor::new(data));
        let mut out = Vec::new();
        let mut small = [0; 7];
        loop {
            let n = reader.read(&mut small).unwrap();
            if n == 0 {
                break;
            }
            out.extend_from_slice(&small[..n]);
        }
        assert_eq!(&out[42..297], &[1; 255]);
        assert_eq!(&out[297..], &[2, 3]);
        // Valid page CRCs cannot hide a missing continuation flag or page number.
        for broken in [page(&[2, 3], 4, 2), page(&[2, 3], 5, 3)] {
            let data = [
                identification.as_slice(),
                fragment.as_slice(),
                broken.as_slice(),
            ]
            .concat();
            assert!(
                Ogg::new(Cursor::new(data))
                    .read_to_end(&mut Vec::new())
                    .is_err()
            );
        }
    }
}
