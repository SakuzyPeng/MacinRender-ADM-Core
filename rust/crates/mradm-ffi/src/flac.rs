//! FLAC output (PCM consistency 7.2 A): the final float32 WAVE encoded by libFLAC's stream encoder
//! (24-bit, compression level 5) and the Vorbis comment written through libFLAC's metadata chain,
//! with the calls, order and bytes of the C++ island they replace (`flac_io.cpp`). libFLAC is
//! built by the islands' CMake project (`mradm-islands-sys`), not by a second Cargo build.
use mradm_engine::offline::Progress;
use mradm_engine::render::{MetadataFields, metadata_comment, operation, stage};
use mradm_engine::request::Error;
use mradm_wav::Reader;
use std::ffi::{CStr, c_char, c_int, c_void};
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;
use std::{ptr, slice};

const UNSUPPORTED: i32 = 2;
const IO_ERROR: i32 = 3;
const CANCELLED: i32 = 5;

const BITS: u32 = 24;
/// 2^23 − 1: ±1.0 maps to ±8388607.
const SCALE: f32 = 8_388_607.0;
const BLOCK: u64 = 4096;
const VORBIS_COMMENT: c_int = 4;

type Encoder = c_void;
type Chain = c_void;
type Iterator = c_void;
type Metadata = c_void;

#[repr(C)]
struct Entry {
    length: u32,
    entry: *mut u8,
}

/// `FLAC__IOCallbacks`.
#[repr(C)]
struct IoCallbacks {
    read: Option<unsafe extern "C" fn(*mut c_void, usize, usize, *mut c_void) -> usize>,
    write: Option<unsafe extern "C" fn(*const c_void, usize, usize, *mut c_void) -> usize>,
    seek: Option<unsafe extern "C" fn(*mut c_void, i64, c_int) -> c_int>,
    tell: Option<unsafe extern "C" fn(*mut c_void) -> i64>,
    eof: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    close: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
}

// Stable public libFLAC ABI (FLAC/stream_encoder.h, FLAC/metadata.h).
unsafe extern "C" {
    static FLAC__StreamEncoderInitStatusString: [*const c_char; 0];
    static FLAC__Metadata_ChainStatusString: [*const c_char; 0];

    fn FLAC__stream_encoder_new() -> *mut Encoder;
    fn FLAC__stream_encoder_delete(encoder: *mut Encoder);
    fn FLAC__stream_encoder_set_channels(encoder: *mut Encoder, value: u32) -> c_int;
    fn FLAC__stream_encoder_set_bits_per_sample(encoder: *mut Encoder, value: u32) -> c_int;
    fn FLAC__stream_encoder_set_sample_rate(encoder: *mut Encoder, value: u32) -> c_int;
    fn FLAC__stream_encoder_set_compression_level(encoder: *mut Encoder, value: u32) -> c_int;
    fn FLAC__stream_encoder_init_stream(
        encoder: *mut Encoder,
        write: unsafe extern "C" fn(
            *const Encoder,
            *const u8,
            usize,
            u32,
            u32,
            *mut c_void,
        ) -> c_int,
        seek: Option<unsafe extern "C" fn(*const Encoder, u64, *mut c_void) -> c_int>,
        tell: Option<unsafe extern "C" fn(*const Encoder, *mut u64, *mut c_void) -> c_int>,
        metadata: Option<unsafe extern "C" fn(*const Encoder, *const c_void, *mut c_void)>,
        client: *mut c_void,
    ) -> c_int;
    fn FLAC__stream_encoder_process_interleaved(
        encoder: *mut Encoder,
        buffer: *const i32,
        samples: u32,
    ) -> c_int;
    fn FLAC__stream_encoder_finish(encoder: *mut Encoder) -> c_int;

    fn FLAC__metadata_chain_new() -> *mut Chain;
    fn FLAC__metadata_chain_delete(chain: *mut Chain);
    fn FLAC__metadata_chain_status(chain: *mut Chain) -> c_int;
    fn FLAC__metadata_chain_read_with_callbacks(
        chain: *mut Chain,
        handle: *mut c_void,
        callbacks: IoCallbacks,
    ) -> c_int;
    fn FLAC__metadata_chain_sort_padding(chain: *mut Chain);
    fn FLAC__metadata_chain_write_with_callbacks_and_tempfile(
        chain: *mut Chain,
        use_padding: c_int,
        handle: *mut c_void,
        callbacks: IoCallbacks,
        temp_handle: *mut c_void,
        temp_callbacks: IoCallbacks,
    ) -> c_int;
    fn FLAC__metadata_object_new(kind: c_int) -> *mut Metadata;
    fn FLAC__metadata_object_delete(object: *mut Metadata);
    fn FLAC__metadata_object_vorbiscomment_append_comment(
        object: *mut Metadata,
        entry: Entry,
        copy: c_int,
    ) -> c_int;
    fn FLAC__metadata_iterator_new() -> *mut Iterator;
    fn FLAC__metadata_iterator_delete(iterator: *mut Iterator);
    fn FLAC__metadata_iterator_init(iterator: *mut Iterator, chain: *mut Chain);
    fn FLAC__metadata_iterator_next(iterator: *mut Iterator) -> c_int;
    fn FLAC__metadata_iterator_get_block_type(iterator: *const Iterator) -> c_int;
    fn FLAC__metadata_iterator_set_block(iterator: *mut Iterator, block: *mut Metadata) -> c_int;
    fn FLAC__metadata_iterator_insert_block_after(
        iterator: *mut Iterator,
        block: *mut Metadata,
    ) -> c_int;
}

