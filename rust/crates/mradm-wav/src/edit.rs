//! Container-level edits of finished WAVE files: BWF metadata append, layout rewrite, chunk
//! replacement and a tolerant chunk probe. Sample bytes are copied verbatim, never decoded.
use crate::{
    Chna, ChnaEntry, ChunkReader, Container, Error, Reader, Result, SampleFormat, add, bytes,
};
use std::io::{Read, Seek, SeekFrom, Write};

/// Fixed BWF v2 `bext` payload without coding history (EBU Tech 3285).
pub const BEXT_SIZE: usize = 602;
/// AmbiX marker for third-order HOA: B-format, ACN ordering, SN3D normalisation, 16 channels.
pub const AMBI_HOA3: [u8; 16] = [1, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 16, 0, 0, 0];
const NOT_INDICATED: i16 = 0x7fff;

pub struct BextFields<'a> {
    pub description: &'a [u8],
    pub originator: &'a [u8],
    pub originator_reference: &'a [u8],
    /// ISO 8601 UTC such as `2026-05-22T14:30:00Z`; shorter text leaves date/time zeroed.
    pub date_utc: &'a [u8],
    pub loudness: Option<f64>,
    pub true_peak: Option<f64>,
}

fn bext_text(field: &mut [u8], value: &[u8]) {
    // NUL-terminated within the field, matching the previous C++ writer.
    let length = value.len().min(field.len() - 1);
    field[..length].copy_from_slice(&value[..length]);
}

/// Hundredths as stored by BWF v2. Like the former `lround` cast, out-of-range values wrap;
/// non-finite measurements are written as not indicated.
fn bext_level(value: Option<f64>) -> i16 {
    match value {
        Some(value) if value.is_finite() => (value * 100.0).round() as i64 as i16,
        _ => NOT_INDICATED,
    }
}

pub fn bext_payload(fields: &BextFields) -> [u8; BEXT_SIZE] {
    let mut out = [0; BEXT_SIZE];
    bext_text(&mut out[0..256], fields.description);
    bext_text(&mut out[256..288], fields.originator);
    bext_text(&mut out[288..320], fields.originator_reference);
    let date = fields.date_utc;
    if date.len() >= 10 {
        out[320..330].copy_from_slice(&date[..10]);
    }
    if date.len() >= 19 {
        // ISO `Thh:mm:ss` becomes the EBU `hh-mm-ss` form.
        out[330..338].copy_from_slice(&[
            date[11], date[12], b'-', date[14], date[15], b'-', date[17], date[18],
        ]);
    }
    // TimeReference stays zero; Version 2 carries the loudness fields; UMID stays zero.
    out[346..348].copy_from_slice(&2u16.to_le_bytes());
    let levels = [
        bext_level(fields.loudness),
        NOT_INDICATED,
        bext_level(fields.true_peak),
        NOT_INDICATED,
        NOT_INDICATED,
    ];
    for (slot, level) in out[412..422].as_chunks_mut::<2>().0.iter_mut().zip(levels) {
        slot.copy_from_slice(&level.to_le_bytes());
    }
    out
}

/// Append `bext` (and optionally write `ambi`, in place when one exists) and update the RIFF size
/// or `ds64.bw64Size`. All checks run before the first byte is written.
pub fn append_bext<F: Read + Write + Seek>(
    file: &mut F,
    bext: &[u8; BEXT_SIZE],
    ambi: Option<&[u8; 16]>,
) -> Result<()> {
    let physical_end = file.seek(SeekFrom::End(0))?;
    let reader = ChunkReader::new(&mut *file)?;
    let container = reader.container();
    let end = reader.end();
    let existing_ambi = reader.chunks().iter().find(|c| c.id == *b"ambi").copied();
    let ds64 = reader.chunks().iter().find(|c| c.id == *b"ds64").copied();
    drop(reader);
    if end != physical_end {
        return Err(Error::io("WAVE 文件末尾存在容器外数据"));
    }
    if existing_ambi.is_some_and(|chunk| ambi.is_some() && chunk.size != 16) {
        return Err(Error::io("已有 ambi chunk 长度无效"));
    }
    let appended = 8
        + BEXT_SIZE as u64
        + if ambi.is_some() && existing_ambi.is_none() {
            24
        } else {
            0
        };
    let final_end = add(end, appended)?;
    if container == Container::Riff && final_end - 8 > u64::from(u32::MAX) {
        return Err(Error::unsupported(
            "RIFF WAVE 追加元数据后超过 4GB；请使用 RF64/BW64 输出",
        ));
    }
    file.seek(SeekFrom::Start(end))?;
    file.write_all(b"bext")?;
    file.write_all(&(BEXT_SIZE as u32).to_le_bytes())?;
    file.write_all(bext)?;
    if let Some(payload) = ambi {
        match existing_ambi {
            Some(chunk) => {
                file.seek(SeekFrom::Start(chunk.offset))?;
            }
            None => {
                file.write_all(b"ambi")?;
                file.write_all(&16u32.to_le_bytes())?;
            }
        }
        file.write_all(payload)?;
    }
    match (container, ds64) {
        (Container::Riff, _) => {
            file.seek(SeekFrom::Start(4))?;
            file.write_all(&((final_end - 8) as u32).to_le_bytes())?;
        }
        (_, Some(chunk)) => {
            file.seek(SeekFrom::Start(chunk.offset))?;
            file.write_all(&(final_end - 8).to_le_bytes())?;
        }
        (_, None) => return Err(Error::io("RF64/BW64 缺少 ds64")),
    }
    file.flush()?;
    Ok(())
}

