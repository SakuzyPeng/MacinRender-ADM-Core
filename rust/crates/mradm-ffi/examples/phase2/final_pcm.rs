//! Final PCM (PCM consistency 3.6): the seconds-to-frame rule shared by the output trim, the
//! preview window and Monitor seek, and integer WAVE quantization through the production writer.
use super::{doubles, integers};
use mradm_dsp::{decimal::parse_f64, timeline::seconds_to_frame};
use mradm_wav::{Format, SampleFormat, Writer, WriterOptions};
use std::{io::Cursor, path::Path};

pub(super) fn run(out: &Path) {
    // Decimal texts as the command line receives them, including products that land exactly on,
    // or one ulp beside, half a frame.
    let texts = [
        "0",
        "0.000010416666666666667",
        "0.1",
        "0.2",
        "0.25001041666666666",
        "0.30001041666666664",
        "0.65",
        "0.7000104166666667",
        "0.99999",
        "1",
        "1.5",
        "59.99999",
        "3599.999995",
        "86400.123456789",
    ];
    let mut frames = Vec::new();
    for rate in [22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000] {
        for text in texts {
            let frame = seconds_to_frame(parse_f64(text).unwrap(), rate).unwrap();
            assert!(frame < 1 << 53);
            frames.push(frame as f64);
        }
    }
    doubles(out, "timeline.10-seconds-to-frame.f64", &frames);

    let mut samples = vec![
        0.0f32,
        -0.0,
        0.5,
        -0.5,
        1.0,
        -1.0,
        1.5,
        -1.5,
        f32::INFINITY,
        f32::NEG_INFINITY,
        f32::MAX,
        f32::MIN,
        f32::MIN_POSITIVE,
        -f32::MIN_POSITIVE,
        f32::from_bits(1),
        -f32::from_bits(1),
        f32::from_bits(1.0f32.to_bits() - 1),
        -f32::from_bits(1.0f32.to_bits() - 1),
    ];
    // Around each integer step of every depth: the step, the quarter and half points, and the
    // neighbouring binary32 values, both signs.
    for scale in [32767.0f64, 8388607.0, 2147483647.0] {
        for step in [1.0f64, 2.0, 1000.0, scale - 1.0] {
            for fraction in [0.0, 0.25, 0.5, 0.75] {
                let value = ((step + fraction) / scale) as f32;
                for v in [
                    value,
                    f32::from_bits(value.to_bits() - 1),
                    f32::from_bits(value.to_bits() + 1),
                ] {
                    samples.extend([v, -v]);
                }
            }
        }
    }
    if samples.len() % 2 == 1 {
        samples.push(0.0);
    }
    for (name, format, width) in [
        ("wav-quantize.10-pcm16.i32", SampleFormat::Pcm16, 2usize),
        ("wav-quantize.20-pcm24.i32", SampleFormat::Pcm24, 3),
        ("wav-quantize.30-pcm32.i32", SampleFormat::Pcm32, 4),
    ] {
        let format = Format {
            channels: 1,
            sample_rate: 48000,
            sample_format: format,
            channel_mask: None,
        };
        let mut writer =
            Writer::new(Cursor::new(Vec::new()), format, WriterOptions::default()).unwrap();
        assert_eq!(writer.write_frames(&samples).unwrap(), samples.len() as u64);
        let bytes = writer.finish().unwrap().into_inner();
        // An even sample count leaves no pad byte, so the data chunk ends the file.
        let data = &bytes[bytes.len() - samples.len() * width..];
        let values: Vec<i32> = data
            .chunks_exact(width)
            .map(|raw| {
                let mut wide = [0u8; 4];
                wide[4 - width..].copy_from_slice(raw);
                i32::from_le_bytes(wide) >> (8 * (4 - width))
            })
            .collect();
        integers(out, name, &values);
    }
}