/// The libFLAC name of status `index` in one of its status string tables.
fn status_text(table: *const [*const c_char; 0], index: c_int) -> String {
    let Ok(index) = usize::try_from(index) else {
        return String::new();
    };
    let entry = unsafe { *table.cast::<*const c_char>().add(index) };
    if entry.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(entry) }
        .to_string_lossy()
        .into_owned()
}

fn failure(code: i32, message: impl Into<String>, path: &str) -> Error {
    Error::new(code, message, format!("path={path}"))
}

// ── Encoding ────────────────────────────────────────────────────────────────────

/// The encoder's sink: the output file, written through a buffer as the C++ island's `FILE*`.
struct Sink {
    file: BufWriter<File>,
}

unsafe extern "C" fn sink_write(
    _: *const Encoder,
    buffer: *const u8,
    bytes: usize,
    _: u32,
    _: u32,
    client: *mut c_void,
) -> c_int {
    let sink = unsafe { &mut *client.cast::<Sink>() };
    let data = if bytes == 0 {
        &[][..]
    } else {
        unsafe { slice::from_raw_parts(buffer, bytes) }
    };
    match catch_unwind(AssertUnwindSafe(|| sink.file.write_all(data))) {
        Ok(Ok(())) => 0,
        _ => 1,
    }
}

unsafe extern "C" fn sink_seek(_: *const Encoder, offset: u64, client: *mut c_void) -> c_int {
    let sink = unsafe { &mut *client.cast::<Sink>() };
    match catch_unwind(AssertUnwindSafe(|| sink.file.seek(SeekFrom::Start(offset)))) {
        Ok(Ok(_)) => 0,
        _ => 1,
    }
}

unsafe extern "C" fn sink_tell(_: *const Encoder, offset: *mut u64, client: *mut c_void) -> c_int {
    let sink = unsafe { &mut *client.cast::<Sink>() };
    match catch_unwind(AssertUnwindSafe(|| sink.file.stream_position())) {
        Ok(Ok(position)) if !offset.is_null() => {
            unsafe { *offset = position };
            0
        }
        _ => 1,
    }
}

/// An initialised encoder; dropping it finishes the stream (rewriting STREAMINFO through the seek
/// callback), then closes the file, as the C++ writer's destructor did.
struct Writer {
    encoder: *mut Encoder,
    sink: Box<Sink>,
    channels: u32,
    samples: Vec<i32>,
}

impl Writer {
    fn open(path: &str, channels: u32, rate: u32) -> Result<Self, Error> {
        if !(1..=8).contains(&channels) {
            return Err(failure(
                UNSUPPORTED,
                format!("FLAC supports 1-8 channels, got {channels}"),
                path,
            ));
        }
        let encoder = unsafe { FLAC__stream_encoder_new() };
        if encoder.is_null() {
            return Err(failure(IO_ERROR, "failed to allocate FLAC encoder", path));
        }
        unsafe {
            FLAC__stream_encoder_set_channels(encoder, channels);
            FLAC__stream_encoder_set_bits_per_sample(encoder, BITS);
            FLAC__stream_encoder_set_sample_rate(encoder, rate);
            FLAC__stream_encoder_set_compression_level(encoder, 5);
        }
        let Ok(file) = File::create(path) else {
            unsafe { FLAC__stream_encoder_delete(encoder) };
            return Err(failure(
                IO_ERROR,
                "failed to open FLAC file for writing",
                path,
            ));
        };
        let mut sink = Box::new(Sink {
            file: BufWriter::new(file),
        });
        let status = unsafe {
            FLAC__stream_encoder_init_stream(
                encoder,
                sink_write,
                Some(sink_seek),
                Some(sink_tell),
                None,
                ptr::from_mut(sink.as_mut()).cast(),
            )
        };
        if status != 0 {
            unsafe { FLAC__stream_encoder_delete(encoder) };
            let text = status_text(&raw const FLAC__StreamEncoderInitStatusString, status);
            return Err(failure(
                IO_ERROR,
                format!("FLAC encoder init failed: {text}"),
                path,
            ));
        }
        Ok(Self {
            encoder,
            sink,
            channels,
            samples: Vec::new(),
        })
    }

