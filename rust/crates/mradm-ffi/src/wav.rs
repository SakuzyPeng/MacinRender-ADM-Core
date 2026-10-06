//! Private file I/O boundary. Owners and their allocations never cross allocators.
use mradm_wav::{
    AMBI_HOA3, BextFields, Chna, ChnaTrack, ChunkReader, Container, Error, Format, LayoutOptions,
    LayoutRewriter, Reader, Result, SampleFormat, Writer, WriterOptions, append_bext, bext_payload,
    has_chunk, replace_chunk,
};
use std::{
    fs::{File, OpenOptions},
    io::{BufReader, BufWriter, Write},
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice, str,
};

pub struct ReadHandle {
    reader: Reader<BufReader<File>>,
}
pub struct ChunkReadHandle {
    reader: ChunkReader<BufReader<File>>,
}
struct CreatedPathGuard<'a> {
    path: &'a str,
    keep: bool,
}
impl Drop for CreatedPathGuard<'_> {
    fn drop(&mut self) {
        if !self.keep {
            let _ = std::fs::remove_file(self.path);
        }
    }
}
pub struct WriteHandle {
    writer: Option<Writer<File>>,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Info {
    frames: u64,
    channels: u32,
    rate: u32,
    /// WAVE_FORMAT_EXTENSIBLE speaker mask; 0 for plain WAVEFORMAT headers.
    channel_mask: u32,
    /// Container bits per sample (fmt `wBitsPerSample`).
    bits: u16,
    /// Format tag after resolving the extensible sub-format: 1 = PCM, 3 = IEEE float.
    format: u16,
}

fn boundary(message: *mut u8, capacity: usize, f: impl FnOnce() -> Result<()>) -> i32 {
    let error = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(error)) => Some(error),
        Err(_) => Some(Error {
            code: 6,
            message: "Rust WAVE panic".into(),
        }),
    };
    let text = error.as_ref().map_or("", |error| error.message.as_str());
    if !message.is_null() && capacity != 0 {
        let mut len = text.len().min(capacity - 1);
        while !text.is_char_boundary(len) {
            len -= 1;
        }
        unsafe {
            ptr::copy_nonoverlapping(text.as_ptr(), message, len);
            *message.add(len) = 0;
        }
    }
    error.map_or(0, |error| error.code)
}
fn valid<T>(pointer: *const T, len: usize) -> Result<()> {
    if len != 0
        && (pointer.is_null()
            || !(pointer as usize).is_multiple_of(align_of::<T>())
            || len > isize::MAX as usize / size_of::<T>())
    {
        return Err(Error::invalid("无效 WAVE 句柄或缓冲区"));
    }
    Ok(())
}
unsafe fn path<'a>(data: *const u8, len: usize) -> Result<&'a str> {
    valid(data, len)?;
    if len == 0 {
        return Err(Error::invalid("WAVE 路径为空"));
    }
    let text = str::from_utf8(unsafe { slice::from_raw_parts(data, len) })
        .map_err(|_| Error::invalid("WAVE 路径必须使用 UTF-8"))?;
    if text.contains('\0') {
        return Err(Error::invalid("WAVE 路径包含 NUL"));
    }
    Ok(text)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_reader_open(
    name: *const u8,
    length: usize,
    output: *mut *mut ReadHandle,
    info: *mut Info,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(output, 1)?;
        unsafe {
            *output = ptr::null_mut();
        }
        valid(info, 1)?;
        unsafe {
            *info = Info::default();
        }
        let reader = Reader::new(BufReader::new(File::open(unsafe { path(name, length)? })?))?;
        let value = reader.info();
        unsafe {
            *info = Info {
                frames: value.frames,
                channels: u32::from(value.format.channels),
                rate: value.format.sample_rate,
                channel_mask: value.format.channel_mask.unwrap_or(0),
                bits: value.format.sample_format.bits(),
                format: if value.format.sample_format.is_integer() {
                    1
                } else {
                    3
                },
            };
            *output = Box::into_raw(Box::new(ReadHandle { reader }));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_reader_destroy(handle: *mut ReadHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_reader_read(
    handle: *mut ReadHandle,
    output: *mut f32,
    samples: usize,
    frames: *mut u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(frames, 1)?;
        unsafe {
            *frames = 0;
        }
        valid(handle, 1)?;
        valid(output, samples)?;
        let output = if samples == 0 {
            &mut []
        } else {
            unsafe { slice::from_raw_parts_mut(output, samples) }
        };
        let count = unsafe { &mut *handle }.reader.read_frames(output)?;
        unsafe {
            *frames = count;
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_reader_seek(
    handle: *mut ReadHandle,
    frame: u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(handle, 1)?;
        unsafe { &mut *handle }.reader.seek_frame(frame)?;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_writer_create(
    name: *const u8,
    length: usize,
    channels: u32,
    rate: u32,
    bits: u16,
    float_output: u8,
    exclusive: u8,
    output: *mut *mut WriteHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(output, 1)?;
        unsafe {
            *output = ptr::null_mut();
        }
        let name = unsafe { path(name, length)? };
        // Float output keeps the established always-RF64 layout: a streaming writer cannot know
        // the final size, and readers then see one container regardless of length.
        let (sample_format, container) = match (float_output != 0, bits) {
            (true, 32) => (SampleFormat::Float32, Some(Container::Rf64)),
            (true, _) => return Err(Error::unsupported("浮点 WAVE 位深必须为 32")),
            (false, 16) => (SampleFormat::Pcm16, None),
            (false, 24) => (SampleFormat::Pcm24, None),
            (false, 32) => (SampleFormat::Pcm32, None),
            (false, _) => return Err(Error::unsupported("整数 WAVE 位深必须为 16/24/32")),
        };
        let format = Format {
            channels: u16::try_from(channels).map_err(|_| Error::invalid("WAVE 声道数过大"))?,
            sample_rate: rate,
            sample_format,
            channel_mask: None,
        };
        format.block_align()?;
        let mut options = OpenOptions::new();
        options.write(true);
        if exclusive != 0 {
            options.create_new(true);
        } else {
            options.create(true).truncate(true);
        }
        let file = options.open(name)?;
        let writer = match Writer::new(
            file,
            format,
            WriterOptions {
                container,
                ..WriterOptions::default()
            },
        ) {
            Ok(writer) => writer,
            Err(error) => {
                let _ = std::fs::remove_file(name);
                return Err(error);
            }
        };
        unsafe {
            *output = Box::into_raw(Box::new(WriteHandle {
                writer: Some(writer),
            }));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_writer_destroy(handle: *mut WriteHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_writer_write(
    handle: *mut WriteHandle,
    input: *const f32,
    samples: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(handle, 1)?;
        valid(input, samples)?;
        let input = if samples == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(input, samples) }
        };
        unsafe { &mut *handle }
            .writer
            .as_mut()
            .ok_or_else(|| Error::invalid("WAVE writer 已关闭"))?
            .write_frames(input)?;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_writer_finish(
    handle: *mut WriteHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(handle, 1)?;
        let writer = unsafe { &mut *handle }
            .writer
            .take()
            .ok_or_else(|| Error::invalid("WAVE writer 已关闭"))?;
        drop(writer.finish()?);
        Ok(())
    })
}

/// Borrowed bytes; a null pointer is accepted only for an empty slice.
unsafe fn borrowed<'a, T>(data: *const T, len: usize) -> Result<&'a [T]> {
    valid(data, len)?;
    Ok(if len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, len) }
    })
}
unsafe fn fourcc(id: *const u8) -> Result<[u8; 4]> {
    let id = unsafe { borrowed(id, 4)? };
    Ok([id[0], id[1], id[2], id[3]])
}
fn disjoint(regions: &[(usize, usize)]) -> Result<()> {
    for (index, &(start, len)) in regions.iter().enumerate() {
        for &(other, other_len) in &regions[index + 1..] {
            if len != 0 && other_len != 0 && start < other + other_len && other < start + len {
                return Err(Error::invalid("WAVE 输出缓冲区互相重叠"));
            }
        }
    }
    Ok(())
}
fn region<T>(pointer: *const T, len: usize) -> (usize, usize) {
    (pointer as usize, len * size_of::<T>())
}

#[repr(C)]
pub struct WavBext {
    description: *const u8,
    description_len: usize,
    originator: *const u8,
    originator_len: usize,
    originator_reference: *const u8,
    originator_reference_len: usize,
    /// ISO 8601 UTC text.
    date_utc: *const u8,
    date_utc_len: usize,
    loudness: f64,
    true_peak: f64,
    has_loudness: u8,
    has_true_peak: u8,
    /// Also write the third-order AmbiX `ambi` marker.
    hoa3_ambi: u8,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_append_bext(
    name: *const u8,
    length: usize,
    fields: *const WavBext,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(fields, 1)?;
        let fields = unsafe { &*fields };
        let payload = unsafe {
            bext_payload(&BextFields {
                description: borrowed(fields.description, fields.description_len)?,
                originator: borrowed(fields.originator, fields.originator_len)?,
                originator_reference: borrowed(
                    fields.originator_reference,
                    fields.originator_reference_len,
                )?,
                date_utc: borrowed(fields.date_utc, fields.date_utc_len)?,
                loudness: (fields.has_loudness != 0).then_some(fields.loudness),
                true_peak: (fields.has_true_peak != 0).then_some(fields.true_peak),
            })
        };
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(unsafe { path(name, length)? })?;
        append_bext(
            &mut file,
            &payload,
            (fields.hoa3_ambi != 0).then_some(&AMBI_HOA3),
        )
    })
}

#[repr(C)]
pub struct WavChnaTrack {
    uid: *const u8,
    uid_len: usize,
    track_format: *const u8,
    track_format_len: usize,
    pack_format: *const u8,
    pack_format_len: usize,
    /// One-based audio track.
    track_index: u16,
}
#[repr(C)]
pub struct WavLayoutOptions {
    /// File channel -> source channel; empty is identity.
    permutation: *const u16,
    permutation_len: usize,
    chna: *const WavChnaTrack,
    chna_len: usize,
    axml: *const u8,
    axml_len: usize,
    channel_mask: u32,
    force_extensible: u8,
    prefer_riff: u8,
    include_pcm_fact: u8,
}
pub struct LayoutHandle {
    rewriter: Option<LayoutRewriter<BufReader<File>, BufWriter<File>>>,
}

/// Opens `source`, creates `target` exclusively and writes the new header. The caller owns the
/// target path (including removal on failure) and installs it only after a successful finish.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_layout_begin(
    source: *const u8,
    source_length: usize,
    target: *const u8,
    target_length: usize,
    options: *const WavLayoutOptions,
    output: *mut *mut LayoutHandle,
    frames: *mut u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(output, 1)?;
        valid(frames, 1)?;
        disjoint(&[region(output, 1), region(frames, 1)])?;
        unsafe {
            *output = ptr::null_mut();
            *frames = 0;
        }
        valid(options, 1)?;
        let options = unsafe { &*options };
        let tracks = unsafe { borrowed(options.chna, options.chna_len)? }
            .iter()
            .map(|track| unsafe {
                Ok(ChnaTrack {
                    track_index: track.track_index,
                    uid: borrowed(track.uid, track.uid_len)?,
                    track_format: borrowed(track.track_format, track.track_format_len)?,
                    pack_format: borrowed(track.pack_format, track.pack_format_len)?,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let layout = LayoutOptions {
            channel_mask: options.channel_mask,
            force_extensible: options.force_extensible != 0,
            include_pcm_fact: options.include_pcm_fact != 0,
            prefer_riff: options.prefer_riff != 0,
            permutation: unsafe { borrowed(options.permutation, options.permutation_len)? },
            axml: unsafe { borrowed(options.axml, options.axml_len)? },
            chna: &tracks,
        };
        let input = BufReader::new(File::open(unsafe { path(source, source_length)? })?);
        let target = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(unsafe { path(target, target_length)? })?;
        let rewriter = LayoutRewriter::new(input, BufWriter::new(target), &layout)?;
        unsafe {
            *frames = rewriter.frames();
            *output = Box::into_raw(Box::new(LayoutHandle {
                rewriter: Some(rewriter),
            }));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_layout_step(
    handle: *mut LayoutHandle,
    max_frames: u64,
    copied: *mut u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(copied, 1)?;
        unsafe {
            *copied = 0;
        }
        valid(handle, 1)?;
        let count = unsafe { &mut *handle }
            .rewriter
            .as_mut()
            .ok_or_else(|| Error::invalid("WAVE 布局重写已结束"))?
            .step(max_frames)?;
        unsafe {
            *copied = count;
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_layout_finish(
    handle: *mut LayoutHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(handle, 1)?;
        let rewriter = unsafe { &mut *handle }
            .rewriter
            .take()
            .ok_or_else(|| Error::invalid("WAVE 布局重写已结束"))?;
        let mut sink = rewriter.finish()?;
        sink.flush()?;
        sink.into_inner()
            .map_err(|error| Error::io(error.error().to_string()))?;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_layout_destroy(handle: *mut LayoutHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

/// Creates `target` exclusively as a copy of `source` with the axml payload replaced.
/// Existing targets (including aliases of the source) are left untouched. The caller installs
/// the completed file; a target created by this call is removed if the rewrite fails.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_replace_axml(
    source: *const u8,
    source_length: usize,
    target: *const u8,
    target_length: usize,
    axml: *const u8,
    axml_length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let axml = unsafe { borrowed(axml, axml_length)? };
        let source = unsafe { path(source, source_length)? };
        let target = unsafe { path(target, target_length)? };
        let input = BufReader::new(File::open(source)?);
        let output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(target)?;
        // create_new established ownership. The sink below closes before this guard runs,
        // including on Windows and when the boundary catches a panic.
        let mut guard = CreatedPathGuard {
            path: target,
            keep: false,
        };
        let result = (|| {
            let mut sink = BufWriter::new(output);
            replace_chunk(input, &mut sink, *b"axml", axml)?;
            sink.into_inner()
                .map_err(|error| Error::io(error.error().to_string()))?;
            Ok(())
        })();
        guard.keep = result.is_ok();
        result
    })
}

/// Opens only the container/chunk table; sample encoding restrictions belong to the audio reader.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_chunks_open(
    name: *const u8,
    length: usize,
    output: *mut *mut ChunkReadHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(output, 1)?;
        unsafe {
            *output = ptr::null_mut();
        }
        let reader = ChunkReader::new(BufReader::new(File::open(unsafe { path(name, length)? })?))?;
        unsafe {
            *output = Box::into_raw(Box::new(ChunkReadHandle { reader }));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_chunks_destroy(handle: *mut ChunkReadHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

/// Reports the first chunk with `id` (four bytes). With `capacity == 0` only `found`/`size` are
/// set; otherwise `capacity` must equal the payload size and the payload is copied.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_chunks_read(
    handle: *mut ChunkReadHandle,
    id: *const u8,
    output: *mut u8,
    output_capacity: usize,
    size: *mut u64,
    found: *mut u8,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(size, 1)?;
        valid(found, 1)?;
        valid(output, output_capacity)?;
        disjoint(&[
            region(output, output_capacity),
            region(size, 1),
            region(found, 1),
        ])?;
        unsafe {
            *size = 0;
            *found = 0;
        }
        valid(handle, 1)?;
        let id = unsafe { fourcc(id)? };
        let reader = &mut unsafe { &mut *handle }.reader;
        let Some(chunk) = reader.chunks().iter().find(|c| c.id == id).copied() else {
            return Ok(());
        };
        if output_capacity != 0 {
            reader.read_chunk(id, unsafe {
                slice::from_raw_parts_mut(output, output_capacity)
            })?;
        }
        unsafe {
            *size = chunk.size;
            *found = 1;
        }
        Ok(())
    })
}

/// Import-side CHNA: track indices as stored and 12-byte UIDs (`uid` holds `12 * entry_capacity`
/// bytes). With `entry_capacity == 0` only `found`/`count` are set; otherwise it must be at
/// least `count`. Reserved records after numUIDs are ignored.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_chunks_chna(
    handle: *mut ChunkReadHandle,
    track_index: *mut u16,
    uid: *mut u8,
    entry_capacity: usize,
    count: *mut usize,
    found: *mut u8,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(count, 1)?;
        valid(found, 1)?;
        valid(track_index, entry_capacity)?;
        let uid_bytes = entry_capacity
            .checked_mul(12)
            .ok_or_else(|| Error::invalid("CHNA 缓冲区过大"))?;
        valid(uid, uid_bytes)?;
        disjoint(&[
            region(track_index, entry_capacity),
            region(uid, uid_bytes),
            region(count, 1),
            region(found, 1),
        ])?;
        unsafe {
            *count = 0;
            *found = 0;
        }
        valid(handle, 1)?;
        let Some(data) = unsafe { &mut *handle }
            .reader
            .metadata(*b"chna", u64::MAX)?
        else {
            return Ok(());
        };
        let chna = Chna::decode_import(&data)?;
        if entry_capacity != 0 {
            if entry_capacity < chna.entries.len() {
                return Err(Error::invalid("CHNA 缓冲区容量不足"));
            }
            let indices = unsafe { slice::from_raw_parts_mut(track_index, entry_capacity) };
            let uids = unsafe { slice::from_raw_parts_mut(uid, uid_bytes) };
            for ((entry, index), uid) in chna
                .entries
                .iter()
                .zip(indices.iter_mut())
                .zip(uids.as_chunks_mut::<12>().0.iter_mut())
            {
                *index = entry.track_index;
                uid.copy_from_slice(&entry.uid);
            }
        }
        unsafe {
            *count = chna.entries.len();
            *found = 1;
        }
        Ok(())
    })
}

/// Tolerant routing probe: sets `present` when a top-level chunk `id` exists. Fails only when the
/// file cannot be opened or is not RIFF/RF64/BW64 WAVE.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_wav_has_chunk(
    name: *const u8,
    length: usize,
    id: *const u8,
    present: *mut u8,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(present, 1)?;
        unsafe {
            *present = 0;
        }
        let id = unsafe { fourcc(id)? };
        let found = has_chunk(
            BufReader::new(File::open(unsafe { path(name, length)? })?),
            id,
        )?;
        unsafe {
            *present = u8::from(found);
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn metadata_wave(tag: u16, bits: u16) -> Vec<u8> {
        let align = bits / 8;
        let mut format = Vec::new();
        format.extend_from_slice(&tag.to_le_bytes());
        format.extend_from_slice(&1u16.to_le_bytes());
        format.extend_from_slice(&48000u32.to_le_bytes());
        format.extend_from_slice(&(48000 * u32::from(align)).to_le_bytes());
        format.extend_from_slice(&align.to_le_bytes());
        format.extend_from_slice(&bits.to_le_bytes());
        let mut out = b"RIFF\0\0\0\0WAVE".to_vec();
        for (id, data) in [
            (*b"fmt ", format),
            (*b"data", vec![0x5a; usize::from(align) * 3]),
            (*b"axml", b"<old/>".to_vec()),
        ] {
            out.extend_from_slice(&id);
            out.extend_from_slice(&(data.len() as u32).to_le_bytes());
            out.extend_from_slice(&data);
            if data.len() & 1 != 0 {
                out.push(0);
            }
        }
        let size = out.len() as u32 - 8;
        out[4..8].copy_from_slice(&size.to_le_bytes());
        out
    }

    #[test]
    fn metadata_reader_accepts_undecodable_audio_and_validates_outputs() {
        let path = std::env::temp_dir().join(format!("mradm-chunks-{}.wav", std::process::id()));
        let name = path.to_str().unwrap();
        let mut message = [0; 128];
        unsafe {
            let mut reader = ptr::dangling_mut();
            assert_eq!(
                mradm_wav_chunks_open(ptr::null(), 0, &mut reader, message.as_mut_ptr(), 128),
                1
            );
            assert!(reader.is_null());
            mradm_wav_chunks_destroy(reader);
            for (tag, bits) in [(1, 8), (3, 64)] {
                std::fs::write(&path, metadata_wave(tag, bits)).unwrap();
                assert_eq!(
                    mradm_wav_chunks_open(
                        name.as_ptr(),
                        name.len(),
                        &mut reader,
                        message.as_mut_ptr(),
                        128
                    ),
                    0
                );
                let (mut size, mut found) = (0, 0);
                let mut xml = [0; 6];
                assert_eq!(
                    mradm_wav_chunks_read(
                        reader,
                        b"axml".as_ptr(),
                        xml.as_mut_ptr(),
                        xml.len(),
                        &mut size,
                        &mut found,
                        message.as_mut_ptr(),
                        128
                    ),
                    0
                );
                assert_eq!((size, found, xml), (6, 1, *b"<old/>"));
                mradm_wav_chunks_destroy(reader);
            }
        }
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn axml_replacement_preserves_existing_files_and_removes_failed_outputs() {
        let root = std::env::temp_dir().join(format!("mradm-replace-{}", std::process::id()));
        std::fs::create_dir(&root).unwrap();
        let source = root.join("source.wav");
        let target = root.join("target.wav");
        let hard_link = root.join("hard.wav");
        let original = metadata_wave(1, 16);
        std::fs::write(&source, &original).unwrap();
        std::fs::write(&target, b"existing output").unwrap();
        std::fs::hard_link(&source, &hard_link).unwrap();
        let replace = |target: &std::path::Path| {
            let source = source.to_str().unwrap();
            let target = target.to_str().unwrap();
            let mut message = [0; 128];
            unsafe {
                mradm_wav_replace_axml(
                    source.as_ptr(),
                    source.len(),
                    target.as_ptr(),
                    target.len(),
                    b"<new/>".as_ptr(),
                    6,
                    message.as_mut_ptr(),
                    128,
                )
            }
        };
        for path in [&source, &target, &hard_link] {
            let before = std::fs::read(path).unwrap();
            assert_eq!(replace(path), 3);
            assert_eq!(std::fs::read(path).unwrap(), before);
            assert_eq!(std::fs::read(&source).unwrap(), original);
        }
        #[cfg(unix)]
        {
            let link = root.join("symbolic.wav");
            std::os::unix::fs::symlink(&source, &link).unwrap();
            assert_eq!(replace(&link), 3);
            assert!(link.is_symlink());
            assert_eq!(std::fs::read(&source).unwrap(), original);
        }
        let fresh = root.join("fresh.wav");
        assert_eq!(replace(&fresh), 0);
        let mut reader = ChunkReader::new(BufReader::new(File::open(&fresh).unwrap())).unwrap();
        assert_eq!(reader.metadata(*b"axml", 6).unwrap().unwrap(), b"<new/>");
        drop(reader);
        std::fs::remove_file(&fresh).unwrap();

        std::fs::write(&source, &original[..original.len() - 1]).unwrap();
        assert_eq!(replace(&fresh), 3);
        assert!(!fresh.exists());
        assert_eq!(
            std::fs::read(&source).unwrap(),
            original[..original.len() - 1]
        );
        assert_eq!(replace(&target), 3);
        assert_eq!(std::fs::read(&target).unwrap(), b"existing output");
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn failed_open_clears_outputs_and_invalid_buffers_return_errors() {
        unsafe {
            let mut handle = ptr::dangling_mut();
            let mut info = Info::default();
            let mut message = [0; 64];
            assert_eq!(
                mradm_wav_reader_open(
                    ptr::null(),
                    0,
                    &mut handle,
                    &mut info,
                    message.as_mut_ptr(),
                    64
                ),
                1
            );
            assert!(handle.is_null());
            let mut frames = 99;
            assert_eq!(
                mradm_wav_reader_read(
                    ptr::null_mut(),
                    ptr::null_mut(),
                    0,
                    &mut frames,
                    message.as_mut_ptr(),
                    64
                ),
                1
            );
            assert_eq!(frames, 0);
            assert_eq!(
                mradm_wav_writer_finish(ptr::null_mut(), message.as_mut_ptr(), 64),
                1
            );
            mradm_wav_reader_destroy(ptr::null_mut());
            mradm_wav_writer_destroy(ptr::null_mut());
        }
    }
    #[test]
    fn utf8_path_exclusive_create_finish_and_sample_buffers() {
        let name = std::env::temp_dir().join(format!("mradm-波形-é-{}.wav", std::process::id()));
        let name_text = name.to_str().unwrap();
        let mut message = [0; 128];
        unsafe {
            let mut writer = ptr::null_mut();
            assert_eq!(
                mradm_wav_writer_create(
                    name_text.as_ptr(),
                    name_text.len(),
                    1,
                    96000,
                    24,
                    0,
                    1,
                    &mut writer,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            let mut collision = ptr::dangling_mut();
            assert_eq!(
                mradm_wav_writer_create(
                    name_text.as_ptr(),
                    name_text.len(),
                    1,
                    96000,
                    24,
                    0,
                    1,
                    &mut collision,
                    message.as_mut_ptr(),
                    128
                ),
                3
            );
            assert!(collision.is_null());
            assert_eq!(
                mradm_wav_writer_write(
                    writer,
                    [0.0, 1.0, -1.0].as_ptr(),
                    3,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(
                mradm_wav_writer_finish(writer, message.as_mut_ptr(), 128),
                0
            );
            assert_eq!(
                mradm_wav_writer_finish(writer, message.as_mut_ptr(), 128),
                1
            );
            mradm_wav_writer_destroy(writer);
            let mut reader = ptr::null_mut();
            let mut info = Info::default();
            assert_eq!(
                mradm_wav_reader_open(
                    name_text.as_ptr(),
                    name_text.len(),
                    &mut reader,
                    &mut info,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(info.frames, 3);
            assert_eq!(info.rate, 96000);
            assert_eq!((info.bits, info.format, info.channel_mask), (24, 1, 0));
            let mut samples = [0.0; 3];
            let mut count = 0;
            assert_eq!(
                mradm_wav_reader_read(
                    reader,
                    samples.as_mut_ptr(),
                    3,
                    &mut count,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(count, 3);
            assert_eq!(
                samples,
                [0.0, 8388607.0 / 8388608.0, -8388607.0 / 8388608.0]
            );
            assert_eq!(
                mradm_wav_reader_read(
                    reader,
                    ptr::null_mut(),
                    usize::MAX,
                    &mut count,
                    message.as_mut_ptr(),
                    128
                ),
                1
            );
            assert_eq!(count, 0);
            assert_eq!(
                mradm_wav_reader_seek(reader, u64::MAX, message.as_mut_ptr(), 128),
                0
            );
            assert_eq!(
                mradm_wav_reader_read(
                    reader,
                    samples.as_mut_ptr(),
                    3,
                    &mut count,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(count, 0);
            mradm_wav_reader_destroy(reader);
        }
        std::fs::remove_file(name).unwrap();
    }

    #[test]
    fn float_writer_truncates_existing_output_and_writes_rf64() {
        let name = std::env::temp_dir().join(format!("mradm-float-{}.wav", std::process::id()));
        std::fs::write(&name, b"stale bytes that must be replaced").unwrap();
        let name_text = name.to_str().unwrap();
        let mut message = [0; 128];
        unsafe {
            let mut writer = ptr::null_mut();
            assert_eq!(
                mradm_wav_writer_create(
                    name_text.as_ptr(),
                    name_text.len(),
                    2,
                    48000,
                    32,
                    1,
                    0,
                    &mut writer,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            let samples = [0.25, -1.5, f32::MIN_POSITIVE, 2.0];
            assert_eq!(
                mradm_wav_writer_write(writer, samples.as_ptr(), 4, message.as_mut_ptr(), 128),
                0
            );
            assert_eq!(
                mradm_wav_writer_finish(writer, message.as_mut_ptr(), 128),
                0
            );
            mradm_wav_writer_destroy(writer);
            assert_eq!(&std::fs::read(&name).unwrap()[..4], b"RF64");
            let mut reader = ptr::null_mut();
            let mut info = Info::default();
            assert_eq!(
                mradm_wav_reader_open(
                    name_text.as_ptr(),
                    name_text.len(),
                    &mut reader,
                    &mut info,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(
                (
                    info.frames,
                    info.channels,
                    info.bits,
                    info.format,
                    info.channel_mask
                ),
                (2, 2, 32, 3, 0)
            );
            let mut read = [0.0; 4];
            let mut count = 0;
            assert_eq!(
                mradm_wav_reader_read(
                    reader,
                    read.as_mut_ptr(),
                    4,
                    &mut count,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!((count, read), (2, samples));
            mradm_wav_reader_destroy(reader);
            let mut invalid = ptr::dangling_mut();
            assert_eq!(
                mradm_wav_writer_create(
                    name_text.as_ptr(),
                    name_text.len(),
                    1,
                    48000,
                    24,
                    1,
                    0,
                    &mut invalid,
                    message.as_mut_ptr(),
                    128
                ),
                2
            );
            assert!(invalid.is_null());
        }
        std::fs::remove_file(name).ok();
    }

    fn empty_options() -> WavLayoutOptions {
        WavLayoutOptions {
            permutation: ptr::null(),
            permutation_len: 0,
            chna: ptr::null(),
            chna_len: 0,
            axml: ptr::null(),
            axml_len: 0,
            channel_mask: 0,
            force_extensible: 0,
            prefer_riff: 0,
            include_pcm_fact: 0,
        }
    }

    #[test]
    fn container_edits_validate_arguments_and_round_trip() {
        let dir = std::env::temp_dir();
        let source = dir.join(format!("mradm-edit-{}.wav", std::process::id()));
        let target = dir.join(format!("mradm-edit-{}.layout.wav", std::process::id()));
        let copy = dir.join(format!("mradm-edit-{}.copy.wav", std::process::id()));
        for path in [&source, &target, &copy] {
            std::fs::remove_file(path).ok();
        }
        let source_text = source.to_str().unwrap().as_bytes();
        let target_text = target.to_str().unwrap().as_bytes();
        let copy_text = copy.to_str().unwrap().as_bytes();
        let mut message = [0u8; 128];
        unsafe {
            let mut writer = ptr::null_mut();
            assert_eq!(
                mradm_wav_writer_create(
                    source_text.as_ptr(),
                    source_text.len(),
                    2,
                    48000,
                    16,
                    0,
                    1,
                    &mut writer,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            let samples = [0.5f32, -0.5, 0.25, -0.25, 0.125, -0.125];
            assert_eq!(
                mradm_wav_writer_write(writer, samples.as_ptr(), 6, message.as_mut_ptr(), 128),
                0
            );
            assert_eq!(
                mradm_wav_writer_finish(writer, message.as_mut_ptr(), 128),
                0
            );
            mradm_wav_writer_destroy(writer);

            // Layout: null options are rejected and outputs cleared; a swap with ADM chunks succeeds.
            let mut layout = ptr::dangling_mut();
            let mut frames = 99;
            assert_eq!(
                mradm_wav_layout_begin(
                    source_text.as_ptr(),
                    source_text.len(),
                    target_text.as_ptr(),
                    target_text.len(),
                    ptr::null(),
                    &mut layout,
                    &mut frames,
                    message.as_mut_ptr(),
                    128
                ),
                1
            );
            assert!(layout.is_null() && frames == 0);
            let permutation = [1u16, 0];
            let tracks = [1u16, 2].map(|index| WavChnaTrack {
                uid: b"ATU_00000001".as_ptr(),
                uid_len: 12,
                track_format: b"AT_00010001_01".as_ptr(),
                track_format_len: 14,
                pack_format: b"AP_00010002".as_ptr(),
                pack_format_len: 11,
                track_index: index,
            });
            let options = WavLayoutOptions {
                permutation: permutation.as_ptr(),
                permutation_len: 2,
                chna: tracks.as_ptr(),
                chna_len: 2,
                axml: b"<a/>".as_ptr(),
                axml_len: 4,
                channel_mask: 0x3,
                ..empty_options()
            };
            assert_eq!(
                mradm_wav_layout_begin(
                    source_text.as_ptr(),
                    source_text.len(),
                    target_text.as_ptr(),
                    target_text.len(),
                    &options,
                    &mut layout,
                    &mut frames,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(frames, 3);
            let mut copied = 0;
            let mut total = 0;
            loop {
                assert_eq!(
                    mradm_wav_layout_step(layout, 2, &mut copied, message.as_mut_ptr(), 128),
                    0
                );
                if copied == 0 {
                    break;
                }
                total += copied;
            }
            assert_eq!(total, 3);
            assert_eq!(
                mradm_wav_layout_finish(layout, message.as_mut_ptr(), 128),
                0
            );
            assert_eq!(
                mradm_wav_layout_finish(layout, message.as_mut_ptr(), 128),
                1
            );
            mradm_wav_layout_destroy(layout);
            // The target is created exclusively.
            assert_eq!(
                mradm_wav_layout_begin(
                    source_text.as_ptr(),
                    source_text.len(),
                    target_text.as_ptr(),
                    target_text.len(),
                    &empty_options(),
                    &mut layout,
                    &mut frames,
                    message.as_mut_ptr(),
                    128
                ),
                3
            );
            assert!(layout.is_null());

            // bext append keeps the file readable; then the reader exposes axml and CHNA.
            let fields = WavBext {
                description: ptr::null(),
                description_len: 0,
                originator: b"enc".as_ptr(),
                originator_len: 3,
                originator_reference: ptr::null(),
                originator_reference_len: 0,
                date_utc: ptr::null(),
                date_utc_len: 0,
                loudness: -23.0,
                true_peak: 0.0,
                has_loudness: 1,
                has_true_peak: 0,
                hoa3_ambi: 0,
            };
            assert_eq!(
                mradm_wav_append_bext(
                    target_text.as_ptr(),
                    target_text.len(),
                    &fields,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            let mut reader = ptr::null_mut();
            let mut info = Info::default();
            assert_eq!(
                mradm_wav_reader_open(
                    target_text.as_ptr(),
                    target_text.len(),
                    &mut reader,
                    &mut info,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!((info.frames, info.channel_mask), (3, 0x3));
            mradm_wav_reader_destroy(reader);
            let mut reader = ptr::null_mut();
            assert_eq!(
                mradm_wav_chunks_open(
                    target_text.as_ptr(),
                    target_text.len(),
                    &mut reader,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            let (mut size, mut found) = (0u64, 0u8);
            assert_eq!(
                mradm_wav_chunks_read(
                    reader,
                    b"axml".as_ptr(),
                    ptr::null_mut(),
                    0,
                    &mut size,
                    &mut found,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!((size, found), (4, 1));
            let mut axml = [0u8; 4];
            assert_eq!(
                mradm_wav_chunks_read(
                    reader,
                    b"axml".as_ptr(),
                    axml.as_mut_ptr(),
                    4,
                    &mut size,
                    &mut found,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(&axml, b"<a/>");
            // Wrong capacity, and an output buffer overlapping the size slot, are rejected.
            assert_eq!(
                mradm_wav_chunks_read(
                    reader,
                    b"axml".as_ptr(),
                    axml.as_mut_ptr(),
                    3,
                    &mut size,
                    &mut found,
                    message.as_mut_ptr(),
                    128
                ),
                1
            );
            let mut slots = [0u64; 2];
            let base = slots.as_mut_ptr();
            assert_eq!(
                mradm_wav_chunks_read(
                    reader,
                    b"axml".as_ptr(),
                    base.cast(),
                    4,
                    base,
                    &mut found,
                    message.as_mut_ptr(),
                    128
                ),
                1
            );
            let mut count = 0usize;
            assert_eq!(
                mradm_wav_chunks_chna(
                    reader,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    0,
                    &mut count,
                    &mut found,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!((count, found), (2, 1));
            let mut indices = [0u16; 2];
            let mut uids = [0u8; 24];
            assert_eq!(
                mradm_wav_chunks_chna(
                    reader,
                    indices.as_mut_ptr(),
                    uids.as_mut_ptr(),
                    1,
                    &mut count,
                    &mut found,
                    message.as_mut_ptr(),
                    128
                ),
                1
            );
            assert_eq!(
                mradm_wav_chunks_chna(
                    reader,
                    indices.as_mut_ptr(),
                    uids.as_mut_ptr(),
                    2,
                    &mut count,
                    &mut found,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(indices, [1, 2]);
            assert_eq!(&uids[12..], b"ATU_00000001");
            assert_eq!(
                mradm_wav_chunks_read(
                    reader,
                    b"none".as_ptr(),
                    ptr::null_mut(),
                    0,
                    &mut size,
                    &mut found,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!((size, found), (0, 0));
            mradm_wav_chunks_destroy(reader);

            // axml replacement, then the tolerant probe.
            assert_eq!(
                mradm_wav_replace_axml(
                    target_text.as_ptr(),
                    target_text.len(),
                    copy_text.as_ptr(),
                    copy_text.len(),
                    b"<b/>!".as_ptr(),
                    5,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            let mut present = 9;
            assert_eq!(
                mradm_wav_has_chunk(
                    copy_text.as_ptr(),
                    copy_text.len(),
                    b"bext".as_ptr(),
                    &mut present,
                    message.as_mut_ptr(),
                    128
                ),
                0
            );
            assert_eq!(present, 1);
            assert_eq!(
                mradm_wav_has_chunk(
                    copy_text.as_ptr(),
                    copy_text.len(),
                    ptr::null(),
                    &mut present,
                    message.as_mut_ptr(),
                    128
                ),
                1
            );
            assert_eq!(present, 0);
            assert_eq!(
                mradm_wav_append_bext(
                    copy_text.as_ptr(),
                    copy_text.len(),
                    ptr::null(),
                    message.as_mut_ptr(),
                    128
                ),
                1
            );
        }
        for path in [&source, &target, &copy] {
            std::fs::remove_file(path).ok();
        }
    }
}
