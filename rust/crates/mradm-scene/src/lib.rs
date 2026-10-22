//! The render scene (PCM consistency 4.3): loudspeaker layouts, channel-bed input presets and the
//! input import that produces the scene the renderers consume, and the semantic policy applied
//! to it (4.4).
#![forbid(unsafe_code)]

pub mod checkpoint;
pub mod error;
pub mod import;
pub mod input;
pub mod layouts;
pub mod live_policy;
pub mod policy;
pub mod report;

pub use error::{Context, Error, Result};
pub use import::{ImportOptions, Info, RenderScene, SourceKind, import, import_adm};
