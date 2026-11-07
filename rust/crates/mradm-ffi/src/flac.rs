//! Strict validation before the compatibility dr_flac reader supplies PCM.
//! libFLAC is provided by the existing CMake provider, not a second Cargo build.
use crate::wav::{self, Info};
use mradm_wav::{Error, Result, flac::Input};
use std::{
    ffi::c_void,
    io::{Chain, Cursor, Read},
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice,
};

// Stable public libFLAC ABI (FLAC/format.h and FLAC/stream_decoder.h). The frame
// callback borrows only the leading FrameHeader, never the following subframes.
#[repr(C)]
struct FrameHeader {
    blocksize: u32,
    sample_rate: u32,
    channels: u32,
    channel_assignment: i32,
    bits_per_sample: u32,
    number_type: i32,
    sample_number: u64,
    crc: u8,
}
type Decoder = c_void;
unsafe extern "C" {
    fn FLAC__stream_decoder_new() -> *mut Decoder;
    fn FLAC__stream_decoder_delete(decoder: *mut Decoder);
    fn FLAC__stream_decoder_set_md5_checking(decoder: *mut Decoder, enabled: i32) -> i32;
    fn FLAC__stream_decoder_init_stream(
        decoder: *mut Decoder,
        read: unsafe extern "C" fn(*const Decoder, *mut u8, *mut usize, *mut c_void) -> i32,
        seek: Option<unsafe extern "C" fn(*const Decoder, u64, *mut c_void) -> i32>,
        tell: Option<unsafe extern "C" fn(*const Decoder, *mut u64, *mut c_void) -> i32>,
        length: Option<unsafe extern "C" fn(*const Decoder, *mut u64, *mut c_void) -> i32>,
        eof: Option<unsafe extern "C" fn(*const Decoder, *mut c_void) -> i32>,
        write: unsafe extern "C" fn(
            *const Decoder,
            *const FrameHeader,
            *const *const i32,
            *mut c_void,
        ) -> i32,
        metadata: Option<unsafe extern "C" fn(*const Decoder, *const c_void, *mut c_void)>,
        error: unsafe extern "C" fn(*const Decoder, i32, *mut c_void),
        client: *mut c_void,
    ) -> i32;
    fn FLAC__stream_decoder_process_until_end_of_metadata(decoder: *mut Decoder) -> i32;
    fn FLAC__stream_decoder_process_single(decoder: *mut Decoder) -> i32;
    fn FLAC__stream_decoder_get_state(decoder: *const Decoder) -> i32;
    fn FLAC__stream_decoder_get_decode_position(decoder: *const Decoder, position: *mut u64)
    -> i32;
    fn FLAC__stream_decoder_finish(decoder: *mut Decoder) -> i32;
}

struct Owner(*mut Decoder);
impl Drop for Owner {
    fn drop(&mut self) {
        unsafe { FLAC__stream_decoder_delete(self.0) };
    }
}

struct State {
    input: Chain<Cursor<[u8; 42]>, Input>,
    bytes: u64,
    frames: u64,
    info: Info,
    failure: Option<Error>,
}

fn corrupt(message: &str) -> Error {
    Error {
        code: 3,
        message: format!("invalid FLAC: {message}"),
    }
}

// Callbacks never unwind into C, including allocation or I/O panics.
unsafe extern "C" fn read(
    _: *const Decoder,
    out: *mut u8,
    size: *mut usize,
    client: *mut c_void,
) -> i32 {
    let state = unsafe { &mut *client.cast::<State>() };
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<usize> {
        if state.failure.is_some() {
            return Ok(0);
        }
        let count = unsafe { *size };
        if count == 0 {
            return Err(corrupt("empty decoder read request"));
        }
        let count = state
            .input
            .read(unsafe { slice::from_raw_parts_mut(out, count) })?;
        state.bytes = state
            .bytes
            .checked_add(count as u64)
            .ok_or_else(|| corrupt("byte count overflow"))?;
        Ok(count)
    }));
    let count = match result {
        Ok(Ok(n)) => n,
        Ok(Err(e)) => {
            state.failure = Some(e);
            0
        }
        Err(_) => {
            state.failure = Some(corrupt("input callback panicked"));
            0
        }
    };
    unsafe { *size = count };
    if state.failure.is_some() {
        2
    } else if count == 0 {
        1
    } else {
        0
    }
}

unsafe extern "C" fn tell(_: *const Decoder, position: *mut u64, client: *mut c_void) -> i32 {
    unsafe { *position = (*client.cast::<State>()).bytes };
    0
}

unsafe extern "C" fn write(
    _: *const Decoder,
    frame: *const FrameHeader,
    _: *const *const i32,
    client: *mut c_void,
) -> i32 {
    let state = unsafe { &mut *client.cast::<State>() };
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<()> {
        if state.failure.is_some() {
            return Ok(());
        }
        let h = unsafe { &*frame };
        if h.number_type != 1 || h.sample_number != state.frames || h.blocksize == 0 {
            return Err(corrupt("missing, repeated or out-of-order audio frame"));
        }
        if h.channels != state.info.channels
            || h.sample_rate != state.info.rate
            || h.bits_per_sample != u32::from(state.info.bits)
        {
            return Err(corrupt("frame format differs from STREAMINFO"));
        }
        state.frames = state
            .frames
            .checked_add(u64::from(h.blocksize))
            .ok_or_else(|| corrupt("sample count overflow"))?;
        Ok(())
    }));
    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => state.failure = Some(e),
        Err(_) => state.failure = Some(corrupt("frame callback panicked")),
    }
    i32::from(state.failure.is_some())
}

