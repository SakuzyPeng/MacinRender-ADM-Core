//! The Rust interface of this crate: what `mradm-capi` (the C ABI and the CLI service) and the
//! consistency examples call. Everything else in the crate is the private C boundary of the C++
//! islands and tests, reached only through the services table (`islands::services`).
//!
//! Each group stays here, rather than in a safe crate, because it holds a C table, a callback that
//! keeps an address, or a platform service.

/// The C++ island tables the C ABI registers, and the text view their callbacks pass.
pub mod islands {
    pub use crate::adm::Text;
    pub use crate::engine::{Facts, PlatformIslands, platform};
    pub use crate::render::apple_island::AppleIslands;
    pub use crate::render::device_island::{DeviceEnumeration, DeviceIslands};
    pub use crate::render::device_island::{DevicePull, DeviceTable};
    pub use crate::render::{
        engine_monitor::EngineIslandError, engine_scene_output::EngineSceneDeviceProgress,
    };
    pub use crate::services::RustServices;

    /// This build's output devices: miniaudio (hardware and the null backend) and, on macOS and
    /// Windows, the system-spatial sink.
    pub fn devices() -> *const DeviceIslands {
        &raw const crate::render::device_island::DEVICES
    }

    /// The Rust services the islands call: the C ABI hands this table to the islands
    /// (`mradm_islands_bind`) before it first calls one.
    pub fn services() -> *const RustServices {
        &raw const crate::services::SERVICES
    }
}

/// Offline render and preview: the engine state machine stepped over the islands, and the platform
/// services of the engine (room floating-point environment, CUDA, room switching).
pub mod render {
    pub use crate::apac::run_child as run_apac_child;
    pub use crate::engine_render::{FfiHost, binaural_spread_of, speaker_spread_of};
    pub use crate::engine_service::{EngineProgress, IslandContext, IslandRunner, RenderDriver};
    pub use crate::render::RenderPrepared;
}

/// An imported render scene shared by renders, streams and monitors.
pub mod scene {
    pub use crate::scene::SceneHandle;
}

/// Scene streams (on the engine host) and Scene outputs (on the device islands; pinned, since the
/// device callback keeps the output's address).
pub mod scene_stream {
    pub use crate::render::engine_scene::{EngineSceneStream, create_stream};
    pub use crate::render::engine_scene_output::EngineSceneOutput;
}

/// Realtime monitors on the device and Apple islands.
pub mod monitor {
    pub use crate::render::engine_monitor_session::{
        EngineMonitorSession, EngineMonitorSessionHost, choice, hptf_design,
    };
}

/// Room packages and preparations, which run in the room floating-point environment.
pub mod room {
    pub use crate::render::room_ffi::switch_config;
    pub use crate::room::{RoomPreparedHandle, RoomProfileHandle, error_code, validate_screens};
}

/// HpTF requests read from the C ABI records, and the validation of parsed profiles.
pub mod hptf {
    pub use crate::hptf::settings::{Settings, profile_error};
}

/// The OSC loopback receiver with the platform's bind (Windows: `SO_EXCLUSIVEADDRUSE`).
pub mod osc {
    pub use crate::osc::receiver;
}

/// The Windows file API and C runtime's shared path encoding for the C ABI and IAMF bridge.
#[cfg(windows)]
pub mod windows_paths {
    pub use crate::windows_path::{code_page, native_bytes};
}
