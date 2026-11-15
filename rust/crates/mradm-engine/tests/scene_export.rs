//! Export writeback as whole files (formerly the C++ `scene_export_fixture_test`, R3 C3): the
//! input's AXML is rewritten with the semantic policy, as the C ABI's export does it
//! (`patched_axml`, then `replace_axml`), and the output re-imported. A plain export keeps every
//! block field, a gain scale halves the object gain only, an interpolation cap shortens the
//! interpolation only, a position override is deferred, a head-lock override is written on every
//! block of every type while the object keeps its value, the data chunk stays bit-exact and a BW64
//! source stays BW64. The AXML each policy produces is pinned by `mradm-scene`'s `export-v1`
//! goldens.
mod common;
use common::render::*;
use mradm_engine::scene_json;
use mradm_json::Value;
use mradm_testkit::adm::*;
use mradm_wav::{Chna, ChnaEntry, Container, Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::{Path, PathBuf};

/// The C++ source block: polar 30 / 10, gain 0.8, diffuse 0.3, width 20, a 1 s interpolation.
fn source_block() -> Properties {
    polar(
        30.,
        10.,
        [
            gain_linear(0.8),
            diffuse(0.3f32),
            width(20.0f32),
            jump_position([Param::text(false), interpolation_length(1.0)]),
        ],
    )
}

fn fixed<const N: usize>(text: &str) -> [u8; N] {
    let mut out = [b' '; N];
    out[..text.len()].copy_from_slice(text.as_bytes());
    out
}

/// A one-track source: a ramp of `frames` 24-bit samples with the AXML and one CHNA entry.
fn source(dir: &Dir, single: Single, container: Option<Container>) -> PathBuf {
    let path = dir.file("src.wav");
    let (axml, uid) = single.built();
    let format = Format {
        channels: 1,
        sample_rate: 48000,
        sample_format: SampleFormat::Pcm24,
        channel_mask: None,
    };
    let chna = Chna {
        tracks: 1,
        entries: vec![ChnaEntry {
            track_index: 1,
            uid: fixed(&uid),
            track_format: fixed(""),
            pack_format: fixed(""),
            pad: 0,
        }],
    };
    let options = WriterOptions {
        container,
        axml: Some(axml.into_bytes()),
        chna: Some(chna),
    };
    let mut writer = Writer::new(File::create(&path).unwrap(), format, options).unwrap();
    let ramp: Vec<f32> = (0..96).map(|i| i as f32 / 128.).collect();
    writer.write_frames(&ramp).unwrap();
    writer.finish().unwrap();
    path
}

fn axml_of(path: &Path) -> Vec<u8> {
    let mut reader =
        mradm_wav::ChunkReader::new(std::io::BufReader::new(File::open(path).unwrap())).unwrap();
    let chunk = reader
        .chunks()
        .iter()
        .find(|c| c.id == *b"axml")
        .copied()
        .unwrap();
    let mut axml = vec![0u8; chunk.size as usize];
    reader.read_chunk(*b"axml", &mut axml).unwrap();
    axml
}

/// The C ABI export: import, policy, rewritten AXML over the source's data.
fn export(src: &Path, dst: &Path, policy: &str) {
    let mut scene =
        mradm_scene::import(src.to_str().unwrap().as_bytes(), Default::default()).unwrap();
    if !policy.is_empty() {
        scene.apply_policy(mradm_scene::policy::parse(policy.as_bytes()).unwrap());
    }
    let rewritten = mradm_scene::writeback::patched_axml(&scene, &axml_of(src)).unwrap();
    mradm_render::postprocess::replace_axml(
        src.to_str().unwrap(),
        dst.to_str().unwrap(),
        rewritten.as_bytes(),
    )
    .unwrap_or_else(|f| panic!("{}", f.message));
}

fn json(path: &Path) -> Value {
    let scene = mradm_scene::import(path.to_str().unwrap().as_bytes(), Default::default()).unwrap();
    mradm_json::parse(scene_json::inspect_json(&scene, "<input>").as_bytes()).unwrap()
}

fn get<'a>(v: &'a Value, path: &str) -> &'a Value {
    path.split('.').fold(v, |v, k| match k.parse::<usize>() {
        Ok(i) => &v.as_array().unwrap()[i],
        Err(_) => v.as_object().unwrap().get(k).unwrap(),
    })
}

fn num(v: &Value, path: &str) -> f64 {
    get(v, path).as_f64().unwrap()
}

/// The data chunk's payload.
fn data(path: &Path) -> Vec<u8> {
    let bytes = std::fs::read(path).unwrap();
    let mut offset = 12;
    while offset + 8 <= bytes.len() {
        let id = &bytes[offset..offset + 4];
        let size = u32::from_le_bytes(bytes[offset + 4..offset + 8].try_into().unwrap()) as usize;
        if id == b"data" {
            let size = if size == u32::MAX as usize {
                bytes.len() - offset - 8
            } else {
                size
            };
            return bytes[offset + 8..offset + 8 + size].to_vec();
        }
        offset += 8 + size + (size & 1);
    }
    panic!("no data chunk");
}

