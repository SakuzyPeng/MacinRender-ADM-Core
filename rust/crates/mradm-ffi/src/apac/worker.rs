//! The worker owns the WAVE reader and reports completed blocks and the start of the encoder flush.
use crate::adm::Text;
use mradm_engine::request::Error;
use mradm_wav::Reader;
use std::ffi::{CStr, c_char, c_void};
use std::fs::File;
use std::io::{BufReader, Write};
use std::ptr::NonNull;

/// The Apple island's AudioToolbox operations (apac_encoder.h).
#[repr(C)]
pub struct ApacEncoder {
    open: unsafe extern "C" fn(Text, u32, u32, u32, u32, *mut c_char, usize) -> *mut c_void,
    write: unsafe extern "C" fn(*mut c_void, *mut f32, u32, u32) -> i32,
    finish: unsafe extern "C" fn(*mut c_void) -> i32,
}

unsafe extern "C" {
    fn mradm_apple_apac_encoder() -> *const ApacEncoder;
}

struct Writer {
    api: &'static ApacEncoder,
    file: NonNull<c_void>,
}

impl Writer {
    fn open(
        output: &[u8],
        tag: u32,
        bitrate: u32,
        drc_music: bool,
        caf: bool,
    ) -> Result<Self, Error> {
        let api = unsafe { &*mradm_apple_apac_encoder() };
        let mut message = [0; 128];
        let raw = unsafe {
            (api.open)(
                Text::bytes(output),
                tag,
                bitrate,
                drc_music.into(),
                caf.into(),
                message.as_mut_ptr(),
                message.len(),
            )
        };
        let file = NonNull::new(raw).ok_or_else(|| {
            let message = unsafe { CStr::from_ptr(message.as_ptr()) };
            Error::new(3, message.to_string_lossy(), "")
        })?;
        Ok(Self { api, file })
    }

    fn write(&self, pcm: &mut [f32], channels: u32) -> Result<(), Error> {
        let frames = (pcm.len() / channels as usize) as u32;
        let status =
            unsafe { (self.api.write)(self.file.as_ptr(), pcm.as_mut_ptr(), channels, frames) };
        check(status, "ExtAudioFileWrite")
    }

    fn finish(self) -> Result<(), Error> {
        let writer = std::mem::ManuallyDrop::new(self);
        check(
            unsafe { (writer.api.finish)(writer.file.as_ptr()) },
            "ExtAudioFileDispose",
        )
    }
}

impl Drop for Writer {
    fn drop(&mut self) {
        unsafe { (self.api.finish)(self.file.as_ptr()) };
    }
}

fn check(status: i32, operation: &str) -> Result<(), Error> {
    if status == 0 {
        Ok(())
    } else {
        Err(Error::new(
            3,
            format!("APAC: {operation} failed ({status})"),
            "",
        ))
    }
}

/// AudioChannelLayoutTag is the layout identifier in the high half and the channel count in the low half.
/// For wav71 the encoder takes AudioUnit_7_1, with the sides before the rears.
fn layout_tag(layout: &[u8]) -> Result<u32, Error> {
    let (id, channels) = match layout {
        b"binaural" => (106, 2),
        b"0+2+0" => (101, 2),
        b"wav71" => (128, 8),
        b"hoa3" => (190, 16),
        b"4+5+0" | b"5.1.4" | b"atmos514" => (195, 10),
        b"4+7+0" | b"7.1.4" | b"atmos714" => (192, 12),
        b"9.1.6" | b"atmos916" => (193, 16),
        b"9+10+3" | b"22.2" | b"cicp13" => (204, 24),
        _ => {
            return Err(Error::new(
                2,
                format!(
                    "APAC: unsupported layout '{}'",
                    String::from_utf8_lossy(layout)
                ),
                "",
            ));
        }
    };
    Ok((id << 16) | channels)
}

fn progress(out: &mut dyn Write, current: u64, total: u64) {
    let _ = writeln!(out, "P {current} {total}");
    let _ = out.flush();
}

pub(super) fn run(
    input: &[u8],
    output: &[u8],
    layout: &[u8],
    bitrate: u32,
    drc_music: bool,
    caf: bool,
    out: &mut dyn Write,
) -> Result<(), Error> {
    let tag = layout_tag(layout)?;
    // The WAVE boundary accepts nonempty UTF-8; AudioToolbox accepts the destination's filesystem bytes.
    let src = unsafe { crate::wav::path(input.as_ptr(), input.len()) }
        .map_err(|e| Error::new(e.code, e.message, ""))?;
    let mut reader = File::open(src)
        .map_err(mradm_wav::Error::from)
        .and_then(|file| Reader::new(BufReader::new(file)))
        .map_err(|e| Error::new(e.code, e.message, ""))?;
    let info = reader.info();
    let channels = u32::from(info.format.channels);
    let expected = tag & 0xffff;
    if channels != expected {
        return Err(Error::new(
            1,
            format!(
                "APAC layout '{}' requires {expected} channels, got {channels}",
                String::from_utf8_lossy(layout)
            ),
            "",
        ));
    }
    if info.format.sample_rate != 48_000 {
        return Err(Error::new(
            1,
            format!(
                "APAC requires 48000 Hz input, got {} Hz",
                info.format.sample_rate
            ),
            "",
        ));
    }
    let bitrate = if bitrate == 0 && expected >= 10 {
        (2048 * channels + 6) / 12
    } else {
        bitrate
    };
    let writer = Writer::open(output, tag, bitrate, drc_music, caf)?;
    let total = info.frames;
    let mut done = 0;
    let mut pcm = vec![0.; 4096 * channels as usize];
    progress(out, done, total);
    while done < total {
        let frames = (total - done).min(4096) as usize;
        let read = reader
            .read_frames(&mut pcm[..frames * channels as usize])
            .unwrap_or(0);
        if read == 0 {
            return Err(Error::new(3, "short read in convert_to_apac", ""));
        }
        let block = &mut pcm[..read as usize * channels as usize];
        if layout == b"wav71" {
            for frame in block.as_chunks_mut::<8>().0 {
                frame.swap(4, 6);
                frame.swap(5, 7);
            }
        }
        writer.write(block, channels)?;
        done += read;
        progress(out, done, total);
    }
    let _ = writeln!(out, "F");
    let _ = out.flush();
    if std::env::var_os("MRADM_APAC_TEST_FORCE_STALL").is_some() {
        // Stand in for a flush that never returns; the parent kills the process group.
        loop {
            std::thread::park();
        }
    }
    writer.finish()?;
    progress(out, total, total);
    Ok(())
}
