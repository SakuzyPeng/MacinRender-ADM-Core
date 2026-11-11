//! The Scene stream's tail after the end marker (PCM consistency 5.4 E): only the frames the end
//! of input counts are rendered (binaural renders none), and tail slices continue the timeline
//! instead of reusing the key of the slice at sample 0. Only a diagnostic build writes
//! checkpoints (`--features mradm-dsp/diagnostics`); elsewhere this test is empty.
mod common;
use common::backend::{TestHost, repo};
use mradm_dsp::room::loading;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, RoomRequest, Screens, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::renderer;
use mradm_engine::scene::stream::{
    Config, Descriptor, FrameInput, PlaneView, Stream, Submit, frame_flag, pull_flag,
};
use mradm_render::live::{self, field};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

/// Renders 2048 frames, applies a gain policy at the end marker and drains; returns the trace.
fn replay(trace: &Path, selection: Selection, layout: &'static [u8]) -> PathBuf {
    let _ = std::fs::remove_dir_all(trace);
    // SAFETY: the only test of this binary sets it between its replays, before any stream exists.
    unsafe { std::env::set_var("MR_ADM_TRACE_DIR", trace) };
    let manifest = repo().join("tests/fixtures/room/synthetic/room.json");
    let package = (selection == Selection::RoomBinaural)
        .then(|| Arc::new(loading::load_package(manifest.to_str().unwrap().as_bytes()).unwrap()));
    let renderer = renderer::Config {
        selection,
        layout_id: layout,
        apple_geometry: false,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread: BinauralSpreadMode::Automatic,
        split_power: false,
        smoothing_frames: 0,
        sample_rate: 48000,
        sofa: None,
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry::default(),
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        room: RoomRequest {
            package,
            hrir: b"",
            metres: 0.,
            quality: 0,
        },
        sink: true,
    };
    let config = Config {
        output_sample_rate: 48000,
        output_ring_frames: 1 << 17,
        startup_watermark_frames: 1,
        ..Config::default()
    };
    let s = Stream::create(&config, &renderer, Arc::new(TestHost))
        .unwrap_or_else(|e| panic!("{}", String::from_utf8_lossy(&e.message)));
    s.begin_epoch(1, 0).unwrap();
    let object = Descriptor {
        element_id: 1,
        role: 0,
        speaker_label: Vec::new(),
        has_position: false,
        position: [0., 1., 0.],
        flags: 0,
        identity: None,
    };
    s.configure_generation(1, 1, &[object]).unwrap();
    let samples = vec![0.25f32; 2048];
    let planes = [PlaneView {
        element_id: 1,
        sample_count: 2048,
        stride: 1,
        has_signal: true,
        samples: Some(&samples),
    }];
    let initial = [live::Entry {
        element_id: 1,
        state: live::State {
            valid_fields: field::KNOWN,
            position: [0.25, 0.75, 0.],
            ..live::State::default()
        },
    }];
    let view = FrameInput {
        epoch: 1,
        generation: 1,
        start: 0,
        duration: 2048,
        flags: frame_flag::STATE_COMPLETE,
        planes: &planes[..],
        initial: &initial,
        updates: &[],
    };
    assert_eq!(s.submit_frame(&view, 5000).unwrap(), Submit::Accepted);
    s.wait_idle(10_000).unwrap();
    // Applied at the next work item, the end marker: its first tail slice carries the retarget.
    s.set_semantic_policy_json(
        br#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":0.5}}}"#,
        1,
    )
    .unwrap();
    s.signal_end(1, 2048).unwrap();
    let mut buffer = vec![0f32; 2 * 1024];
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        let p = s.pull(&mut buffer, 1024);
        assert_eq!(p.flags & pull_flag::FAILED, 0);
        if p.flags & pull_flag::EOS != 0 {
            break;
        }
        assert!(Instant::now() < deadline, "no end of stream");
        if p.media_frames == 0 {
            std::thread::sleep(Duration::from_millis(1));
        }
    }
    drop(s);
    trace.join("scene")
}

#[test]
fn tail_slices_follow_the_end_of_input() {
    if !mradm_dsp::diagnostics::ENABLED {
        return;
    }
    let root = std::env::temp_dir().join(format!("mradm-scene-tail-{}", std::process::id()));
    let room = replay(&root.join("room"), Selection::RoomBinaural, b"binaural");
    assert!(room.join("e1-g1-s0.20-effective.f32").is_file());
    let tail = room.join("e1-g1-s2048.20-effective.f32");
    assert!(tail.is_file(), "{} missing", tail.display());
    let words = std::fs::read(&tail).unwrap();
    // offset, ramp, x, y, z, gain, head_locked of the retarget: half the initial gain.
    let gain = f32::from_le_bytes(words[20..24].try_into().unwrap());
    assert_eq!(gain, 0.5);

    // The binaural end of input counts no tail: no slice is rendered after sample 2048.
    let binaural = replay(&root.join("binaural"), Selection::SafBinaural, b"binaural");
    assert!(
        binaural
            .join("e1-g1-s0-current.30-binaural-commands.i32")
            .is_file()
    );
    let names: Vec<String> = std::fs::read_dir(&binaural)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    assert!(
        names.iter().all(|n| !n.starts_with("e1-g1-s2048")),
        "binaural rendered a tail slice"
    );
    let _ = std::fs::remove_dir_all(&root);
}
