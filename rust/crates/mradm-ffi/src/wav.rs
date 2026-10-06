//! Private file I/O boundary. Owners and their allocations never cross allocators.
use mradm_wav::{Container, Error, Format, Reader, Result, SampleFormat, Writer, WriterOptions};
use std::{
    fs::{File, OpenOptions},
    io::BufReader,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr, slice, str,
};

pub struct ReadHandle {
    reader: Reader<BufReader<File>>,
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

#[cfg(test)]
mod tests {
    use super::*;
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
}
