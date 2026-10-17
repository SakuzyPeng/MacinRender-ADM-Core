//! The render scene (PCM consistency 4.3): loudspeaker layouts, channel-bed input presets and the
//! input import that produces the scene the renderers consume.
#![forbid(unsafe_code)]

pub mod checkpoint;
pub mod error;
pub mod import;
pub mod input;
pub mod layouts;

pub use error::{Context, Error, Result};
pub use import::{ImportOptions, Info, RenderScene, SourceKind, import, import_adm};
