//! Replays `catalog-v1`, the discovery documents and scene inspect JSON the C++ engine produced
//! before PCM consistency 5.2 A moved them to Rust (see `catalog-v1/README.md`).
use mradm_engine::{catalog::Platform, scene_json, tables};
use mradm_json::Value;

mod common;
use common::{adm_wav, bed_wav, field};
use mradm_wav::SampleFormat;
use std::path::{Path, PathBuf};

fn dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/catalog-v1")
}

fn scene_dir(set: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../mradm-scene/tests")
        .join(set)
        .join("scenes")
}

const ADM_SCENES: [(&str, &str); 18] = [
    ("policy-v1", "directspeakers"),
    ("policy-v1", "directspeakers-lfe"),
    ("policy-v1", "hoa"),
    ("policy-v1", "hoa-pack-attributes"),
    ("policy-v1", "hoa3"),
    ("policy-v1", "objects-cartesian"),
    ("policy-v1", "objects-extent"),
    ("policy-v1", "objects-extent-multi"),
    ("policy-v1", "objects-point"),
    ("policy-v1", "objects-room"),
    ("policy-v1", "objects-stream-only"),
    ("policy-v1", "params"),
    ("policy-v1", "policy-objects"),
    ("policy-v1", "types-from-ids"),
    ("export-v1", "common-bed"),
    ("export-v1", "shared-block"),
    ("policy-v1", "objects-point"),
    ("policy-v1", "hoa3"),
];