/// One CHNA assignment as identifiers; fields are space padded to their fixed widths.
pub struct ChnaTrack<'a> {
    pub track_index: u16,
    pub uid: &'a [u8],
    pub track_format: &'a [u8],
    pub pack_format: &'a [u8],
}

pub struct LayoutOptions<'a> {
    pub channel_mask: u32,
    /// WAVE_FORMAT_EXTENSIBLE even with mask 0 (positions explicitly unspecified).
    pub force_extensible: bool,
    pub include_pcm_fact: bool,
    pub prefer_riff: bool,
    /// File channel -> source channel; empty means identity.
    pub permutation: &'a [u16],
    /// Non-empty AXML and CHNA label the output as ADM; integer PCM then becomes BW64.
    pub axml: &'a [u8],
    pub chna: &'a [ChnaTrack<'a>],
}

fn padded<const N: usize>(value: &[u8]) -> [u8; N] {
    let mut field = [b' '; N];
    field[..value.len()].copy_from_slice(value);
    field
}

fn chna_payload(channels: u16, tracks: &[ChnaTrack]) -> Result<Vec<u8>> {
    if tracks.len() > usize::from(u16::MAX) {
        return Err(Error::unsupported("WAVE CHNA 条目数超过 65535"));
    }
    let mut used = vec![false; usize::from(channels)];
    let mut entries = Vec::with_capacity(tracks.len());
    for track in tracks {
        if track.track_index == 0 || track.track_index > channels {
            return Err(Error::invalid(format!(
                "CHNA track index {} 超出 1..{}",
                track.track_index, channels
            )));
        }
        let slot = &mut used[usize::from(track.track_index - 1)];
        if *slot {
            return Err(Error::invalid(format!(
                "CHNA track index {} 重复",
                track.track_index
            )));
        }
        *slot = true;
        if track.uid.len() > 12 || track.track_format.len() > 14 || track.pack_format.len() > 11 {
            return Err(Error::invalid(format!(
                "track {} 的 CHNA 标识超出固定字段宽度",
                track.track_index
            )));
        }
        entries.push(ChnaEntry {
            track_index: track.track_index,
            uid: padded(track.uid),
            track_format: padded(track.track_format),
            pack_format: padded(track.pack_format),
            pad: b' ',
        });
    }
    Chna {
        tracks: channels,
        entries,
    }
    .encode()
}

fn disk_size(payload: u64) -> Result<u64> {
    add(8, add(payload, payload & 1)?)
}

fn put_chunk(out: &mut Vec<u8>, id: &[u8; 4], size: u32) {
    out.extend_from_slice(id);
    out.extend_from_slice(&size.to_le_bytes());
}

