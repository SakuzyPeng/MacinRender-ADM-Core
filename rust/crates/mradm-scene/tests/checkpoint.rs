use mradm_adm::Snapshot;
use mradm_scene::{Info, RenderScene, SourceKind, checkpoint::encode};

/// The frozen version-1 stream the C++ encoder wrote for an empty 48 kHz, three-channel scene with
/// 0x100000003 frames (tests/fixtures/render-scene-v1/README.md).
#[test]
fn empty_scene_matches_the_frozen_cpp_encoding() {
    let scene = RenderScene {
        info: Info {
            sample_rate: 48000,
            channels: 3,
            frames: 0x1_0000_0003,
            source_kind: SourceKind::Adm,
            input_layout: String::new(),
            input_channel_labels: Vec::new(),
        },
        snapshot: Snapshot::default(),
    };
    let fixture = include_bytes!("../../../../tests/fixtures/render-scene-v1/empty.i32");
    let expected: Vec<i32> = fixture
        .chunks(4)
        .map(|w| i32::from_le_bytes(w.try_into().unwrap()))
        .collect();
    assert_eq!(encode(&scene), expected);
}
