//! The monitor-v1 scene inputs: silent one-second BW64 files of the policy-v1 and request-v1
//! scenes at 48 and 44.1 kHz, named `<scene>-<rate>.wav` (the C++ generator read the same files).
use super::adm_wav_with;
use mradm_wav::SampleFormat;
use std::path::{Path, PathBuf};

pub const SCENES: &[(&str, &str)] = &[
    ("policy-v1", "objects-point"),
    ("policy-v1", "objects-room"),
    ("policy-v1", "objects-extent-multi"),
    ("policy-v1", "objects-cartesian"),
    ("policy-v1", "objects-stream-only"),
    ("policy-v1", "directspeakers"),
    ("policy-v1", "directspeakers-lfe"),
    ("policy-v1", "hoa3"),
    ("policy-v1", "hoa"),
    ("request-v1", "ds-aliases"),
    ("request-v1", "ds-two-labels"),
    ("request-v1", "ds-lowpass-lfe"),
];
pub const RATES: [u32; 2] = [48000, 44100];
pub const FRAMES: usize = 48000;

fn scenes(set: &str) -> PathBuf {
    let crate_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    match set {
        "request-v1" => crate_dir.join("tests/request-v1/scenes"),
        _ => crate_dir.join("../mradm-scene/tests/policy-v1/scenes"),
    }
}

pub fn write_inputs(out: &Path) {
    std::fs::create_dir_all(out).unwrap();
    for (set, name) in SCENES {
        for rate in RATES {
            adm_wav_with(
                &scenes(set),
                name,
                &out.join(format!("{name}-{rate}.wav")),
                SampleFormat::Pcm24,
                Some(rate),
                FRAMES,
            );
        }
    }
}
