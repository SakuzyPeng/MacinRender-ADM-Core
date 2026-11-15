//! Plain channel-bed inputs rendered through the service (formerly the C++
//! `channel_bed_render_test`, R3 C3): WAVE masks of the speaker outputs, a custom-labelled bed to
//! binaural with ADM Binaural semantics and no speaker mask, the 7.1.4 WAVE-order rewrite keeping
//! each speaker's signal, integer outputs above 8 channels as BW64 with DirectSpeakers ADM, and HOA3
//! with its ACN / SN3D pack, each re-imported.
mod common;
use common::render::*;
use mradm_engine::render::Request;
use mradm_engine::request::{BitDepth, Selection};
use mradm_json::Value;
use mradm_wav::{Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::{Path, PathBuf};

const FRAMES: usize = 4096;

/// A plain float32 WAVE of `channels` 80 Hz tones, channel `c` at amplitude 0.01 (c + 1).
fn bed(dir: &Dir, channels: u16) -> PathBuf {
    let path = dir.file(&format!("bed-{channels}.wav"));
    let format = Format {
        channels,
        sample_rate: 48000,
        sample_format: SampleFormat::Float32,
        channel_mask: None,
    };
    let mut writer = Writer::new(
        File::create(&path).unwrap(),
        format,
        WriterOptions::default(),
    )
    .unwrap();
    let samples: Vec<f32> = (0..FRAMES)
        .flat_map(|f| {
            let phase = f as f32 * 80. * std::f32::consts::TAU / 48000.;
            (0..channels).map(move |c| 0.01 * f32::from(c + 1) * phase.sin())
        })
        .collect();
    writer.write_frames(&samples).unwrap();
    writer.finish().unwrap();
    path
}

fn bed_request(
    input: &Path,
    output: &Path,
    renderer: Selection,
    layout: &str,
    input_layout: &str,
) -> Request {
    let mut r = request(input, output, renderer, layout);
    r.peak_limit = false;
    r.input_layout = Some(input_layout.to_owned());
    r
}

fn mask(path: &Path) -> Option<u32> {
    mradm_wav::Reader::new(std::io::BufReader::new(File::open(path).unwrap()))
        .unwrap()
        .info()
        .format
        .channel_mask
}

fn scene(path: &Path) -> Value {
    let scene = mradm_scene::import(path.to_str().unwrap().as_bytes(), Default::default()).unwrap();
    mradm_json::parse(mradm_engine::scene_json::inspect_json(&scene, "<input>").as_bytes()).unwrap()
}

fn at<'a>(v: &'a Value, path: &str) -> &'a Value {
    path.split('.').fold(v, |v, k| match k.parse::<usize>() {
        Ok(i) => &v.as_array().unwrap()[i],
        Err(_) => v.as_object().unwrap().get(k).unwrap(),
    })
}

fn rms(audio: &Audio) -> Vec<f64> {
    (0..audio.channels)
        .map(|c| {
            ((0..audio.frames)
                .map(|f| f64::from(audio.samples[f * audio.channels + c]).powi(2))
                .sum::<f64>()
                / audio.frames as f64)
                .sqrt()
        })
        .collect()
}

#[test]
fn speaker_outputs_carry_their_wave_masks() {
    let dir = scratch("bed-masks");
    let input = bed(&dir, 6);
    for (layout, channels, expected) in [
        ("5.1", 6, 0x3F),
        ("5.1.2", 8, 0x503F),
        ("7.1", 8, 0x063F),
        ("5.1.4", 10, 0x2D03F),
    ] {
        let out = dir.file(&format!("{layout}.wav"));
        let (_, audio) = render(bed_request(&input, &out, Selection::Ear, layout, "5.1"));
        assert_eq!(
            (audio.channels, audio.rate, audio.frames),
            (channels, 48000, FRAMES),
            "{layout}"
        );
        assert!(
            audio.samples.iter().all(|v| v.is_finite()) && audio.energy() > 0.01,
            "{layout}"
        );
        assert_eq!(mask(&out), Some(expected), "{layout}");
    }
}