unsafe extern "C" fn error(_: *const Decoder, status: i32, client: *mut c_void) {
    let state = unsafe { &mut *client.cast::<State>() };
    // Error construction is covered too: no Rust unwind may cross libFLAC.
    let result = catch_unwind(AssertUnwindSafe(|| {
        if state.failure.is_none() {
            state.failure = Some(corrupt(match status {
                0 => "lost frame synchronization",
                1 => "invalid frame header",
                2 => "frame CRC mismatch",
                4 => "invalid metadata",
                6 => "missing audio frame",
                _ => "decoder rejected the stream",
            }));
        }
    }));
    if result.is_err() {
        std::process::abort();
    }
}

fn position(decoder: *const Decoder) -> Result<u64> {
    let mut at = 0;
    if unsafe { FLAC__stream_decoder_get_decode_position(decoder, &mut at) } == 0 {
        return Err(corrupt("decoder position unavailable"));
    }
    Ok(at)
}

fn validate(path: &str) -> Result<Info> {
    let mut input = Input::open(path)?;
    let mut prefix = [0_u8; 42];
    input.read_exact(&mut prefix)?;
    if &prefix[..4] != b"fLaC" || prefix[4] & 127 != 0 || prefix[5..8] != [0, 0, 34] {
        return Err(corrupt("missing STREAMINFO"));
    }
    let packed = u64::from_be_bytes(prefix[18..26].try_into().unwrap());
    let info = Info {
        frames: packed & ((1_u64 << 36) - 1),
        channels: ((packed >> 41) & 7) as u32 + 1,
        rate: (packed >> 44) as u32,
        bits: ((packed >> 36) & 31) as u16 + 1,
        format: 1,
        channel_mask: 0,
    };
    if info.rate == 0 || !(4..=32).contains(&info.bits) {
        return Err(corrupt("invalid STREAMINFO format"));
    }
    let mut state = State {
        input: Cursor::new(prefix).chain(input),
        bytes: 0,
        frames: 0,
        info,
        failure: None,
    };
    let pointer = unsafe { FLAC__stream_decoder_new() };
    if pointer.is_null() {
        return Err(corrupt("failed to allocate decoder"));
    }
    let decoder = Owner(pointer);
    unsafe {
        FLAC__stream_decoder_set_md5_checking(decoder.0, 1);
        if FLAC__stream_decoder_init_stream(
            decoder.0,
            read,
            None,
            Some(tell),
            None,
            None,
            write,
            None,
            error,
            (&raw mut state).cast(),
        ) != 0
        {
            return Err(corrupt("failed to initialize decoder"));
        }
    }
    if unsafe { FLAC__stream_decoder_process_until_end_of_metadata(decoder.0) } == 0
        || state.failure.is_some()
    {
        return Err(state
            .failure
            .take()
            .unwrap_or_else(|| corrupt("incomplete metadata")));
    }
    let mut complete_end = position(decoder.0)?;
    while unsafe { FLAC__stream_decoder_get_state(decoder.0) } != 4 {
        let before = state.frames;
        let ok = unsafe { FLAC__stream_decoder_process_single(decoder.0) };
        if let Some(e) = state.failure.take() {
            return Err(e);
        }
        if ok == 0 {
            return Err(corrupt("incomplete audio frame"));
        }
        if state.frames != before {
            complete_end = position(decoder.0)?;
        }
    }
    // A decoder may report EOS after consuming only part of the last frame.
    // Count bytes through the demuxer and require EOF at a complete frame boundary.
    let mut extra = [0];
    if complete_end != state.bytes || state.input.read(&mut extra)? != 0 {
        return Err(corrupt("truncated audio frame or trailing data"));
    }
    if state.info.frames != 0 && state.info.frames != state.frames {
        return Err(corrupt("decoded sample count differs from STREAMINFO"));
    }
    if let Some(total) = state.input.get_ref().1.ogg_samples()
        && total != state.frames
    {
        return Err(corrupt("Ogg granule position differs from decoded length"));
    }
    if unsafe { FLAC__stream_decoder_finish(decoder.0) } == 0 {
        return Err(corrupt("STREAMINFO MD5 mismatch"));
    }
    state.info.frames = state.frames;
    Ok(state.info)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_flac_validate(
    path: *const u8,
    length: usize,
    info: *mut Info,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    wav::boundary(message, capacity, || {
        wav::valid(info, 1)?;
        unsafe { *info = Info::default() };
        let value = validate(unsafe { wav::path(path, length)? })?;
        unsafe { ptr::write(info, value) };
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cargo_tests_link_the_selected_native_decoder() {
        // Keep a native call reachable even when a platform's linker discards
        // every unused file-validation path from the other Rust tests.
        let pointer = unsafe { FLAC__stream_decoder_new() };
        assert!(!pointer.is_null());
        let decoder = Owner(pointer);
        assert_ne!(
            unsafe { FLAC__stream_decoder_set_md5_checking(decoder.0, 1) },
            0
        );
    }
}