/// Streams a finished WAVE into a new file with a different channel order, speaker mask,
/// container and ADM chunks. Call `step` until it returns 0, then `finish`.
pub struct LayoutRewriter<R, W> {
    source: R,
    sink: W,
    permutation: Vec<u16>,
    width: usize,
    frames: u64,
    done: u64,
    odd_data: bool,
    axml: Vec<u8>,
    input: Vec<u8>,
    output: Vec<u8>,
}
impl<R: Read + Seek, W: Write> LayoutRewriter<R, W> {
    pub fn new(source: R, mut sink: W, options: &LayoutOptions) -> Result<Self> {
        let reader = Reader::new(source)?;
        let info = reader.info();
        let data = reader
            .chunks()
            .iter()
            .find(|c| c.id == *b"data")
            .copied()
            .ok_or_else(|| Error::io("缺少 data chunk"))?;
        let mut source = reader.into_inner();
        let format = info.format;
        let channels = format.channels;
        if info.frames == 0 {
            return Err(Error::invalid("WAVE 布局收尾需要非空音频"));
        }
        if options.channel_mask != 0 && options.channel_mask.count_ones() != u32::from(channels) {
            return Err(Error::invalid(format!(
                "WAVE 声道掩码 0x{:X} 有 {} 个位置，但文件有 {} 声道",
                options.channel_mask,
                options.channel_mask.count_ones(),
                channels
            )));
        }
        let permutation = if options.permutation.is_empty() {
            (0..channels).collect()
        } else {
            if options.permutation.len() != usize::from(channels) {
                return Err(Error::invalid(format!(
                    "WAVE 声道置换有 {} 项，但文件有 {} 声道",
                    options.permutation.len(),
                    channels
                )));
            }
            let mut used = vec![false; usize::from(channels)];
            for &source in options.permutation {
                let slot = used
                    .get_mut(usize::from(source))
                    .filter(|used| !**used)
                    .ok_or_else(|| Error::invalid("WAVE 声道置换不是双射"))?;
                *slot = true;
            }
            options.permutation.to_vec()
        };
        let has_axml = !options.axml.is_empty();
        if has_axml != !options.chna.is_empty() {
            return Err(Error::invalid("ADM WAVE 收尾需要同时提供 AXML 与 CHNA"));
        }
        let chna = chna_payload(channels, options.chna)?;
        if options.axml.len() as u64 > u64::from(u32::MAX) - 1
            || chna.len() as u64 > u64::from(u32::MAX) - 1
        {
            return Err(Error::unsupported("ADM 元数据 chunk 超过 4GB"));
        }

        let is_float = format.sample_format == SampleFormat::Float32;
        let extensible = options.channel_mask != 0 || options.force_extensible;
        let include_fact = is_float || options.include_pcm_fact;
        let mut body = add(4, disk_size(if extensible { 40 } else { 16 })?)?;
        if include_fact {
            body = add(body, disk_size(4)?)?;
        }
        if has_axml {
            body = add(body, disk_size(chna.len() as u64)?)?;
            body = add(body, disk_size(options.axml.len() as u64)?)?;
        }
        body = add(body, disk_size(info.data_bytes)?)?;
        let bw64 = has_axml && !is_float;
        let wide = bw64
            || (!options.prefer_riff && info.container != Container::Riff)
            || body > u64::from(u32::MAX);
        let riff_size = if wide {
            add(body, disk_size(28)?)?
        } else {
            body
        };

        let bits = format.sample_format.bits();
        let align = format.block_align()?;
        let byte_rate = u64::from(format.sample_rate) * u64::from(align);
        let byte_rate = u32::try_from(byte_rate)
            .map_err(|_| Error::unsupported("WAVE byte rate 超出 uint32"))?;
        let tag: u16 = if is_float { 3 } else { 1 };
        let mut header = Vec::with_capacity(128 + chna.len());
        header.extend_from_slice(match (bw64, wide) {
            (true, _) => b"BW64",
            (false, true) => b"RF64",
            (false, false) => b"RIFF",
        });
        header.extend_from_slice(&(if wide { u32::MAX } else { riff_size as u32 }).to_le_bytes());
        header.extend_from_slice(b"WAVE");
        if wide {
            put_chunk(&mut header, b"ds64", 28);
            header.extend_from_slice(&riff_size.to_le_bytes());
            header.extend_from_slice(&info.data_bytes.to_le_bytes());
            header.extend_from_slice(&info.frames.to_le_bytes());
            header.extend_from_slice(&0u32.to_le_bytes());
        }
        put_chunk(&mut header, b"fmt ", if extensible { 40 } else { 16 });
        header.extend_from_slice(&(if extensible { 0xfffe } else { tag }).to_le_bytes());
        header.extend_from_slice(&channels.to_le_bytes());
        header.extend_from_slice(&format.sample_rate.to_le_bytes());
        header.extend_from_slice(&byte_rate.to_le_bytes());
        header.extend_from_slice(&align.to_le_bytes());
        header.extend_from_slice(&bits.to_le_bytes());
        if extensible {
            header.extend_from_slice(&22u16.to_le_bytes());
            header.extend_from_slice(&bits.to_le_bytes());
            header.extend_from_slice(&options.channel_mask.to_le_bytes());
            header.extend_from_slice(&tag.to_le_bytes());
            header.extend_from_slice(&[0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113]);
        }
        if has_axml {
            put_chunk(&mut header, b"chna", chna.len() as u32);
            header.extend_from_slice(&chna);
            if chna.len() & 1 != 0 {
                header.push(0);
            }
        }
        if include_fact {
            put_chunk(&mut header, b"fact", 4);
            header.extend_from_slice(&(info.frames.min(u64::from(u32::MAX)) as u32).to_le_bytes());
        }
        let data_size = if wide {
            u32::MAX
        } else {
            u32::try_from(info.data_bytes).map_err(|_| Error::unsupported("RIFF data 超过 4GB"))?
        };
        put_chunk(&mut header, b"data", data_size);
        sink.write_all(&header)?;
        source.seek(SeekFrom::Start(data.offset))?;
        Ok(Self {
            source,
            sink,
            permutation,
            width: usize::from(bits / 8),
            frames: info.frames,
            done: 0,
            odd_data: info.data_bytes & 1 != 0,
            axml: options.axml.to_vec(),
            input: Vec::new(),
            output: Vec::new(),
        })
    }
    pub fn frames(&self) -> u64 {
        self.frames
    }
    /// Copy up to `max_frames` frames; returns the number copied, 0 once all frames are written.
    pub fn step(&mut self, max_frames: u64) -> Result<u64> {
        let count = max_frames.min(self.frames - self.done);
        if count == 0 {
            return Ok(0);
        }
        let frame = self.permutation.len() * self.width;
        let size = usize::try_from(count)
            .ok()
            .and_then(|count| count.checked_mul(frame))
            .ok_or_else(|| Error::invalid("WAVE 布局块过大"))?;
        if self.input.len() != size {
            self.input = bytes(size as u64)?;
            self.output = bytes(size as u64)?;
        }
        self.source.read_exact(&mut self.input)?;
        let width = self.width;
        for (input, output) in self
            .input
            .chunks_exact(frame)
            .zip(self.output.chunks_exact_mut(frame))
        {
            for (slot, &source) in output.chunks_exact_mut(width).zip(&self.permutation) {
                let at = usize::from(source) * width;
                slot.copy_from_slice(&input[at..at + width]);
            }
        }
        self.sink.write_all(&self.output)?;
        self.done += count;
        Ok(count)
    }
    pub fn finish(mut self) -> Result<W> {
        if self.done != self.frames {
            return Err(Error::invalid("WAVE 布局重写尚未复制全部样本"));
        }
        if self.odd_data {
            self.sink.write_all(&[0])?;
        }
        if !self.axml.is_empty() {
            put_axml(&mut self.sink, &self.axml)?;
        }
        self.sink.flush()?;
        Ok(self.sink)
    }
}

