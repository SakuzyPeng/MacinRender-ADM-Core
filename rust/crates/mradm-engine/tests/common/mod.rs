//! Test inputs shared by the engine tests: ADM BW64 scenes and plain WAVE channel beds.
#![allow(dead_code)]
pub mod backend;
pub mod monitor;
use mradm_json::Value;
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::Path;

pub fn field<'a>(value: &'a Value, key: &str) -> &'a Value {
    value.as_object().unwrap().get(key).unwrap()
}

pub fn fixed<const N: usize>(text: &str) -> [u8; N] {
    let mut out = [b' '; N];
    out[..text.len()].copy_from_slice(text.as_bytes());
    out
}

/// A 480-frame PCM24 BW64 file with the scene's AXML and CHNA from `<dir>/<name>.xml` and `.json` (as `mradm-scene`'s tests write it).
pub fn adm_wav(dir: &Path, name: &str, path: &Path, sample_format: SampleFormat) {
    adm_wav_with(dir, name, path, sample_format, None, 480);
}

/// [`adm_wav`] with `frames` silent frames, at `rate` instead of the scene's own rate if given.
pub fn adm_wav_with(
    dir: &Path,
    name: &str,
    path: &Path,
    sample_format: SampleFormat,
    rate: Option<u32>,
    frames: usize,
) {
    let meta =
        mradm_json::parse(&std::fs::read(dir.join(format!("{name}.json"))).unwrap()).unwrap();
    let channels = field(&meta, "channels").as_f64().unwrap() as u16;
    let entries = field(&meta, "chna")
        .as_array()
        .unwrap()
        .iter()
        .map(|e| {
            let e = e.as_array().unwrap();
            ChnaEntry {
                track_index: e[0].as_f64().unwrap() as u16,
                uid: fixed::<12>(e[1].as_str().unwrap()),
                track_format: fixed::<14>(e[2].as_str().unwrap()),
                pack_format: fixed::<11>(e[3].as_str().unwrap()),
                pad: 0,
            }
        })
        .collect::<Vec<_>>();
    let format = Format {
        channels,
        sample_rate: rate.unwrap_or(field(&meta, "sample_rate").as_f64().unwrap() as u32),
        sample_format,
        channel_mask: None,
    };
    let options = WriterOptions {
        container: None,
        axml: Some(std::fs::read(dir.join(format!("{name}.xml"))).unwrap()),
        chna: Some(Chna {
            tracks: entries.len() as u16,
            entries,
        }),
    };
    let mut writer = Writer::new(File::create(path).unwrap(), format, options).unwrap();
    writer
        .write_frames(&vec![0.0; usize::from(channels) * frames])
        .unwrap();
    writer.finish().unwrap();
}

/// A plain WAVE channel bed without ADM chunks.
pub fn bed_wav(
    path: &Path,
    channels: u16,
    rate: u32,
    sample_format: SampleFormat,
    mask: Option<u32>,
    frames: usize,
) {
    let format = Format {
        channels,
        sample_rate: rate,
        sample_format,
        channel_mask: mask,
    };
    let options = WriterOptions {
        container: None,
        axml: None,
        chna: None,
    };
    let mut writer = Writer::new(File::create(path).unwrap(), format, options).unwrap();
    writer
        .write_frames(&vec![0.0; usize::from(channels) * frames])
        .unwrap();
    writer.finish().unwrap();
}