/// Name, channels, rate, sample format, WAVE mask and frames of an extra channel-bed input.
type Bed = (&'static str, u16, u32, SampleFormat, Option<u32>, usize);

/// Every inspect input, written into `out`; returns the names in replay order.
pub fn write_inputs(out: &Path) -> Vec<String> {
    let mut names = Vec::new();
    for (i, (set, name)) in ADM_SCENES.iter().enumerate() {
        // The last two repeat a scene as float32 and PCM16 files.
        let (label, format) = match i {
            16 => (format!("{name}-f32"), SampleFormat::Float32),
            17 => (format!("{name}-pcm16"), SampleFormat::Pcm16),
            _ => (name.to_string(), SampleFormat::Pcm24),
        };
        adm_wav(
            &scene_dir(set),
            name,
            &out.join(format!("{label}.wav")),
            format,
        );
        names.push(label);
    }
    for layout in mradm_scene::input::input_layouts() {
        if layout.wave_channel_mask == 0 {
            continue;
        }
        let label = format!("bed-{}", layout.id);
        let channels = layout.wave_mask_channels.len() as u16;
        bed_wav(
            &out.join(format!("{label}.wav")),
            channels,
            48000,
            SampleFormat::Pcm16,
            Some(layout.wave_channel_mask),
            480,
        );
        names.push(label);
    }
    let extra: [Bed; 5] = [
        (
            "bed-71-float-44k",
            8,
            44100,
            SampleFormat::Float32,
            Some(0x063F),
            441,
        ),
        (
            "bed-51-empty",
            6,
            48000,
            SampleFormat::Pcm24,
            Some(0x003F),
            0,
        ),
        ("bed-2ch-plain", 2, 48000, SampleFormat::Pcm16, None, 480),
        ("bed-6ch-mask0", 6, 48000, SampleFormat::Pcm16, Some(0), 480),
        ("bed-3ch-plain", 3, 48000, SampleFormat::Pcm16, None, 480),
    ];
    for (label, channels, rate, format, mask, frames) in extra {
        bed_wav(
            &out.join(format!("{label}.wav")),
            channels,
            rate,
            format,
            mask,
            frames,
        );
        names.push(label.to_owned());
    }
    std::fs::write(out.join("not-wave.wav"), b"not a wave file").unwrap();
    names.push("not-wave".to_owned());
    names.push("missing".to_owned());
    names
}

fn linux() -> Platform {
    Platform {
        sofa: true,
        ..Platform::default()
    }
}

fn golden(name: &str) -> String {
    let text = std::fs::read_to_string(dir().join(name)).unwrap();
    text.strip_suffix('\n').unwrap_or(&text).to_owned()
}

#[test]
fn discovery_documents_match_the_cpp_engine() {
    let platform = linux();
    assert_eq!(
        tables::capabilities_json(&platform),
        golden("capabilities.json")
    );
    assert_eq!(tables::layouts_json(&platform), golden("layouts.json"));
    assert_eq!(
        tables::output_formats_json(&platform),
        golden("output-formats.json")
    );
    assert_eq!(tables::input_layouts_json(), golden("input-layouts.json"));
    assert_eq!(
        tables::support_matrix_json(&platform),
        golden("support-matrix.json")
    );
}

#[test]
fn scene_inspect_json_matches_the_cpp_engine() {
    let out = std::env::temp_dir().join(format!("mradm-engine-catalog-{}", std::process::id()));
    std::fs::create_dir_all(&out).unwrap();
    let names = write_inputs(&out);
    let expected = std::fs::read_to_string(dir().join("scenes.jsonl")).unwrap();
    let records: Vec<Value> = expected
        .lines()
        .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
        .collect();
    assert_eq!(records.len(), names.len());
    let mut compared = 0;
    for (name, record) in names.iter().zip(&records) {
        assert_eq!(field(record, "name").as_str().unwrap(), name);
        let path = out.join(format!("{name}.wav"));
        let imported = mradm_scene::import(path.to_str().unwrap().as_bytes(), Default::default());
        let code = field(record, "code").as_f64().unwrap() as i32;
        match imported {
            Ok(scene) => {
                assert_eq!(code, 0, "{name}: C++ rejected the input");
                let json = scene_json::inspect_json(&scene, "<input>");
                assert_eq!(json, field(record, "json").as_str().unwrap(), "{name}");
                compared += 1;
            }
            Err(error) => assert_eq!(error.code, code, "{name}"),
        }
    }
    assert_eq!(compared, 24, "only {compared} scenes compared");
    std::fs::remove_dir_all(&out).unwrap();
}

fn apple() -> mradm_engine::catalog::Backend {
    use mradm_engine::catalog::{Backend, Layout};
    let layout = |id: &str, channels: u16, binaural: bool| Layout {
        id: id.into(),
        display_name: format!("Apple {id}"),
        channel_count: channels,
        is_3d: !binaural,
        lfe_count: u16::from(!binaural),
        supports_spread: false,
        is_binaural: binaural,
    };
    Backend {
        renderer: "apple".into(),
        name: "apple-spatial-mixer".into(),
        version: "1".into(),
        layouts: vec![layout("binaural", 2, true), layout("4+7+0", 12, false)],
        supports_objects: true,
        hrtf_sources: vec!["system".into()],
        ..Backend::default()
    }
}

#[test]
fn island_facts_reach_the_documents() {
    use mradm_engine::catalog::SpatialLayout;
    let platform = Platform {
        sofa: false,
        apple: Some(apple()),
        system_spatial_layouts: vec![SpatialLayout {
            id: "4+7+0".into(),
            display_name: "7.1.4".into(),
            channel_count: 12,
        }],
        apple_system_spatial_lfe_routing_ok: Some(false),
        ..Platform::default()
    };
    let caps = mradm_json::parse(tables::capabilities_json(&platform).as_bytes()).unwrap();
    let backends = field(&caps, "backends").as_array().unwrap();
    assert_eq!(field(&backends[6], "renderer").as_str(), Some("apple"));
    let binaural = field(&backends[4], "hrtf_sources").as_array().unwrap();
    assert_eq!(binaural.len(), 1, "no user SOFA without the build switch");
    assert_eq!(
        field(&caps, "apple_system_spatial_lfe_routing_ok").as_bool(),
        Some(false)
    );
    let spatial = field(&caps, "system_spatial_layouts").as_array().unwrap();
    assert_eq!(field(&spatial[0], "channel_count").as_f64(), Some(12.0));
    assert!(!tables::capabilities_json(&linux()).contains("apple_system_spatial_lfe_routing_ok"));

    let rows = tables::output_layouts(&platform);
    let row = rows
        .iter()
        .find(|r| r.format == "wav" && r.layout == "7.1.4")
        .unwrap();
    assert_eq!(row.supported_by.last().map(String::as_str), Some("apple"));
    let row = rows
        .iter()
        .find(|r| r.format == "wav" && r.layout == "binaural")
        .unwrap();
    assert!(row.supported_by.iter().any(|r| r == "apple"));
    let matrix = tables::support_matrix_json(&platform);
    assert!(matrix.contains("\"renderer\": \"apple\""));
}

#[test]
fn encoder_availability_follows_the_platform() {
    let reason = |platform: &Platform, name: &str| {
        let formats = tables::output_formats(platform);
        let f = formats.iter().find(|f| f.format == name).unwrap().clone();
        (f.available, f.available_reason)
    };
    let mut platform = Platform {
        iamf: false,
        iamf_mp4_packager: true,
        ..Platform::default()
    };
    assert_eq!(
        reason(&platform, "iamf_mp4"),
        (
            false,
            "requires MR_ADM_ENABLE_IAMF=ON and the AOM iamf-tools bridge"
        )
    );
    let doc = tables::output_formats_json(&platform);
    assert!(
        doc.contains("\"iamf_mp4_packager\": false"),
        "the packager counts only with IAMF"
    );
    platform.iamf = true;
    platform.iamf_mp4_packager = false;
    assert_eq!(reason(&platform, "iamf"), (true, ""));
    assert_eq!(
        reason(&platform, "iamf_mp4"),
        (false, "requires mp4box (GPAC) or ffmpeg in PATH")
    );
    platform.iamf_mp4_packager = true;
    platform.apac = true;
    assert_eq!(reason(&platform, "iamf_mp4"), (true, ""));
    assert_eq!(reason(&platform, "apac"), (true, ""));
    let doc = tables::output_formats_json(&platform);
    assert!(!doc.contains("available_reason"));
    let matrix = tables::support_matrix_json(&platform);
    assert!(!matrix.contains("is unavailable") && !matrix.contains("macOS only"));
}

#[test]
fn inspect_text_matches_the_cpp_cli() {
    let out = std::env::temp_dir().join(format!("mradm-engine-inspect-{}", std::process::id()));
    std::fs::create_dir_all(&out).unwrap();
    let mut names = write_inputs(&out);
    let inspect = dir().join("../inspect-v1");
    adm_wav(&inspect, "rich", &out.join("rich.wav"), SampleFormat::Pcm24);
    names.push("rich".to_owned());
    let expected = std::fs::read_to_string(inspect.join("scenes.jsonl")).unwrap();
    let records: Vec<Value> = expected
        .lines()
        .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
        .collect();
    assert_eq!(records.len(), names.len());
    let mut compared = 0;
    for (name, record) in names.iter().zip(&records) {
        assert_eq!(field(record, "name").as_str().unwrap(), name);
        let path = out.join(format!("{name}.wav"));
        let imported = mradm_scene::import(path.to_str().unwrap().as_bytes(), Default::default());
        match (imported, field(record, "text").as_str()) {
            (Ok(scene), Some(text)) => {
                assert_eq!(scene_json::inspect_text(&scene), text, "{name}");
                compared += 1;
            }
            (Err(_), None) => {}
            (imported, _) => panic!("{name}: Rust import ok = {}", imported.is_ok()),
        }
    }
    assert_eq!(compared, 25, "only {compared} scenes compared");
    std::fs::remove_dir_all(&out).unwrap();
}