fn put_axml(sink: &mut impl Write, axml: &[u8]) -> Result<()> {
    sink.write_all(b"axml")?;
    sink.write_all(&(axml.len() as u32).to_le_bytes())?;
    sink.write_all(axml)?;
    if axml.len() & 1 != 0 {
        sink.write_all(&[0])?;
    }
    Ok(())
}

/// Copy every chunk verbatim except `id`, whose payload becomes `payload`; the container tag is
/// kept and the RIFF size or `ds64.bw64Size` is recomputed. PCM bytes are never re-encoded.
pub fn replace_chunk<R: Read + Seek, W: Write>(
    source: R,
    mut sink: W,
    id: [u8; 4],
    payload: &[u8],
) -> Result<()> {
    let reader = ChunkReader::new(source)?;
    let container = reader.container();
    let chunks = reader.chunks().to_vec();
    let mut source = reader.into_inner();
    let mut body = 4u64;
    let mut found = false;
    let wide = container != Container::Riff;
    let mut fields = Vec::with_capacity(chunks.len());
    for chunk in &chunks {
        let size = if chunk.id == id {
            found = true;
            payload.len() as u64
        } else {
            chunk.size
        };
        fields.push(match u32::try_from(size) {
            Ok(size) if size != u32::MAX => size,
            _ if wide && chunk.id == *b"data" => u32::MAX,
            _ => return Err(Error::unsupported("非 data chunk 超过 4GB，暂不支持")),
        });
        body = add(body, disk_size(size)?)?;
    }
    if !found {
        return Err(Error::io(format!(
            "源文件缺少 {} chunk",
            String::from_utf8_lossy(&id)
        )));
    }
    if !wide && body > u64::from(u32::MAX) {
        return Err(Error::unsupported(
            "输出超过 4GB 但源非 BW64/RF64，暂不支持自动升级",
        ));
    }
    let mut ds64 = Vec::new();
    if let Some(chunk) = chunks.iter().find(|c| c.id == *b"ds64") {
        ds64 = bytes(chunk.size)?;
        source.seek(SeekFrom::Start(chunk.offset))?;
        source.read_exact(&mut ds64)?;
        // Only bw64Size is rewritten; a size table for other >4GB chunks would go stale.
        // Realistic ADM BWF files leave it empty.
        if u32::from_le_bytes([ds64[24], ds64[25], ds64[26], ds64[27]]) != 0 {
            return Err(Error::unsupported(
                "ds64 size table 非空（含超过 4GB 的非 data chunk），写回暂不支持",
            ));
        }
        ds64[..8].copy_from_slice(&body.to_le_bytes());
    }
    sink.write_all(match container {
        Container::Riff => b"RIFF",
        Container::Rf64 => b"RF64",
        Container::Bw64 => b"BW64",
    })?;
    sink.write_all(&(if wide { u32::MAX } else { body as u32 }).to_le_bytes())?;
    sink.write_all(b"WAVE")?;
    let mut buffer = bytes(1 << 16)?;
    for (chunk, field) in chunks.iter().zip(fields) {
        sink.write_all(&chunk.id)?;
        sink.write_all(&field.to_le_bytes())?;
        let size = if chunk.id == id {
            sink.write_all(payload)?;
            payload.len() as u64
        } else if chunk.id == *b"ds64" {
            sink.write_all(&ds64)?;
            chunk.size
        } else {
            source.seek(SeekFrom::Start(chunk.offset))?;
            let mut remaining = chunk.size;
            while remaining > 0 {
                let part = remaining.min(buffer.len() as u64) as usize;
                source.read_exact(&mut buffer[..part])?;
                sink.write_all(&buffer[..part])?;
                remaining -= part as u64;
            }
            chunk.size
        };
        if size & 1 != 0 {
            sink.write_all(&[0])?;
        }
    }
    sink.flush()?;
    Ok(())
}

