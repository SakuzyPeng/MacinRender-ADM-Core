//! Rendering on top of the render scene (PCM consistency 4.5): the shared DirectSpeakers, label
//! and LFE routing rules, the offline driver, file streaming and the loudspeaker and HOA backends.
#![forbid(unsafe_code)]

pub mod hoa;
pub mod offline;
pub mod routing;
pub mod scene;
pub mod stream;
pub mod vbap;
