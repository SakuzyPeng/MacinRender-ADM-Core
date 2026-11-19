//! The Scene stream engine (`mradm-engine` `scene::stream`) behind the C++
//! `SceneStreamEngine` shell. Errors use the render boundary (code, message, context); the frame
//! records reuse the Scene renderer's entry and update records.
use super::Error;
use crate::engine_render::FfiHost;
use mradm_engine::scene::stream::{Config, Stream};
use std::sync::Arc;

/// A Scene stream handle.
pub struct EngineSceneStream {
    pub(crate) stream: Arc<Stream>,
}

impl EngineSceneStream {
    /// The handle of a created stream (`mradm_engine_scene_create`).
    pub fn new(stream: Stream) -> Box<Self> {
        Box::new(Self {
            stream: Arc::new(stream),
        })
    }

    /// The stream, for Rust callers.
    pub fn stream(&self) -> &Arc<Stream> {
        &self.stream
    }
}

/// The checked Scene stream configuration and renderer, for Rust callers: a stream on the FFI
/// host (room floating-point environment, Windows worker scheduling).
pub fn create_stream(
    config: &Config,
    renderer: &mradm_engine::scene::renderer::Config<'_>,
) -> Result<Box<EngineSceneStream>, Error> {
    Ok(EngineSceneStream::new(Stream::create(
        config,
        renderer,
        Arc::new(FfiHost),
    )?))
}