/// Tolerant top-level scan used to route inputs before strict decoding: reports whether a chunk
/// with `id` exists. Errors only when the file is not RIFF/RF64/BW64 WAVE; a truncated chunk
/// table simply ends the scan.
pub fn has_chunk<R: Read + Seek>(mut source: R, id: [u8; 4]) -> Result<bool> {
    let end = source.seek(SeekFrom::End(0))?;
    source.seek(SeekFrom::Start(0))?;
    let mut header = [0; 12];
    source
        .read_exact(&mut header)
        .map_err(|_| Error::unsupported("不是 RIFF/RF64/BW64 WAVE 文件"))?;
    if !matches!(&header[..4], b"RIFF" | b"RF64" | b"BW64") || &header[8..] != b"WAVE" {
        return Err(Error::unsupported("不是 RIFF/RF64/BW64 WAVE 文件"));
    }
    let mut long_data = 0u64;
    let mut at = 12u64;
    while at.checked_add(8).is_some_and(|next| next <= end) {
        source.seek(SeekFrom::Start(at))?;
        let mut chunk = [0; 8];
        if source.read_exact(&mut chunk).is_err() {
            break;
        }
        let chunk_id = [chunk[0], chunk[1], chunk[2], chunk[3]];
        if chunk_id == id {
            return Ok(true);
        }
        let short = u32::from_le_bytes([chunk[4], chunk[5], chunk[6], chunk[7]]);
        let mut size = u64::from(short);
        if chunk_id == *b"ds64" && short >= 24 {
            let mut sizes = [0; 16];
            if source.read_exact(&mut sizes).is_err() {
                break;
            }
            long_data = u64::from_le_bytes(sizes[8..].try_into().expect("8 bytes"));
        } else if chunk_id == *b"data" && short == u32::MAX && long_data > 0 {
            size = long_data;
        }
        match size
            .checked_add(size & 1)
            .and_then(|size| size.checked_add(at + 8))
        {
            Some(next) => at = next,
            None => break,
        }
    }
    Ok(false)
}