    /// `frames` interleaved frames of `pcm`, clamped to ±1 and rounded half away from zero.
    fn write(&mut self, pcm: &[f32], frames: u64) -> bool {
        let total = frames as usize * self.channels as usize;
        self.samples.clear();
        self.samples.extend(
            pcm[..total]
                .iter()
                .map(|&x| (x.clamp(-1., 1.) * SCALE).round() as i32),
        );
        unsafe {
            FLAC__stream_encoder_process_interleaved(
                self.encoder,
                self.samples.as_ptr(),
                frames as u32,
            ) != 0
        }
    }
}

impl Drop for Writer {
    fn drop(&mut self) {
        unsafe {
            FLAC__stream_encoder_finish(self.encoder);
            FLAC__stream_encoder_delete(self.encoder);
        }
        let _ = self.sink.file.flush();
    }
}

fn encode_progress(fraction: f64, current: u64, total: u64, message: &'static str) -> Progress {
    let f = if fraction.is_nan() {
        0.
    } else {
        fraction.clamp(0., 1.)
    };
    Progress {
        stage: stage::POST_PROCESSING,
        operation: operation::ENCODE_FLAC,
        fraction: f,
        stage_fraction: f,
        current,
        total,
        message,
    }
}

/// Encodes the fully post-processed float32 WAVE `src` to `dst` (`convert_to_flac`): one
/// quantisation, at 24 bits. `cancelled` is polled before each 4096-frame block.
pub(crate) fn encode(
    src: &str,
    dst: &str,
    cancelled: &dyn Fn() -> bool,
    progress: &dyn Fn(&Progress),
) -> Result<(), Error> {
    if cancelled() {
        return Err(failure(CANCELLED, "render cancelled", dst));
    }
    let mut reader = File::open(src)
        .map_err(mradm_wav::Error::from)
        .and_then(|f| Reader::new(BufReader::new(f)))
        .map_err(|e| failure(wav_code(e.code), e.message, src))?;
    let info = reader.info();
    let channels = u32::from(info.format.channels);
    let mut writer = Writer::open(dst, channels, info.format.sample_rate)?;
    let mut pcm = vec![0f32; channels as usize * BLOCK as usize];
    let total = info.frames;
    let mut left = total;
    let mut done = 0u64;
    progress(&encode_progress(0., 0, total, "encoding FLAC"));
    while left > 0 {
        if cancelled() {
            return Err(failure(CANCELLED, "render cancelled", dst));
        }
        let n = BLOCK.min(left);
        // A failed read ends the loop, as the C++ reader's 0 did.
        let got = reader
            .read_frames(&mut pcm[..(n as usize * channels as usize)])
            .unwrap_or(0);
        if got == 0 {
            break;
        }
        if !writer.write(&pcm, got) {
            return Err(failure(IO_ERROR, "short write in convert_to_flac", dst));
        }
        left -= got;
        done += got;
        progress(&encode_progress(
            done as f64 / total.max(1) as f64,
            done,
            total,
            "encoding FLAC",
        ));
    }
    if left != 0 {
        return Err(failure(IO_ERROR, "short read in convert_to_flac", src));
    }
    progress(&encode_progress(1., total, total, "FLAC encoded"));
    Ok(())
}

/// The `adm::ErrorCode` of a WAVE failure (as `wav_error`).
fn wav_code(code: i32) -> i32 {
    if (1..=6).contains(&code) { code } else { 6 }
}

// ── The Vorbis comment ──────────────────────────────────────────────────────────

/// A metadata I/O handle: the source read back and rewritten, or the temporary file written.
/// `eof` follows `feof`: set by a short read, cleared by a seek.
struct Handle {
    file: Option<BufReader<File>>,
    temp: Option<BufWriter<File>>,
    eof: bool,
}

