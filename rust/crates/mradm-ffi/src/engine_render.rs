//! Private boundary of the backend preparation and the offline render step
//! (`mradm-engine` `backend` and `offline`). This layer is the engine's [`Host`]: the room
//! floating-point scope, the room resources with their CUDA selection and the room switch.
use mradm_dsp::room::loading::Package;
use mradm_dsp::room::screen::{Mapping, RoomScreens};
use mradm_engine::backend::{BinauralSpreadMode, Host, SpeakerSpread};
use mradm_render::room;
use mradm_render::room_runtime::{Prepare, Resources, Scope};

/// The platform services of the engine (formerly the C++ `RoomRenderer::prepare` calls).
pub struct FfiHost;

impl Host for FfiHost {
    fn validate_screens(&self, screens: &RoomScreens) -> Result<(), (i32, String)> {
        crate::room::scope(&mut || Mapping::new(*screens).map(drop))
            .map_err(|(code, message)| (code, message.to_owned()))
    }

    fn prepare_room(
        &self,
        package: &Package,
        hrir: &[u8],
        sofa: &[u8],
        quality: u32,
        metres: f64,
    ) -> Result<Resources, (i32, String)> {
        let mut out = None;
        crate::room::scope(&mut || {
            let (set, cuda, metadata) =
                crate::room::prepare_set(package, hrir, sofa, None, quality, metres)?;
            // `mradm_room_prepare_quality` also described the set; keep its failure.
            crate::room::description(&set, metres, metadata)?;
            out = Some(Resources { set, cuda });
            Ok(())
        })
        .map_err(|(code, message)| (code, message.to_owned()))?;
        Ok(out.expect("prepared in scope"))
    }

    fn room_polar(&self) -> room::PolarConversion {
        crate::render::room_polar
    }

    fn room_scope(&self) -> Scope {
        crate::room::scope
    }

    fn room_switch(&self) -> Prepare {
        crate::render::room_ffi::switch()
    }
    #[cfg(windows)]
    fn room_worker(&self, active: bool) {
        crate::scene_worker::set_active(active);
    }
    #[cfg(windows)]
    fn room_wait(&self, duration: std::time::Duration) {
        crate::scene_worker::wait(duration);
    }
}

pub fn speaker_spread_of(value: u32) -> Option<SpeakerSpread> {
    Some(match value {
        0 => SpeakerSpread::Automatic,
        1 => SpeakerSpread::None,
        2 => SpeakerSpread::Mdap,
        _ => return None,
    })
}

pub fn binaural_spread_of(value: u32) -> Option<BinauralSpreadMode> {
    Some(match value {
        0 => BinauralSpreadMode::Automatic,
        1 => BinauralSpreadMode::None,
        2 => BinauralSpreadMode::Cloud,
        3 => BinauralSpreadMode::SafSpreader,
        _ => return None,
    })
}
