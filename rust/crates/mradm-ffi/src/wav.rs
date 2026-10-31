//! Private file I/O boundary. Owners and their allocations never cross allocators.
use mradm_render::postprocess::{self, Begin, Failure, Pass, Profile, Progress};
use mradm_wav::{
    AMBI_HOA3, BextFields, Chna, ChunkReader, Error, Reader, Result, Writer, append_bext,
    bext_payload, has_chunk,
};
use std::{
    fs::{File, OpenOptions},
    io::BufReader,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice, str,
};

pub struct ReadHandle {
    reader: Reader<BufReader<File>>,
}
pub struct ChunkReadHandle {
    reader: ChunkReader<BufReader<File>>,
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
pub(crate) unsafe fn path<'a>(data: *const u8, len: usize) -> Result<&'a str> {
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
        let writer = mradm_wav::create_file_writer(
            name.as_ref(),
            channels,
            rate,
            bits,
            float_output != 0,
            exclusive != 0,
        )?;
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

// ── Post-processing passes (PCM consistency 4.8) ──────────────────────────────

pub struct PostHandle {
    pass: Option<Pass>,
}
/// One progress report. `emit` asks for a progress event; once `complete`, the data is written
/// and finish installs the output. `install_check` (begin only) asks for one more cancellation
/// check before finish.
#[repr(C)]
pub struct OutputPostProgress {
    fraction: f64,
    done: u64,
    total: u64,
    /// Static text.
    message: *const u8,
    message_len: usize,
    emit: u8,
    complete: u8,
    install_check: u8,
}
fn report(progress: Progress, complete: bool, install_check: bool) -> OutputPostProgress {
    OutputPostProgress {
        fraction: progress.fraction,
        done: progress.done,
        total: progress.total,
        message: progress.message.as_ptr(),
        message_len: progress.message.len(),
        emit: u8::from(progress.emit),
        complete: u8::from(complete),
        install_check: u8::from(install_check),
    }
}
/// Copies `text` into a C buffer, truncated at a character boundary and NUL-terminated.
fn copy_text(text: &str, output: *mut u8, capacity: usize) {
    if !output.is_null() && capacity != 0 {
        let mut len = text.len().min(capacity - 1);
        while !text.is_char_boundary(len) {
            len -= 1;
        }
        unsafe {
            ptr::copy_nonoverlapping(text.as_ptr(), output, len);
            *output.add(len) = 0;
        }
    }
}
/// The FFI boundary of a pass: errors carry their own context.
fn post(
    message: *mut u8,
    capacity: usize,
    context: *mut u8,
    context_capacity: usize,
    f: impl FnOnce() -> std::result::Result<(), Failure>,
) -> i32 {
    let failure = match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => None,
        Ok(Err(failure)) => Some(failure),
        Err(_) => Some(Failure {
            code: 6,
            message: "Rust 后处理 panic".into(),
            context: String::new(),
        }),
    };
    copy_text(
        failure.as_ref().map_or("", |f| &f.message),
        message,
        capacity,
    );
    copy_text(
        failure.as_ref().map_or("", |f| &f.context),
        context,
        context_capacity,
    );
    failure.map_or(0, |f| f.code)
}
fn invalid(error: Error) -> Failure {
    Failure {
        code: error.code,
        message: error.message,
        context: String::new(),
    }
}
/// Publishes a begin result: a handle for a started pass, or null with the final event.
unsafe fn started(
    begin: Begin,
    output: *mut *mut PostHandle,
    progress: *mut OutputPostProgress,
) -> std::result::Result<(), Failure> {
    let (handle, first) = match begin {
        Begin::Skipped(event) => (ptr::null_mut(), report(event, true, false)),
        Begin::Started(pass, event) => {
            let check = pass.cancel_before_install();
            (
                Box::into_raw(Box::new(PostHandle { pass: Some(*pass) })),
                report(event, false, check),
            )
        }
    };
    unsafe {
        *output = handle;
        *progress = first;
    }
    Ok(())
}
unsafe fn post_outputs(
    output: *mut *mut PostHandle,
    progress: *mut OutputPostProgress,
) -> std::result::Result<(), Failure> {
    valid(output, 1).map_err(invalid)?;
    valid(progress, 1).map_err(invalid)?;
    unsafe {
        *output = ptr::null_mut();
    }
    Ok(())
}
unsafe fn post_path<'a>(data: *const u8, len: usize) -> std::result::Result<&'a str, Failure> {
    unsafe { path(data, len) }.map_err(invalid)
}
unsafe fn post_bytes<'a>(data: *const u8, len: usize) -> std::result::Result<&'a [u8], Failure> {
    unsafe { borrowed(data, len) }.map_err(invalid)
}

/// `apply_gain_to_file`; `cancelled` is checked after the unity-gain test.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_gain_begin(
    name: *const u8,
    length: usize,
    gain: f32,
    layout: *const u8,
    layout_len: usize,
    cancelled: u8,
    output: *mut *mut PostHandle,
    progress: *mut OutputPostProgress,
    message: *mut u8,
    capacity: usize,
    context: *mut u8,
    context_capacity: usize,
) -> i32 {
    post(message, capacity, context, context_capacity, || unsafe {
        post_outputs(output, progress)?;
        let name = post_path(name, length)?;
        let layout = post_bytes(layout, layout_len)?;
        started(
            postprocess::gain(name, gain, layout, cancelled != 0)?,
            output,
            progress,
        )
    })
}
/// `trim_file_frames`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_trim_begin(
    name: *const u8,
    length: usize,
    start_frame: u64,
    frame_count: u64,
    layout: *const u8,
    layout_len: usize,
    output: *mut *mut PostHandle,
    progress: *mut OutputPostProgress,
    message: *mut u8,
    capacity: usize,
    context: *mut u8,
    context_capacity: usize,
) -> i32 {
    post(message, capacity, context, context_capacity, || unsafe {
        post_outputs(output, progress)?;
        let name = post_path(name, length)?;
        let layout = post_bytes(layout, layout_len)?;
        started(
            postprocess::trim(name, start_frame, frame_count, layout)?,
            output,
            progress,
        )
    })
}
/// `downconvert_to_int`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_bitdepth_begin(
    name: *const u8,
    length: usize,
    bits: u16,
    output: *mut *mut PostHandle,
    progress: *mut OutputPostProgress,
    message: *mut u8,
    capacity: usize,
    context: *mut u8,
    context_capacity: usize,
) -> i32 {
    post(message, capacity, context, context_capacity, || unsafe {
        post_outputs(output, progress)?;
        let name = post_path(name, length)?;
        started(postprocess::bitdepth(name, bits)?, output, progress)
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
unsafe fn explicit_layout(options: &WavLayoutOptions) -> Result<postprocess::Layout> {
    let chna = unsafe { borrowed(options.chna, options.chna_len)? }
        .iter()
        .map(|track| unsafe {
            Ok(postprocess::ChnaEntry {
                track_index: track.track_index,
                uid: borrowed(track.uid, track.uid_len)?.to_vec(),
                track_format: borrowed(track.track_format, track.track_format_len)?.to_vec(),
                pack_format: borrowed(track.pack_format, track.pack_format_len)?.to_vec(),
            })
        })
        .collect::<Result<Vec<_>>>()?;
    Ok(postprocess::Layout {
        channel_mask: options.channel_mask,
        permutation: unsafe { borrowed(options.permutation, options.permutation_len)? }.to_vec(),
        axml: unsafe { borrowed(options.axml, options.axml_len)? }.to_vec(),
        chna,
        force_extensible: options.force_extensible != 0,
        prefer_riff: options.prefer_riff != 0,
        include_pcm_fact: options.include_pcm_fact != 0,
    })
}
/// WAV layout finalization, in place. With `options` the layout is explicit (the caller has
/// checked cancellation); otherwise it follows `layout` and `profile` (0 ADM semantics, 1 speaker
/// re-render) and `cancelled` is checked after building it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_finalize_begin(
    name: *const u8,
    length: usize,
    layout: *const u8,
    layout_len: usize,
    profile: u32,
    options: *const WavLayoutOptions,
    cancelled: u8,
    output: *mut *mut PostHandle,
    progress: *mut OutputPostProgress,
    message: *mut u8,
    capacity: usize,
    context: *mut u8,
    context_capacity: usize,
) -> i32 {
    post(message, capacity, context, context_capacity, || unsafe {
        post_outputs(output, progress)?;
        let name = post_path(name, length)?;
        let begin = if options.is_null() {
            let layout = String::from_utf8_lossy(post_bytes(layout, layout_len)?);
            let profile = if profile == 1 {
                Profile::SpeakerRerender
            } else {
                Profile::AdmSemantics
            };
            postprocess::finalize_rendered(name, &layout, profile, cancelled != 0)?
        } else {
            valid(options, 1).map_err(invalid)?;
            postprocess::finalize(name, &explicit_layout(&*options).map_err(invalid)?)?
        };
        started(begin, output, progress)
    })
}
/// One block of a started pass.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_post_step(
    handle: *mut PostHandle,
    progress: *mut OutputPostProgress,
    message: *mut u8,
    capacity: usize,
    context: *mut u8,
    context_capacity: usize,
) -> i32 {
    post(message, capacity, context, context_capacity, || unsafe {
        valid(handle, 1).map_err(invalid)?;
        valid(progress, 1).map_err(invalid)?;
        let pass = (*handle)
            .pass
            .as_mut()
            .ok_or_else(|| invalid(Error::invalid("后处理已结束")))?;
        let step = pass.step()?;
        *progress = report(step.progress, step.complete, false);
        Ok(())
    })
}
/// Installs the output of a completed pass; `progress` receives the final event.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_post_finish(
    handle: *mut PostHandle,
    progress: *mut OutputPostProgress,
    message: *mut u8,
    capacity: usize,
    context: *mut u8,
    context_capacity: usize,
) -> i32 {
    post(message, capacity, context, context_capacity, || unsafe {
        valid(handle, 1).map_err(invalid)?;
        valid(progress, 1).map_err(invalid)?;
        let pass = (*handle)
            .pass
            .take()
            .ok_or_else(|| invalid(Error::invalid("后处理已结束")))?;
        *progress = report(pass.finish()?, true, false);
        Ok(())
    })
}
/// Destroying an unfinished pass removes its temporary file.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_post_destroy(handle: *mut PostHandle) {
    if !handle.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(handle)) }));
    }
}

