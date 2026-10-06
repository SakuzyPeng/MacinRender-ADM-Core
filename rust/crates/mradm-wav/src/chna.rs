use crate::{Error, Result};

/// Fixed-width identifiers are bytes, not Unicode strings. Preserve padding on read/write.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChnaEntry {
    pub track_index: u16,
    pub uid: [u8; 12],
    pub track_format: [u8; 14],
    pub pack_format: [u8; 11],
    pub pad: u8,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Chna {
    pub tracks: u16,
    pub entries: Vec<ChnaEntry>,
}
impl Chna {
    pub fn decode(data: &[u8]) -> Result<Self> {
        if data.len() < 4 {
            return Err(Error::io("CHNA 头被截断"));
        }
        let tracks = u16::from_le_bytes([data[0], data[1]]);
        let count = usize::from(u16::from_le_bytes([data[2], data[3]]));
        if data.len() != 4 + count * 40 {
            return Err(Error::io("CHNA 条目数与长度不一致"));
        }
        let mut entries = Vec::with_capacity(count);
        for record in data[4..].as_chunks::<40>().0 {
            let track_index = u16::from_le_bytes([record[0], record[1]]);
            if track_index > tracks {
                return Err(Error::io("CHNA track index 超出轨道数"));
            }
            // Index zero denotes an unused entry; more than one UID may reference one track.
            let mut uid = [0; 12];
            uid.copy_from_slice(&record[2..14]);
            let mut track_format = [0; 14];
            track_format.copy_from_slice(&record[14..28]);
            let mut pack_format = [0; 11];
            pack_format.copy_from_slice(&record[28..39]);
            entries.push(ChnaEntry {
                track_index,
                uid,
                track_format,
                pack_format,
                pad: record[39],
            });
        }
        Ok(Self { tracks, entries })
    }
    pub fn encode(&self) -> Result<Vec<u8>> {
        let count =
            u16::try_from(self.entries.len()).map_err(|_| Error::invalid("CHNA 条目数超限"))?;
        let mut data = Vec::with_capacity(4 + self.entries.len() * 40);
        data.extend_from_slice(&self.tracks.to_le_bytes());
        data.extend_from_slice(&count.to_le_bytes());
        for entry in &self.entries {
            if entry.track_index > self.tracks {
                return Err(Error::invalid("CHNA track index 超出轨道数"));
            }
            data.extend_from_slice(&entry.track_index.to_le_bytes());
            data.extend_from_slice(&entry.uid);
            data.extend_from_slice(&entry.track_format);
            data.extend_from_slice(&entry.pack_format);
            data.push(entry.pad);
        }
        Ok(data)
    }
}
