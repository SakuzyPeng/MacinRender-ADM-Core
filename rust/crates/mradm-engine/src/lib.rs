//! Render orchestration (PCM consistency phase 5): the backend capability catalog and the
//! discovery JSON documents built from it (5.2 A), moved from the C++ engine with the same tables,
//! order and bytes. Platform facts the crate cannot know (build switches, encoder availability,
//! the Apple backend and the system-spatial sinks) arrive as [`catalog::Platform`] data.
#![forbid(unsafe_code)]

pub mod catalog;
pub mod scene_json;
pub mod tables;
