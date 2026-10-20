use mradm_dsp::room::session::PreparedSet;
use std::{path::PathBuf, sync::Arc};
pub struct Options {
    enabled: bool,
    library: Option<PathBuf>,
    compact_control: bool,
}
impl Options {
    pub fn parse(args: &mut Vec<String>) -> std::result::Result<Self, Box<dyn std::error::Error>> {
        let mut enabled = args.iter().any(|v| v == "--cuda");
        args.retain(|v| v != "--cuda");
        let library = if let Some(index) = args.iter().position(|v| v == "--cuda-library") {
            let value = args
                .get(index + 1)
                .ok_or("--cuda-library needs a path")?
                .into();
            args.drain(index..index + 2);
            enabled = true;
            Some(value)
        } else {
            None
        };
        let compact_control = args.iter().any(|v| v == "--cuda-compact-control");
        args.retain(|v| v != "--cuda-compact-control");
        if compact_control && !enabled {
            return Err("--cuda-compact-control requires a CUDA backend".into());
        }
        Ok(Self {
            enabled,
            library,
            compact_control,
        })
    }
    pub fn prepare(
        &self,
        prepared: Arc<PreparedSet>,
    ) -> std::result::Result<Arc<PreparedSet>, Box<dyn std::error::Error>> {
        if self.enabled {
            let bound = mradm_room_cuda::bind(prepared, self.library.as_deref())?.0;
            if self.compact_control {
                Ok(Arc::new(bound.with_gpu_compact_control()?))
            } else {
                Ok(bound)
            }
        } else {
            Ok(prepared)
        }
    }
    pub fn report(&self) -> serde_json::Value {
        if self.enabled {
            serde_json::json!({"kind":"room-cuda-session","control":"production Session","finite_fade":true,"cpu_reference":false,"binding":"prepared-memory","eq_stagger_frames":256,"compact_control":self.compact_control})
        } else {
            serde_json::Value::Null
        }
    }
}
