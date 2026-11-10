//! Render orchestration (PCM consistency phase 5): the backend capability catalog and the
//! discovery JSON documents built from it (5.2 A), the render request resolution (5.2 B), and the
//! backend preparation and offline render step (5.2 C1), the file-stream and monitor opening (5.3)
//! and the Scene stream renderer creation (5.4 A), moved from the C++ engine with the same tables,
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
pub mod offline;
pub mod render;
pub mod request;
pub mod scene;
pub mod scene_json;
pub mod stream;
pub mod tables;
