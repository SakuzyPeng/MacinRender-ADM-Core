//! Private room control, file stream and Scene stream boundary. The room
//! runtime's session calls run through `crate::room::scope`, the boundary they used to cross.
use super::*;
use crate::room::{RoomProfileHandle, prepare_switch};
use mradm_dsp::room::quality::Quality;
use mradm_render::room_runtime::{Config, Prepare};

const SWITCH: &str = "房间切换必须保持当前档位、HRIR 和坐标比例";

pub(crate) fn switch() -> Prepare {
    Arc::new(prepare_switch)
}

/// The room configuration of a switch request (`RenderRoomConfig` on Rust types): `None` without a
/// room package.
pub fn switch_config(
    profile: Option<&RoomProfileHandle>,
    hrir: &[u8],
    metres: f64,
    quality: Quality,
) -> Result<Config, Error> {
    let Some(profile) = profile else {
        return Err(Error {
            code: 1,
            message: SWITCH.as_bytes().to_vec(),
            context: Vec::new(),
        });
    };
    Ok(Config {
        package: profile.package(),
        hrir: String::from_utf8_lossy(hrir).into_owned(),
        metres,
        quality,
    })
}