unsafe extern "C" fn io_read(out: *mut c_void, size: usize, count: usize, h: *mut c_void) -> usize {
    let h = unsafe { &mut *h.cast::<Handle>() };
    let Some(file) = h.file.as_mut() else {
        return 0;
    };
    let Some(want) = size.checked_mul(count) else {
        return 0;
    };
    if want == 0 {
        return 0;
    }
    let buffer = unsafe { slice::from_raw_parts_mut(out.cast::<u8>(), want) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut got = 0;
        while got < want {
            match file.read(&mut buffer[got..]) {
                Ok(0) => break,
                Ok(n) => got += n,
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                Err(_) => break,
            }
        }
        got
    }));
    let got = result.unwrap_or(0);
    if got < want {
        h.eof = true;
    }
    got / size
}

unsafe extern "C" fn io_write(
    data: *const c_void,
    size: usize,
    count: usize,
    h: *mut c_void,
) -> usize {
    let h = unsafe { &mut *h.cast::<Handle>() };
    let Some(file) = h.temp.as_mut() else {
        return 0;
    };
    let Some(len) = size.checked_mul(count) else {
        return 0;
    };
    if len == 0 {
        return 0;
    }
    let bytes = unsafe { slice::from_raw_parts(data.cast::<u8>(), len) };
    match catch_unwind(AssertUnwindSafe(|| file.write_all(bytes))) {
        Ok(Ok(())) => count,
        _ => 0,
    }
}

unsafe extern "C" fn io_seek(h: *mut c_void, offset: i64, whence: c_int) -> c_int {
    let h = unsafe { &mut *h.cast::<Handle>() };
    let Some(file) = h.file.as_mut() else {
        return -1;
    };
    let target = match whence {
        0 => match u64::try_from(offset) {
            Ok(o) => SeekFrom::Start(o),
            Err(_) => return -1,
        },
        1 => SeekFrom::Current(offset),
        2 => SeekFrom::End(offset),
        _ => return -1,
    };
    match catch_unwind(AssertUnwindSafe(|| file.seek(target))) {
        Ok(Ok(_)) => {
            h.eof = false;
            0
        }
        _ => -1,
    }
}

unsafe extern "C" fn io_tell(h: *mut c_void) -> i64 {
    let h = unsafe { &mut *h.cast::<Handle>() };
    let Some(file) = h.file.as_mut() else {
        return -1;
    };
    match catch_unwind(AssertUnwindSafe(|| file.stream_position())) {
        Ok(Ok(p)) => i64::try_from(p).unwrap_or(-1),
        _ => -1,
    }
}

unsafe extern "C" fn io_eof(h: *mut c_void) -> c_int {
    c_int::from(unsafe { &*h.cast::<Handle>() }.eof)
}

fn source_callbacks() -> IoCallbacks {
    IoCallbacks {
        read: Some(io_read),
        write: None,
        seek: Some(io_seek),
        tell: Some(io_tell),
        eof: Some(io_eof),
        close: None,
    }
}

fn temp_callbacks() -> IoCallbacks {
    IoCallbacks {
        read: None,
        write: Some(io_write),
        seek: None,
        tell: None,
        eof: None,
        close: None,
    }
}

/// The tags of the Vorbis comment (`write_flac_metadata`), in order.
fn tags(m: &MetadataFields) -> Vec<String> {
    let mut tags = Vec::new();
    if !m.encoder.is_empty() {
        tags.push(format!("ENCODER={}", m.encoder));
    }
    if !m.date_utc.is_empty() {
        tags.push(format!("DATE={}", m.date_utc));
    }
    if !m.renderer.is_empty() || !m.output_layout.is_empty() {
        tags.push(format!("COMMENT={}", metadata_comment(m)));
    }
    // WAVEFORMATEXTENSIBLE_CHANNEL_MASK, ffmpeg's convention for multichannel FLAC.
    let mask = match m.output_layout.as_str() {
        "0+2+0" => Some(0x0003u32),
        "0+5+0" => Some(0x003F),
        "wav71" => Some(0x063F),
        _ => None,
    };
    if let Some(mask) = mask {
        tags.push(format!("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x{mask:08X}"));
    }
    tags
}

/// The chain, iterator and new block of one metadata write, freed on every path.
struct Objects {
    chain: *mut Chain,
    iterator: *mut Iterator,
    /// The new Vorbis comment until the chain owns it.
    block: *mut Metadata,
}

impl Drop for Objects {
    fn drop(&mut self) {
        unsafe {
            if !self.iterator.is_null() {
                FLAC__metadata_iterator_delete(self.iterator);
            }
            if !self.block.is_null() {
                FLAC__metadata_object_delete(self.block);
            }
            if !self.chain.is_null() {
                FLAC__metadata_chain_delete(self.chain);
            }
        }
    }
}

