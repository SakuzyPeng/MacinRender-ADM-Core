//! Rendering on top of the render scene: the shared DirectSpeakers, label
//! and LFE routing rules, the offline driver, file streaming, the loudspeaker and HOA backends and
//! the Scene stream VBAP renderer; the Triple Balance and room backends; the binaural HRTF
//! state, source plan and offline renderer; the final float PCM writers and the
//! post-processing passes.
#![forbid(unsafe_code)]

pub mod apple;
pub mod binaural;
pub mod binaural_hrtf;
pub mod binaural_render;
pub mod binaural_stream;
pub mod ear;
pub mod hoa;
pub mod live;
pub mod live_binaural;
pub mod live_triple_balance;
pub mod live_vbap;
pub mod offline;
pub mod output;
pub mod postprocess;
pub mod room;
pub mod room_runtime;
pub mod room_stream;
pub mod routing;
pub mod scene;
pub mod stream;
pub mod triple_balance;
pub mod triple_balance_stream;
pub mod vbap;
