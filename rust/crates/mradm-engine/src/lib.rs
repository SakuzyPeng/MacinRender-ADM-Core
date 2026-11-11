//! Render orchestration: the backend capability catalog and the
//! discovery JSON documents built from it, the render request resolution, and the
//! backend preparation and offline render step, the file-stream and monitor opening
//! and the Scene stream renderer creation, moved from the C++ engine with the same tables,
//! order and bytes. Platform facts the crate cannot know (build switches, encoder availability,
//! the Apple backend and the system-spatial sinks) arrive as [`catalog::Platform`] data.
#![forbid(unsafe_code)]

#[cfg(test)]
extern crate self as mradm_engine;
#[cfg(test)]
#[path = "../tests/common/mod.rs"]
mod test_common;

pub mod backend;
pub mod catalog;
pub mod fmt;
pub mod monitor;
pub mod offline;
pub mod render;
pub mod request;
mod ring;
pub mod scene;
pub mod scene_json;
pub mod stream;
pub mod tables;
