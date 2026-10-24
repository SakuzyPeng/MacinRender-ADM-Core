//! Rendering on top of the render scene (PCM consistency 4.5): the shared DirectSpeakers, label
//! and LFE routing rules, the offline driver, file streaming, the loudspeaker and HOA backends and
//! the Scene stream VBAP renderer; the Triple Balance room backend (4.6).
#![forbid(unsafe_code)]

pub mod ear;
pub mod hoa;
pub mod live_vbap;
pub mod offline;
pub mod routing;
pub mod scene;
pub mod stream;
pub mod triple_balance;
pub mod vbap;
