//! Shared by `consistency_render` and `repeat_render`: the offline render request with the
//! defaults of the former C++ `RenderOptions`, and the platform of the WAV / CAF renders.
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, Screens, SpeakerSpread};
use mradm_engine::render::{PathText, Request, RoomOptions};
use mradm_engine::request::{BitDepth, DsRouting, Selection};
use std::path::Path;

pub fn text(path: &Path) -> PathText {
    let utf8 = path.to_string_lossy().into_owned();
    PathText {
        display: utf8.clone(),
        utf8,
    }
}

/// `RenderOptions` with the tool's float32 default, as `RequestTexts::record` passes it
/// (defaults: `include/adm/options.h` `RenderOptions` and `include/adm/room.h` `RoomConfig`).
pub fn request(input: &Path, output: &Path) -> Request {
    Request {
        input: text(input),
        output: Some(text(output)),
        renderer: Selection::Automatic as u32,
        output_layout: "binaural".to_owned(),
        input_layout: None,
        input_channel_labels: Vec::new(),
        measure_loudness: false,
        loudness_target_lufs: -23.,
        peak_limit: true,
        peak_limit_dbtp: -1.,
        peak_normalize_to_limit: false,
        final_gain_db: 0.,
        output_bit_depth: BitDepth::F32 as u32,
        opus_bitrate_per_ch_kbps: 0,
        apac_bitrate_kbps: 0,
        apac_drc_music: true,
        apac_caf: false,
        sofa: None,
        semantic_policy_path: None,
        semantic_policy_json: None,
        matrix_path: None,
        matrix_json: None,
        semantic_report_path: None,
        capture_semantic_report: false,
        default_interp_ms: 5,
        object_smoothing_frames: 0,
        apple_geometry: false,
        routing: DsRouting::Automatic as u32,
        speaker_spread: SpeakerSpread::Automatic as u32,
        // `TripleBalanceMode::standard`.
        triple_balance_mode: 0,
        binaural_spread: BinauralSpreadMode::Automatic as u32,
        split_power: false,
        // `AppleSpatialPreset::off`.
        apple_spatial_preset: 0,
        apple_speaker_rendering_flags: false,
        listener_orientation: [0.; 3],
        render_start_sec: 0.,
        render_end_sec: None,
        room: RoomOptions {
            package: None,
            hrir: Vec::new(),
            metres: 0.,
            // `RoomQuality::exact`.
            quality: 0,
        },
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry::default(),
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        internal_allow_speaker_stereo: false,
        iamf_mp4: false,
        iamf_layers: Vec::new(),
    }
}

/// The C++ `engine::RenderPlatform` without the Apple backend: the tool's renderers and WAV/CAF
/// outputs read only the SOFA build switch.
pub fn base_platform() -> mradm_engine::catalog::Platform {
    mradm_engine::catalog::Platform {
        sofa: cfg!(feature = "sofa"),
        ..Default::default()
    }
}
