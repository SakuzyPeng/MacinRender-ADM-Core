//! The worker owns the WAVE reader and reports completed blocks and the start of the encoder flush.
use super::encoder::Writer;
use mradm_engine::request::Error;
use mradm_wav::Reader;
use objc2_core_audio_types as ca;
use std::fs::File;
use std::io::{BufReader, Write};

/// For wav71 the encoder takes AudioUnit_7_1, with the sides before the rears.
fn layout_tag(layout: &[u8]) -> Result<u32, Error> {
    let tag = match layout {
        b"binaural" => ca::kAudioChannelLayoutTag_Binaural,
        b"0+2+0" => ca::kAudioChannelLayoutTag_Stereo,
        b"wav71" => ca::kAudioChannelLayoutTag_AudioUnit_7_1,
        b"hoa3" => ca::kAudioChannelLayoutTag_HOA_ACN_SN3D | 16,
        b"4+5+0" | b"5.1.4" | b"atmos514" => ca::kAudioChannelLayoutTag_Atmos_5_1_4,
        b"4+7+0" | b"7.1.4" | b"atmos714" => ca::kAudioChannelLayoutTag_Atmos_7_1_4,
        b"9.1.6" | b"atmos916" => ca::kAudioChannelLayoutTag_Atmos_9_1_6,
        b"9+10+3" | b"22.2" | b"cicp13" => ca::kAudioChannelLayoutTag_CICP_13,
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
    Ok(tag)
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
    let mut writer = Writer::open(output, tag, bitrate, drc_music, caf)?;
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
        writer.write(block)?;
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