/// Replaces (or inserts after STREAMINFO) the Vorbis comment of the FLAC file at `path`
/// (`write_flac_metadata`): ENCODER, DATE, COMMENT and, for recognised layouts, the channel mask.
pub(crate) fn write_metadata(path: &str, m: &MetadataFields) -> Result<(), Error> {
    let mut objects = Objects {
        chain: unsafe { FLAC__metadata_chain_new() },
        iterator: ptr::null_mut(),
        block: ptr::null_mut(),
    };
    if objects.chain.is_null() {
        return Err(failure(IO_ERROR, "FLAC metadata chain alloc failed", path));
    }
    let Ok(file) = File::open(path) else {
        return Err(failure(
            IO_ERROR,
            "failed to open FLAC file for metadata",
            path,
        ));
    };
    let mut source = Box::new(Handle {
        file: Some(BufReader::new(file)),
        temp: None,
        eof: false,
    });
    let source_ptr: *mut c_void = ptr::from_mut(source.as_mut()).cast();
    if unsafe {
        FLAC__metadata_chain_read_with_callbacks(objects.chain, source_ptr, source_callbacks())
    } == 0
    {
        let status = unsafe { FLAC__metadata_chain_status(objects.chain) };
        let text = status_text(&raw const FLAC__Metadata_ChainStatusString, status);
        return Err(failure(
            IO_ERROR,
            format!("FLAC metadata chain read failed: {text}"),
            path,
        ));
    }
    objects.block = unsafe { FLAC__metadata_object_new(VORBIS_COMMENT) };
    if objects.block.is_null() {
        return Err(failure(IO_ERROR, "FLAC Vorbis Comment alloc failed", path));
    }
    let mut tags_ok = true;
    for tag in tags(m) {
        let mut bytes = tag.into_bytes();
        let entry = Entry {
            length: bytes.len() as u32,
            entry: bytes.as_mut_ptr(),
        };
        tags_ok &= unsafe {
            FLAC__metadata_object_vorbiscomment_append_comment(objects.block, entry, 1) != 0
        };
    }
    if !tags_ok {
        return Err(failure(
            IO_ERROR,
            "FLAC Vorbis Comment entry append failed (OOM?)",
            path,
        ));
    }
    objects.iterator = unsafe { FLAC__metadata_iterator_new() };
    if objects.iterator.is_null() {
        return Err(failure(
            IO_ERROR,
            "FLAC metadata iterator alloc failed",
            path,
        ));
    }
    unsafe { FLAC__metadata_iterator_init(objects.iterator, objects.chain) };
    // Replace the encoder's (empty) Vorbis comment: two would violate the FLAC format.
    let mut replaced = false;
    loop {
        if unsafe { FLAC__metadata_iterator_get_block_type(objects.iterator) } == VORBIS_COMMENT {
            if unsafe { FLAC__metadata_iterator_set_block(objects.iterator, objects.block) } == 0 {
                return Err(failure(IO_ERROR, "FLAC VC block replace failed", path));
            }
            objects.block = ptr::null_mut();
            replaced = true;
            break;
        }
        if unsafe { FLAC__metadata_iterator_next(objects.iterator) } == 0 {
            break;
        }
    }
    if !replaced {
        unsafe { FLAC__metadata_iterator_init(objects.iterator, objects.chain) };
        if unsafe { FLAC__metadata_iterator_insert_block_after(objects.iterator, objects.block) }
            == 0
        {
            return Err(failure(IO_ERROR, "FLAC VC block insert failed", path));
        }
        objects.block = ptr::null_mut();
    }
    unsafe {
        FLAC__metadata_iterator_delete(objects.iterator);
        objects.iterator = ptr::null_mut();
        FLAC__metadata_chain_sort_padding(objects.chain);
    }
    let temp_path = format!("{path}.metadata_tmp");
    let Ok(temp_file) = File::create(&temp_path) else {
        return Err(failure(
            IO_ERROR,
            "failed to open FLAC metadata temp file",
            &temp_path,
        ));
    };
    let mut temp = Box::new(Handle {
        file: None,
        temp: Some(BufWriter::new(temp_file)),
        eof: false,
    });
    let temp_ptr: *mut c_void = ptr::from_mut(temp.as_mut()).cast();
    let ok = unsafe {
        FLAC__metadata_chain_write_with_callbacks_and_tempfile(
            objects.chain,
            1,
            source_ptr,
            source_callbacks(),
            temp_ptr,
            temp_callbacks(),
        )
    };
    let status = unsafe { FLAC__metadata_chain_status(objects.chain) };
    drop(objects);
    let flushed = temp.temp.take().is_some_and(|mut w| w.flush().is_ok());
    drop(source);
    if ok == 0 {
        let _ = std::fs::remove_file(&temp_path);
        let text = status_text(&raw const FLAC__Metadata_ChainStatusString, status);
        return Err(failure(
            IO_ERROR,
            format!("FLAC metadata chain write failed: {text}"),
            path,
        ));
    }
    if !flushed || std::fs::rename(&temp_path, path).is_err() {
        let _ = std::fs::remove_file(&temp_path);
        return Err(failure(
            IO_ERROR,
            "failed to replace FLAC file after metadata write",
            path,
        ));
    }
    Ok(())
}