/// `target` becomes a copy of `source` with the axml payload replaced, written to a sidecar and
/// installed (aliases of the source included); on failure the target is left as it was.
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
    context: *mut u8,
    context_capacity: usize,
) -> i32 {
    post(message, capacity, context, context_capacity, || unsafe {
        let axml = post_bytes(axml, axml_length)?;
        let source = post_path(source, source_length)?;
        let target = post_path(target, target_length)?;
        postprocess::replace_axml(source, target, axml)
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

// ── Final float PCM writers and CAF (PCM consistency 4.8) ─────────────────────

pub struct OutputHandle {
    writer: mradm_render::output::Writer,
}
pub struct CafReadHandle {
    reader: mradm_wav::caf::Reader<BufReader<File>>,
}

/// `caf != 0` writes CAF with the `layout` channel layout tag; otherwise RF64 WAVE.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_writer_create(
    name: *const u8,
    length: usize,
    channels: u32,
    rate: u32,
    caf: u8,
    layout: *const u8,
    layout_len: usize,
    output: *mut *mut OutputHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(output, 1)?;
        unsafe {
            *output = ptr::null_mut();
        }
        let name = unsafe { path(name, length)? };
        let layout = unsafe { borrowed(layout, layout_len)? };
        let writer = mradm_render::output::Writer::create(
            name.as_ref(),
            channels,
            rate,
            (caf != 0).then_some(layout),
        )?;
        unsafe {
            *output = Box::into_raw(Box::new(OutputHandle { writer }));
        }
        Ok(())
    })
}
/// Destroying an unfinished writer finishes it on a best-effort basis.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_writer_destroy(handle: *mut OutputHandle) {
    if !handle.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(handle)) }));
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_writer_write(
    handle: *mut OutputHandle,
    input: *const f32,
    samples: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(handle, 1)?;
        let input = unsafe { borrowed(input, samples)? };
        unsafe { &mut *handle }.writer.write(input)?;
        Ok(())
    })
}
/// Repeated calls return the first result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_output_writer_finish(
    handle: *mut OutputHandle,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(handle, 1)?;
        unsafe { &mut *handle }.writer.finish()
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_caf_reader_open(
    name: *const u8,
    length: usize,
    output: *mut *mut CafReadHandle,
    info: *mut Info,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(output, 1)?;
        valid(info, 1)?;
        unsafe {
            *output = ptr::null_mut();
        }
        let name = unsafe { path(name, length)? };
        let reader = mradm_wav::caf::Reader::open(name.as_ref())?;
        unsafe {
            *info = Info {
                frames: reader.frame_count(),
                channels: reader.channels(),
                rate: reader.sample_rate(),
                channel_mask: 0,
                bits: 32,
                format: 3,
            };
            *output = Box::into_raw(Box::new(CafReadHandle { reader }));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_caf_reader_destroy(handle: *mut CafReadHandle) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
/// Reads up to `samples / channels` frames into `output`; `frames` receives the count read.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_caf_reader_read(
    handle: *mut CafReadHandle,
    output: *mut f32,
    samples: usize,
    frames: *mut u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        valid(handle, 1)?;
        valid(frames, 1)?;
        valid(output, samples)?;
        let output = if samples == 0 {
            &mut []
        } else {
            unsafe { slice::from_raw_parts_mut(output, samples) }
        };
        let read = unsafe { &mut *handle }.reader.read(output)?;
        unsafe {
            *frames = read;
        }
        Ok(())
    })
}
/// Appends an `info` chunk with the non-empty encoder, date and comments values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_caf_append_info(
    name: *const u8,
    length: usize,
    encoder: *const u8,
    encoder_len: usize,
    date: *const u8,
    date_len: usize,
    comments: *const u8,
    comments_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let name = unsafe { path(name, length)? };
        let (encoder, date, comments) = unsafe {
            (
                borrowed(encoder, encoder_len)?,
                borrowed(date, date_len)?,
                borrowed(comments, comments_len)?,
            )
        };
        mradm_wav::caf::append_info(name.as_ref(), encoder, date, comments)
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
    fn axml_replacement_installs_or_leaves_the_target_unchanged() {
        let root = std::env::temp_dir().join(format!("mradm-replace-{}", std::process::id()));
        std::fs::create_dir(&root).unwrap();
        let source = root.join("source.wav");
        let target = root.join("target.wav");
        let original = metadata_wave(1, 16);
        std::fs::write(&source, &original).unwrap();
        std::fs::write(&target, b"existing output").unwrap();
        let replace = |source: &std::path::Path, target: &std::path::Path| {
            let source = source.to_str().unwrap();
            let target = target.to_str().unwrap();
            let (mut message, mut context) = ([0; 128], [0; 256]);
            let code = unsafe {
                mradm_wav_replace_axml(
                    source.as_ptr(),
                    source.len(),
                    target.as_ptr(),
                    target.len(),
                    b"<new/>".as_ptr(),
                    6,
                    message.as_mut_ptr(),
                    128,
                    context.as_mut_ptr(),
                    256,
                )
            };
            let end = context.iter().position(|b| *b == 0).unwrap();
            (code, String::from_utf8(context[..end].to_vec()).unwrap())
        };
        let axml = |path: &std::path::Path| {
            let mut reader = ChunkReader::new(BufReader::new(File::open(path).unwrap())).unwrap();
            reader.metadata(*b"axml", 6).unwrap().unwrap()
        };
        let fresh = root.join("fresh.wav");
        assert_eq!(replace(&source, &fresh).0, 0);
        assert_eq!(axml(&fresh), b"<new/>");
        // An existing target, and the source itself, are replaced by an installed copy.
        assert_eq!(replace(&source, &target).0, 0);
        assert_eq!(axml(&target), b"<new/>");
        assert_eq!(replace(&source, &source).0, 0);
        assert_eq!(axml(&source), b"<new/>");
        // A failed rewrite leaves the target and no sidecar.
        std::fs::write(&source, &original[..original.len() - 1]).unwrap();
        let before = std::fs::read(&target).unwrap();
        let (code, context) = replace(&source, &target);
        assert_eq!(code, 3);
        assert!(context.starts_with("path=") && context.contains(" -> "));
        assert_eq!(std::fs::read(&target).unwrap(), before);
        // A directory target is never moved out of the way.
        let directory = root.join("directory.wav");
        std::fs::create_dir(&directory).unwrap();
        std::fs::write(&source, &original).unwrap();
        assert_eq!(replace(&source, &directory).0, 3);
        assert!(directory.is_dir());
        let names: Vec<_> = std::fs::read_dir(&root)
            .unwrap()
            .map(|e| e.unwrap().file_name().into_string().unwrap())
            .collect();
        assert!(names.iter().all(|n| !n.contains("axml_tmp")), "{names:?}");
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

            // Layout: an unknown layout id is rejected with its context; an explicit swap with ADM
            // chunks is finalized in place.
            std::fs::copy(&source, &target).unwrap();
            let mut pass = ptr::dangling_mut();
            let mut progress = std::mem::zeroed::<OutputPostProgress>();
            let mut context = [0u8; 256];
            assert_eq!(
                mradm_output_finalize_begin(
                    target_text.as_ptr(),
                    target_text.len(),
                    b"nope".as_ptr(),
                    4,
                    0,
                    ptr::null(),
                    0,
                    &mut pass,
                    &mut progress,
                    message.as_mut_ptr(),
                    128,
                    context.as_mut_ptr(),
                    256
                ),
                3
            );
            assert!(pass.is_null());
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
                mradm_output_finalize_begin(
                    target_text.as_ptr(),
                    target_text.len(),
                    ptr::null(),
                    0,
                    0,
                    &options,
                    0,
                    &mut pass,
                    &mut progress,
                    message.as_mut_ptr(),
                    128,
                    context.as_mut_ptr(),
                    256
                ),
                0
            );
            assert_eq!(
                (progress.total, progress.emit, progress.install_check),
                (3, 1, 0)
            );
            while progress.complete == 0 {
                assert_eq!(
                    mradm_output_post_step(
                        pass,
                        &mut progress,
                        message.as_mut_ptr(),
                        128,
                        context.as_mut_ptr(),
                        256
                    ),
                    0
                );
            }
            assert_eq!(progress.done, 3);
            assert_eq!(
                mradm_output_post_finish(
                    pass,
                    &mut progress,
                    message.as_mut_ptr(),
                    128,
                    context.as_mut_ptr(),
                    256
                ),
                0
            );
            assert_eq!(
                mradm_output_post_finish(
                    pass,
                    &mut progress,
                    message.as_mut_ptr(),
                    128,
                    context.as_mut_ptr(),
                    256
                ),
                1
            );
            mradm_output_post_destroy(pass);

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
                    128,
                    ptr::null_mut(),
                    0
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