#[test]
fn a_custom_bed_renders_to_binaural_with_adm_binaural_semantics() {
    let dir = scratch("bed-binaural");
    let input = bed(&dir, 6);
    let out = dir.file("binaural.wav");
    let mut r = request(&input, &out, Selection::SafBinaural, "binaural");
    r.peak_limit = false;
    r.input_channel_labels = ["R", "L", "C", "LFE", "M-110", "M+110"]
        .iter()
        .map(|l| (*l).to_owned())
        .collect();
    let (_, audio) = render(r);
    assert_eq!(audio.channels, 2);
    assert!(audio.energy() > 0.01);
    assert_eq!(
        mask(&out).unwrap_or(0),
        0,
        "a binaural WAVE claims no speaker positions"
    );
    let mut reader =
        mradm_wav::ChunkReader::new(std::io::BufReader::new(File::open(&out).unwrap())).unwrap();
    let chunk = reader
        .chunks()
        .iter()
        .find(|c| c.id == *b"axml")
        .copied()
        .unwrap();
    let mut axml = vec![0u8; chunk.size as usize];
    reader.read_chunk(*b"axml", &mut axml).unwrap();
    let axml = String::from_utf8(axml).unwrap();
    assert!(
        axml.contains(r#"typeDefinition="Binaural""#)
            && axml.contains("leftEar")
            && axml.contains("rightEar")
    );
    let j = scene(&out);
    assert_eq!(at(&j, "objects").as_array().unwrap().len(), 1);
    assert_eq!(at(&j, "objects.0.tracks").as_array().unwrap().len(), 2);
    assert_eq!(
        (
            at(&j, "objects.0.tracks.0.channel_index").as_f64(),
            at(&j, "objects.0.tracks.1.channel_index").as_f64()
        ),
        (Some(0.), Some(1.))
    );
}

#[test]
fn the_7_1_4_rewrite_keeps_each_speaker_signal() {
    let dir = scratch("bed-714");
    let input = bed(&dir, 12);
    let out = dir.file("714.wav");
    let mut r = bed_request(&input, &out, Selection::Ear, "7.1.4", "7.1.4");
    r.output_bit_depth = BitDepth::I24 as u32;
    let (_, audio) = render(r);
    assert_eq!(mask(&out), Some(0x2D63F));
    let source = rms(&Audio::read(&input));
    let rendered = rms(&audio);
    let scale = rendered[0] / source[0];
    for (c, (s, r)) in source.iter().zip(&rendered).enumerate() {
        let expected = s * scale;
        assert!(
            (expected - r).abs() < (expected * 0.02).max(2e-4),
            "file channel {}",
            c + 1
        );
    }
}

#[test]
fn integer_outputs_above_8_channels_are_bw64_with_direct_speakers_adm() {
    let dir = scratch("bed-bw64");
    let input = bed(&dir, 6);
    for (layout, channels, first, last) in [
        ("9.1.4", 14, Some("M+030"), "U-150"),
        ("9.1.6", 16, None, "U-150"),
        ("22.2", 24, None, "B-045"),
    ] {
        let out = dir.file(&format!("{layout}.wav"));
        let mut r = bed_request(&input, &out, Selection::Ear, layout, "5.1");
        r.output_bit_depth = BitDepth::I24 as u32;
        assert!(run(r).ok(), "{layout}");
        assert_eq!(&std::fs::read(&out).unwrap()[..4], b"BW64", "{layout}");
        let j = scene(&out);
        assert_eq!(
            at(&j, "file.num_channels").as_f64(),
            Some(channels as f64),
            "{layout}"
        );
        let tracks = at(&j, "objects.0.tracks").as_array().unwrap();
        assert_eq!(
            (at(&j, "objects").as_array().unwrap().len(), tracks.len()),
            (1, channels),
            "{layout}"
        );
        let label = |t: &Value| {
            at(t, "ds_blocks.0.speaker_labels.0")
                .as_str()
                .unwrap()
                .to_owned()
        };
        if let Some(first) = first {
            assert_eq!(label(&tracks[0]), first, "{layout}");
        }
        assert_eq!(label(tracks.last().unwrap()), last, "{layout}");
    }
}

#[test]
fn hoa3_outputs_carry_their_acn_sn3d_pack() {
    let dir = scratch("bed-hoa");
    let input = bed(&dir, 6);
    let out = dir.file("hoa3.wav");
    assert!(run(bed_request(&input, &out, Selection::Hoa, "hoa3", "5.1")).ok());
    let j = scene(&out);
    assert_eq!(at(&j, "file.num_channels").as_f64(), Some(16.));
    assert_eq!(at(&j, "hoa_tracks").as_array().unwrap().len(), 1);
    assert_eq!(at(&j, "hoa_tracks.0.normalization").as_str(), Some("SN3D"));
    assert_eq!(
        at(&j, "hoa_tracks.0.channels").as_array().unwrap().len(),
        16
    );
    assert_eq!(
        (
            at(&j, "hoa_tracks.0.channels.1.order").as_f64(),
            at(&j, "hoa_tracks.0.channels.1.degree").as_f64()
        ),
        (Some(1.), Some(-1.))
    );
}