/// Whether `path` names a FLAC file (the extension, ASCII case-insensitive).
pub(crate) fn is_flac(path: &str) -> bool {
    Path::new(path)
        .extension()
        .is_some_and(|e| e.eq_ignore_ascii_case("flac"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};
    use std::ffi::CString;
    use std::path::PathBuf;

    /// The leading fields of `FLAC__FrameHeader`.
    #[repr(C)]
    struct FrameHeader {
        blocksize: u32,
        sample_rate: u32,
        channels: u32,
    }
    type Decoder = c_void;
    unsafe extern "C" {
        fn FLAC__stream_decoder_new() -> *mut Decoder;
        fn FLAC__stream_decoder_delete(decoder: *mut Decoder);
        fn FLAC__stream_decoder_set_md5_checking(decoder: *mut Decoder, value: c_int) -> c_int;
        fn FLAC__stream_decoder_init_file(
            decoder: *mut Decoder,
            path: *const c_char,
            write: unsafe extern "C" fn(
                *const Decoder,
                *const FrameHeader,
                *const *const i32,
                *mut c_void,
            ) -> c_int,
            metadata: Option<unsafe extern "C" fn(*const Decoder, *const c_void, *mut c_void)>,
            error: unsafe extern "C" fn(*const Decoder, c_int, *mut c_void),
            client: *mut c_void,
        ) -> c_int;
        fn FLAC__stream_decoder_process_until_end_of_stream(decoder: *mut Decoder) -> c_int;
        fn FLAC__stream_decoder_finish(decoder: *mut Decoder) -> c_int;
    }

    #[derive(Default)]
    struct Decoded {
        channels: u32,
        rate: u32,
        samples: Vec<i32>,
        errors: u32,
    }

    unsafe extern "C" fn decoded_frame(
        _: *const Decoder,
        frame: *const FrameHeader,
        buffer: *const *const i32,
        client: *mut c_void,
    ) -> c_int {
        let d = unsafe { &mut *client.cast::<Decoded>() };
        let h = unsafe { &*frame };
        d.channels = h.channels;
        d.rate = h.sample_rate;
        let planes: Vec<&[i32]> = (0..h.channels as usize)
            .map(|c| unsafe { slice::from_raw_parts(*buffer.add(c), h.blocksize as usize) })
            .collect();
        for i in 0..h.blocksize as usize {
            d.samples.extend(planes.iter().map(|p| p[i]));
        }
        0
    }

    unsafe extern "C" fn decode_error(_: *const Decoder, _: c_int, client: *mut c_void) {
        unsafe { (*client.cast::<Decoded>()).errors += 1 };
    }

    /// The interleaved 24-bit samples of the FLAC file at `path`, decoded by libFLAC.
    fn decode(path: &Path) -> Decoded {
        let mut d = Decoded::default();
        let name = CString::new(path.to_str().unwrap()).unwrap();
        unsafe {
            let decoder = FLAC__stream_decoder_new();
            assert_ne!(FLAC__stream_decoder_set_md5_checking(decoder, 1), 0);
            assert_eq!(
                FLAC__stream_decoder_init_file(
                    decoder,
                    name.as_ptr(),
                    decoded_frame,
                    None,
                    decode_error,
                    ptr::from_mut(&mut d).cast(),
                ),
                0
            );
            assert_ne!(FLAC__stream_decoder_process_until_end_of_stream(decoder), 0);
            assert_ne!(FLAC__stream_decoder_finish(decoder), 0, "MD5 mismatch");
            FLAC__stream_decoder_delete(decoder);
        }
        assert_eq!(d.errors, 0);
        d
    }

    /// The metadata block types of the FLAC file at `path` and the tags of its Vorbis comments.
    fn metadata(path: &Path) -> (Vec<u8>, Vec<String>) {
        let bytes = std::fs::read(path).unwrap();
        assert_eq!(&bytes[..4], b"fLaC");
        let (mut types, mut tags, mut at) = (Vec::new(), Vec::new(), 4);
        loop {
            let header = bytes[at];
            let len = u32::from_be_bytes([0, bytes[at + 1], bytes[at + 2], bytes[at + 3]]) as usize;
            let body = &bytes[at + 4..at + 4 + len];
            types.push(header & 0x7f);
            if header & 0x7f == 4 {
                let u32le =
                    |o: usize| u32::from_le_bytes(body[o..o + 4].try_into().unwrap()) as usize;
                let mut o = 4 + u32le(0);
                let count = u32le(o);
                o += 4;
                for _ in 0..count {
                    let n = u32le(o);
                    tags.push(String::from_utf8(body[o + 4..o + 4 + n].to_vec()).unwrap());
                    o += 4 + n;
                }
            }
            at += 4 + len;
            if header & 0x80 != 0 {
                return (types, tags);
            }
        }
    }

    struct Dir(PathBuf);
    impl Dir {
        fn new(name: &str) -> Self {
            // Bytes that name different characters in UTF-8 and legacy code pages.
            let path = std::env::temp_dir()
                .join(format!("mradm-flac-\u{a9}-{name}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&path);
            std::fs::create_dir_all(&path).unwrap();
            Self(path)
        }
        fn file(&self, name: &str) -> String {
            self.0.join(name).to_str().unwrap().to_owned()
        }
    }
    impl Drop for Dir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    /// A float32 WAVE of `channels` × `frames` with values past ±1 and exact rounding ties.
    fn source(path: &str, channels: u32, frames: usize) -> Vec<f32> {
        let mut pcm = Vec::with_capacity(frames * channels as usize);
        for i in 0..frames * channels as usize {
            pcm.push(match i % 7 {
                0 => 1.25,
                1 => -1.5,
                2 => 0.5 / SCALE,
                3 => -2.5 / SCALE,
                _ => ((i * 37 % 200) as f32 - 100.) / 256.,
            });
        }
        let mut w =
            mradm_wav::create_file_writer(Path::new(path), channels, 48000, 32, true, false)
                .unwrap();
        w.write_frames(&pcm).unwrap();
        w.finish().unwrap();
        pcm
    }

    fn fields(layout: &str) -> MetadataFields {
        MetadataFields {
            encoder: "MacinRender ADM Core".into(),
            date_utc: "2026-11-19T00:00:00Z".into(),
            renderer: "vbap-saf".into(),
            output_layout: layout.into(),
            lufs: Some(-23.04),
            peak_dbtp: None,
        }
    }

    #[test]
    fn encoding_round_trips_the_quantised_samples() {
        let dir = Dir::new("roundtrip");
        let (src, dst) = (dir.file("in.wav"), dir.file("out.flac"));
        let pcm = source(&src, 3, 10_000);
        let events = RefCell::new(Vec::new());
        encode(&src, &dst, &|| false, &|p| events.borrow_mut().push(*p)).unwrap();
        let d = decode(Path::new(&dst));
        assert_eq!((d.channels, d.rate), (3, 48000));
        let expected: Vec<i32> = pcm
            .iter()
            .map(|&x| (x.clamp(-1., 1.) * SCALE).round() as i32)
            .collect();
        assert_eq!(d.samples, expected);
        assert_eq!(expected[2..4], [1, -3]);
        let events = events.into_inner();
        // 0, three 4096-frame blocks, the last, and "FLAC encoded".
        assert_eq!(events.len(), 5);
        assert!(events.iter().all(|e| e.stage == stage::POST_PROCESSING
            && e.operation == operation::ENCODE_FLAC
            && e.total == 10_000));
        assert_eq!(events[1].current, 4096);
        assert_eq!(
            (events[4].fraction, events[4].message),
            (1., "FLAC encoded")
        );
        // The encoder's Vorbis comment is the only one until the metadata replaces it.
        assert_eq!(
            metadata(Path::new(&dst))
                .0
                .iter()
                .filter(|&&t| t == 4)
                .count(),
            1
        );
    }

    #[test]
    fn the_vorbis_comment_is_replaced_once_with_the_channel_mask() {
        let dir = Dir::new("comment");
        let (src, dst) = (dir.file("in.wav"), dir.file("out.flac"));
        source(&src, 2, 3000);
        encode(&src, &dst, &|| false, &|_| {}).unwrap();
        write_metadata(&dst, &fields("0+2+0")).unwrap();
        write_metadata(&dst, &fields("0+2+0")).unwrap();
        let (types, tags) = metadata(Path::new(&dst));
        assert_eq!(types.iter().filter(|&&t| t == 4).count(), 1, "{types:?}");
        assert_eq!(types[0], 0);
        assert_eq!(
            tags,
            [
                "ENCODER=MacinRender ADM Core",
                "DATE=2026-11-19T00:00:00Z",
                "COMMENT=renderer=vbap-saf layout=0+2+0 loudness=-23.0LUFS",
                "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x00000003",
            ]
        );
        assert!(!Path::new(&format!("{dst}.metadata_tmp")).exists());
        // The audio is untouched by the rewrite.
        assert_eq!(decode(Path::new(&dst)).samples.len(), 6000);
    }

    #[test]
    fn cancellation_is_polled_before_and_while_encoding() {
        let dir = Dir::new("cancel");
        let src = dir.file("in.wav");
        source(&src, 1, 48000 * 3);
        let before = dir.file("before.flac");
        let e = encode(&src, &before, &|| true, &|_| {}).unwrap_err();
        assert_eq!(
            (e.code, e.context.as_str()),
            (CANCELLED, format!("path={before}").as_str())
        );
        assert!(
            !Path::new(&before).exists(),
            "a pre-cancelled encoder leaves no output"
        );
        assert!(Path::new(&src).exists());

        let events = Cell::new(0);
        let during = dir.file("during.flac");
        let e = encode(&src, &during, &|| events.get() >= 2, &|_| {
            events.set(events.get() + 1)
        })
        .unwrap_err();
        assert_eq!(e.code, CANCELLED);
        assert_eq!(
            events.get(),
            2,
            "the cancelled encoder stops at the next block"
        );
    }

    #[test]
    fn failures_name_the_path() {
        let dir = Dir::new("failures");
        let missing = dir.file("missing.wav");
        let e = encode(&missing, &dir.file("out.flac"), &|| false, &|_| {}).unwrap_err();
        assert_eq!((e.code, e.context), (IO_ERROR, format!("path={missing}")));
        let src = dir.file("wide.wav");
        source(&src, 9, 10);
        let e = encode(&src, &dir.file("wide.flac"), &|| false, &|_| {}).unwrap_err();
        assert_eq!(
            (e.code, e.message.as_str()),
            (UNSUPPORTED, "FLAC supports 1-8 channels, got 9")
        );
        let e = write_metadata(&missing, &fields("")).unwrap_err();
        assert_eq!(e.message, "failed to open FLAC file for metadata");
        std::fs::write(&missing, b"not a FLAC file").unwrap();
        let e = write_metadata(&missing, &fields("")).unwrap_err();
        assert!(
            e.message
                .starts_with("FLAC metadata chain read failed: FLAC__METADATA_CHAIN_STATUS_"),
            "{}",
            e.message
        );
    }

    #[test]
    fn tags_follow_the_island_order_and_masks() {
        let mut m = fields("0+5+0");
        m.peak_dbtp = Some(-1.005);
        assert_eq!(
            tags(&m),
            [
                "ENCODER=MacinRender ADM Core",
                "DATE=2026-11-19T00:00:00Z",
                "COMMENT=renderer=vbap-saf layout=0+5+0 loudness=-23.0LUFS peak=-1.00dBTP",
                "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0000003F",
            ]
        );
        m.output_layout = "wav71".into();
        assert_eq!(tags(&m)[3], "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0000063F");
        m.output_layout = "2+7+4".into();
        m.renderer.clear();
        m.encoder.clear();
        assert_eq!(
            tags(&m),
            [
                "DATE=2026-11-19T00:00:00Z",
                "COMMENT=layout=2+7+4 loudness=-23.0LUFS peak=-1.00dBTP"
            ]
        );
    }

    #[test]
    fn quantisation_clamps_and_rounds_half_away_from_zero() {
        let q = |x: f32| (x.clamp(-1., 1.) * SCALE).round() as i32;
        assert_eq!(q(1.5), 8_388_607);
        assert_eq!(q(-2.), -8_388_607);
        assert_eq!(q(0.5 / SCALE), 1);
        assert_eq!(q(-0.5 / SCALE), -1);
        assert_eq!(q(f32::NAN), 0);
    }

    #[test]
    fn the_extension_is_case_insensitive() {
        assert!(is_flac("a/b.FLAC"));
        assert!(!is_flac("a/flac"));
        assert!(!is_flac("a/b.mka"));
    }
}