const BLOCK: &str = "objects.0.tracks.0.object_blocks.0";

#[test]
fn exports_keep_what_the_policy_leaves_alone() {
    let dir = scratch("export");
    let src = source(&dir, Single::objects(vec![source_block()]), None);
    let before = json(&src);
    assert_eq!(
        num(&before, &format!("{BLOCK}.interp_length_samples")),
        48000.
    );

    let dst = dir.file("plain.wav");
    export(&src, &dst, "");
    let after = json(&dst);
    for key in [
        "position.azimuth",
        "position.elevation",
        "gain",
        "diffuse",
        "width",
    ] {
        let path = format!("{BLOCK}.{key}");
        assert!(
            (num(&after, &path) - num(&before, &path)).abs() < 0.01,
            "{key}"
        );
    }
    assert!(!data(&src).is_empty());
    assert_eq!(data(&src), data(&dst), "the data chunk is bit-exact");

    let dst = dir.file("gain.wav");
    export(
        &src,
        &dst,
        r#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":0.5}}}"#,
    );
    let j = json(&dst);
    assert!(
        (num(&j, "objects.0.gain") - 0.5).abs() < 0.001,
        "the object gain is halved"
    );
    assert!((num(&j, &format!("{BLOCK}.gain")) - 0.8).abs() < 0.001);
    assert!((num(&j, &format!("{BLOCK}.position.azimuth")) - 30.).abs() < 0.01);
    assert!((num(&j, &format!("{BLOCK}.diffuse")) - 0.3).abs() < 0.001);
    assert!((num(&j, &format!("{BLOCK}.width")) - 20.).abs() < 0.01);

    let dst = dir.file("interpolation.wav");
    export(
        &src,
        &dst,
        r#"{"schema":"mradm.semantic-policy.v1","global":{"interpolation":{"max_ms":250}}}"#,
    );
    let j = json(&dst);
    assert_eq!(num(&j, &format!("{BLOCK}.interp_length_samples")), 12000.);
    assert!((num(&j, &format!("{BLOCK}.gain")) - 0.8).abs() < 0.001);
    assert_eq!(data(&src), data(&dst));

    let dst = dir.file("position.wav");
    export(
        &src,
        &dst,
        r#"{"schema":"mradm.semantic-policy.v1","global":{"position":{"azimuth":-90.0,"elevation":0.0}}}"#,
    );
    let j = json(&dst);
    assert!(!matches!(
        get(&j, &format!("{BLOCK}.position.cartesian")),
        Value::Bool(true)
    ));
    assert!(
        (num(&j, &format!("{BLOCK}.position.azimuth")) - 30.).abs() < 0.01,
        "deferred"
    );
    assert!((num(&j, &format!("{BLOCK}.position.elevation")) - 10.).abs() < 0.01);
}

#[test]
fn a_head_lock_override_is_written_on_every_block() {
    for ty in [Type::Objects, Type::DirectSpeakers, Type::Hoa] {
        let dir = scratch("export-head");
        let block = match ty {
            Type::Objects => polar(0., 0., []),
            Type::DirectSpeakers => speaker_block("M+000", 0., 0.),
            _ => hoa_block([order(0), degree(0)]),
        };
        let mut single = Single::new(ty, vec![block]);
        single.doc.set(single.object, head_locked(true));
        let src = source(&dir, single, None);
        let dst = dir.file("out.wav");
        export(
            &src,
            &dst,
            r#"{"schema":"mradm.semantic-policy.v1","global":{"head_locked":false}}"#,
        );
        let j = json(&dst);
        let effective = match ty {
            Type::Objects => get(&j, BLOCK),
            Type::DirectSpeakers => get(&j, "objects.0.tracks.0.ds_blocks.0"),
            _ => get(&j, "hoa_tracks.0.channels.0.blocks.0"),
        };
        assert!(
            matches!(get(&j, "objects.0.head_locked"), Value::Bool(true)),
            "{ty:?}"
        );
        assert!(
            matches!(get(effective, "head_locked"), Value::Bool(false)),
            "{ty:?}"
        );
        let xml = String::from_utf8(axml_of(&dst)).unwrap();
        assert!(
            xml.contains("<headLocked>0</headLocked>"),
            "{ty:?}: false is authored explicitly"
        );
    }
}

#[test]
fn a_bw64_source_stays_bw64() {
    let dir = scratch("export-bw64");
    let src = source(
        &dir,
        Single::objects(vec![source_block()]),
        Some(Container::Bw64),
    );
    assert_eq!(&std::fs::read(&src).unwrap()[..4], b"BW64");
    let dst = dir.file("out.wav");
    export(&src, &dst, "");
    assert_eq!(&std::fs::read(&dst).unwrap()[..4], b"BW64");
    assert_eq!(data(&src), data(&dst));
    json(&dst);
}
