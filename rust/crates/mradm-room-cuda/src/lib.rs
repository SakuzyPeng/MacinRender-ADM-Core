//! Optional native ROOM boundary. Resource preparation is synchronous; the DSP
//! library owns all clocks and never links CUDA or performs file I/O.
use mradm_dsp::{Error, Result, room::session::PreparedSet};
use std::{path::Path, sync::Arc};

#[cfg(windows)]
mod native;
#[cfg(windows)]
pub use native::NativeFactory;

pub fn bind(
    prepared: Arc<PreparedSet>,
    library: Option<&Path>,
) -> Result<(Arc<PreparedSet>, serde_json::Value)> {
    bind_impl(prepared, library, false)
}

/// The product opt-in uses the complete retained V2 configuration. Research
/// callers can still select individual native/host optimizations through bind.
pub fn bind_realtime(
    prepared: Arc<PreparedSet>,
    library: Option<&Path>,
) -> Result<(Arc<PreparedSet>, serde_json::Value)> {
    bind_impl(prepared, library, true)
}

fn bind_impl(
    prepared: Arc<PreparedSet>,
    library: Option<&Path>,
    complete_v2: bool,
) -> Result<(Arc<PreparedSet>, serde_json::Value)> {
    #[cfg(windows)]
    {
        use mradm_dsp::room::{
            gpu_experiment::{Backend, Factory, LateBackend},
            prepared::PreparedRoom,
        };
        struct CachedFactory {
            native: Arc<NativeFactory>,
            // Keep the CPU weak-cache entry alive without duplicating its data.
            _prepared: Arc<PreparedSet>,
        }
        impl Factory for CachedFactory {
            fn create(&self, room: Arc<PreparedRoom>) -> Result<Box<dyn Backend>> {
                self.native.create(room)
            }
            fn create_late(&self, room: Arc<PreparedRoom>) -> Result<Option<Box<dyn LateBackend>>> {
                self.native.create_late(room)
            }
        }
        let native = NativeFactory::new(prepared.room.clone(), library)?;
        if complete_v2 && native.optimizations() != 22 {
            return Err(Error::Unsupported(
                "ROOM CUDA realtime requires the complete V2 runtime configuration",
            ));
        }
        let mut metadata = native.metadata()?;
        metadata["compact_control"] = complete_v2.into();
        if complete_v2 {
            metadata["implementation"] = "room-cuda-v2".into();
        }
        let factory = Arc::new(CachedFactory {
            native,
            _prepared: prepared.clone(),
        });
        let bound = prepared
            .with_gpu_experiment(factory)?
            .with_gpu_tuning(true, 0)?;
        let bound = if complete_v2 {
            bound.with_gpu_compact_control()?
        } else {
            bound
        };
        Ok((Arc::new(bound), metadata))
    }
    #[cfg(not(windows))]
    {
        let _ = (prepared, library, complete_v2);
        Err(Error::Unsupported("ROOM CUDA is available only on Windows"))
    }
}
