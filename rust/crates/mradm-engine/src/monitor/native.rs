//! A Rust file stream (`stream::Body`) as a monitor source: the calls the C++ `EngineStream`
//! adapter made, with the stop token sampled before every seek call.
use super::{Cancel, Overrides, Source, Validate};
use crate::stream::{Body, Info, Validator};
use mradm_render::offline;
use mradm_render::room_runtime::Control as RoomControl;
use mradm_render::routing::Error;
use std::sync::Arc;

pub struct Native {
    validator: Arc<NativeValidator>,
    body: Body,
    info: Info,
}

struct NativeValidator(Validator);

impl Validate for NativeValidator {
    fn validate(&self, overrides: &Overrides) -> Result<(), Error> {
        self.0
            .validate(&overrides.views())
            .map_err(|(code, message, context)| Error {
                code,
                message: message.into_bytes(),
                context,
            })
    }
}

fn error(e: offline::Error) -> Error {
    Error {
        code: e.code,
        message: e.message.into_bytes(),
        context: Vec::new(),
    }
}

impl Native {
    pub fn new(validator: Validator, body: Body, info: Info) -> Self {
        Self {
            validator: Arc::new(NativeValidator(validator)),
            body,
            info,
        }
    }

    fn seek_sampled(&mut self, frame: u64, cancelled: impl Fn() -> bool) -> Result<(), Error> {
        let mut pending = self.body.seek_begin(frame, cancelled()).map_err(error)?;
        while pending {
            pending = self.body.seek_step(cancelled()).map_err(error)?;
        }
        Ok(())
    }
}

impl Source for Native {
    fn out_channels(&self) -> usize {
        self.info.outputs
    }
    fn sample_rate(&self) -> u32 {
        self.info.sample_rate
    }
    fn process(&mut self, output: &mut [f32], frames: usize) -> Result<usize, Error> {
        self.body.process(output, frames).map_err(error)
    }
    fn seek(&mut self, frame: u64) -> Result<(), Error> {
        self.seek_sampled(frame, || false)
    }
    fn seek_with_cancel(&mut self, frame: u64, cancel: &Cancel) -> Result<(), Error> {
        self.seek_sampled(frame, || cancel.requested())
    }
    fn seek_requires_preroll(&self) -> bool {
        self.info.requires_preroll
    }
    fn validator(&self) -> Arc<dyn Validate> {
        self.validator.clone()
    }
    fn set_overrides(&mut self, overrides: &Overrides) {
        // The C++ adapter terminated when the prepared stream rejected validated edits.
        if self.body.set_overrides(&overrides.views()).is_err() {
            std::process::abort();
        }
    }
    fn set_orientation(&mut self, pose: [f32; 3]) {
        self.body.set_pose(pose);
    }
    fn room_control(&self) -> Option<RoomControl> {
        self.body.control()
    }
}
