//! Optional native ROOM boundary. Resource preparation is synchronous; the DSP
//! library owns all clocks and never links CUDA or performs file I/O.
#[cfg(not(windows))]
use mradm_dsp::Error;
use mradm_dsp::{Result, room::session::PreparedSet};
use std::{path::Path, sync::Arc};

#[cfg(windows)]
mod native;
#[cfg(windows)]
pub use native::NativeFactory;

pub fn bind(
    prepared: Arc<PreparedSet>,
    library: Option<&Path>,
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
        let metadata = native.metadata()?;
        let factory = Arc::new(CachedFactory {
            native,
            _prepared: prepared.clone(),
        });
        let bound = prepared
            .with_gpu_experiment(factory)?
            .with_gpu_tuning(true, 0)?;
        Ok((Arc::new(bound), metadata))
    }
    #[cfg(not(windows))]
    {
        let _ = (prepared, library);
        Err(Error::Unsupported("ROOM CUDA is available only on Windows"))
    }
}
