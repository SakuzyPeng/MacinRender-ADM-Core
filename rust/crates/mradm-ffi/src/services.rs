//! The Rust services the C++ islands call (`src/adm_core/rust_services_ffi.h`): one table of
//! function pointers that the C ABI hands to the islands (`mradm_islands_bind`) before it first calls
//! one. The entries are not exported by name, so a Rust `cdylib` exports only the `adm_*` C ABI
//! (6.6): rustc exports every `#[no_mangle]` function of the whole crate graph.
use crate::adm::{Text, View};
use crate::render::{RenderLfeRouting, RenderOverride};
use crate::render_apple::{RenderApplePlan, RenderApplePlanView, RenderAppleRequest};
use crate::scene::{DirectMatrix, DirectMatrixView, LayoutEntry, SceneHandle, SceneImportInfo};
use crate::wav::{Info, OutputHandle, ReadHandle, WavBext};
use crate::{meter, output_dsp, render, render_apple, scene, scene_math, wav};
use mradm_dsp::{gain::GainBank, meter::Meter, scene_math::Rotation};

/// `MradmRustServices`: field for field the former `mradm_*` entries, without the prefix.
#[repr(C)]
pub struct RustServices {
    wav_reader_open: unsafe extern "C" fn(
        *const u8,
        usize,
        *mut *mut ReadHandle,
        *mut Info,
        *mut u8,
        usize,
    ) -> i32,
    wav_reader_destroy: unsafe extern "C" fn(*mut ReadHandle),
    wav_reader_read:
        unsafe extern "C" fn(*mut ReadHandle, *mut f32, usize, *mut u64, *mut u8, usize) -> i32,
    wav_reader_seek: unsafe extern "C" fn(*mut ReadHandle, u64, *mut u8, usize) -> i32,
    output_writer_create: unsafe extern "C" fn(
        *const u8,
        usize,
        u32,
        u32,
        u8,
        *const u8,
        usize,
        *mut *mut OutputHandle,
        *mut u8,
        usize,
    ) -> i32,
    output_writer_destroy: unsafe extern "C" fn(*mut OutputHandle),
    output_writer_write:
        unsafe extern "C" fn(*mut OutputHandle, *const f32, usize, *mut u8, usize) -> i32,
    output_writer_finish: unsafe extern "C" fn(*mut OutputHandle, *mut u8, usize) -> i32,
    caf_append_info: unsafe extern "C" fn(
        *const u8,
        usize,
        *const u8,
        usize,
        *const u8,
        usize,
        *const u8,
        usize,
        *mut u8,
        usize,
    ) -> i32,
    wav_append_bext: unsafe extern "C" fn(*const u8, usize, *const WavBext, *mut u8, usize) -> i32,
    dsp_gain_create:
        unsafe extern "C" fn(usize, u32, u32, *mut *mut GainBank, *mut u8, usize) -> i32,
    dsp_gain_destroy: unsafe extern "C" fn(*mut GainBank),
    dsp_gain_reset: unsafe extern "C" fn(*mut GainBank, *mut u8, usize) -> i32,
    dsp_gain_set_targets:
        unsafe extern "C" fn(*mut GainBank, *const f32, usize, *mut u8, usize) -> i32,
    dsp_gain_process:
        unsafe extern "C" fn(*mut GainBank, *mut f32, usize, u32, *mut u8, usize) -> i32,
    dsp_meter_create: unsafe extern "C" fn(
        u32,
        u32,
        u32,
        *const u32,
        usize,
        *mut *mut Meter,
        *mut u8,
        usize,
    ) -> i32,
    dsp_meter_destroy: unsafe extern "C" fn(*mut Meter),
    dsp_meter_add: unsafe extern "C" fn(*mut Meter, *const f32, usize, *mut u8, usize) -> i32,
    dsp_meter_reset: unsafe extern "C" fn(*mut Meter, *mut u8, usize) -> i32,
    dsp_meter_query: unsafe extern "C" fn(*const Meter, u32, u32, *mut f64, *mut u8, usize) -> i32,
    dsp_scene_math: unsafe extern "C" fn(u32, *const f32, usize, *mut f32, usize) -> i32,
    dsp_scene_rotation_create:
        unsafe extern "C" fn(*const f32, usize, u32, *mut *mut Rotation) -> i32,
    dsp_scene_rotation_destroy: unsafe extern "C" fn(*mut Rotation),
    dsp_scene_rotation_update: unsafe extern "C" fn(*mut Rotation, *const f32, usize) -> i32,
    dsp_scene_rotation_apply:
        unsafe extern "C" fn(*const Rotation, *const f32, usize, *mut f32, usize, u32) -> i32,
    scene_layouts:
        unsafe extern "C" fn(u32, *mut *const LayoutEntry, *mut usize, *mut u8, usize) -> i32,
    scene_view: unsafe extern "C" fn(
        *const SceneHandle,
        *mut SceneImportInfo,
        *mut View,
        *mut u8,
        usize,
    ) -> i32,
    scene_destroy: unsafe extern "C" fn(*mut SceneHandle),
    render_live_override:
        unsafe extern "C" fn(*const RenderOverride, usize, Text, Text, *mut f32, *mut u32) -> u32,
    render_lfe_gain: unsafe extern "C" fn(*const RenderLfeRouting, u32, u32) -> f32,
    render_apple_prepare: unsafe extern "C" fn(
        *const RenderAppleRequest,
        *mut *mut RenderApplePlan,
        *mut RenderApplePlanView,
        *mut u8,
        *mut usize,
        *mut u8,
        *mut usize,
    ) -> i32,
    render_apple_plan_destroy: unsafe extern "C" fn(*mut RenderApplePlan),
    /// The entries only the C++ island tests call.
    tests: *const RustTestServices,
}

/// `MradmRustTestServices` (`tests/support/ffi/island_test_ffi.h`).
#[repr(C)]
pub struct RustTestServices {
    scene_import: unsafe extern "C" fn(
        *const u8,
        usize,
        u32,
        u32,
        Text,
        *const Text,
        usize,
        *mut *mut SceneHandle,
        *mut u32,
        *mut u8,
        usize,
    ) -> i32,
    scene_direct_matrix_parse: unsafe extern "C" fn(
        *const u8,
        usize,
        *mut *mut DirectMatrix,
        *mut DirectMatrixView,
        *mut u8,
        *mut usize,
    ) -> i32,
    scene_direct_matrix_destroy: unsafe extern "C" fn(*mut DirectMatrix),
}

// SAFETY: the table holds only function pointers and a pointer to another immutable static.
unsafe impl Sync for RustServices {}

static TEST_SERVICES: RustTestServices = RustTestServices {
    scene_import: scene::mradm_scene_import,
    scene_direct_matrix_parse: scene::mradm_scene_direct_matrix_parse,
    scene_direct_matrix_destroy: scene::mradm_scene_direct_matrix_destroy,
};

pub(crate) static SERVICES: RustServices = RustServices {
    wav_reader_open: wav::mradm_wav_reader_open,
    wav_reader_destroy: wav::mradm_wav_reader_destroy,
    wav_reader_read: wav::mradm_wav_reader_read,
    wav_reader_seek: wav::mradm_wav_reader_seek,
    output_writer_create: wav::mradm_output_writer_create,
    output_writer_destroy: wav::mradm_output_writer_destroy,
    output_writer_write: wav::mradm_output_writer_write,
    output_writer_finish: wav::mradm_output_writer_finish,
    caf_append_info: wav::mradm_caf_append_info,
    wav_append_bext: wav::mradm_wav_append_bext,
    dsp_gain_create: output_dsp::mradm_dsp_gain_create,
    dsp_gain_destroy: output_dsp::mradm_dsp_gain_destroy,
    dsp_gain_reset: output_dsp::mradm_dsp_gain_reset,
    dsp_gain_set_targets: output_dsp::mradm_dsp_gain_set_targets,
    dsp_gain_process: output_dsp::mradm_dsp_gain_process,
    dsp_meter_create: meter::mradm_dsp_meter_create,
    dsp_meter_destroy: meter::mradm_dsp_meter_destroy,
    dsp_meter_add: meter::mradm_dsp_meter_add,
    dsp_meter_reset: meter::mradm_dsp_meter_reset,
    dsp_meter_query: meter::mradm_dsp_meter_query,
    dsp_scene_math: scene_math::mradm_dsp_scene_math,
    dsp_scene_rotation_create: scene_math::mradm_dsp_scene_rotation_create,
    dsp_scene_rotation_destroy: scene_math::mradm_dsp_scene_rotation_destroy,
    dsp_scene_rotation_update: scene_math::mradm_dsp_scene_rotation_update,
    dsp_scene_rotation_apply: scene_math::mradm_dsp_scene_rotation_apply,
    scene_layouts: scene::mradm_scene_layouts,
    scene_view: scene::mradm_scene_view,
    scene_destroy: scene::mradm_scene_destroy,
    render_live_override: render::mradm_render_live_override,
    render_lfe_gain: render::mradm_render_lfe_gain,
    render_apple_prepare: render_apple::mradm_render_apple_prepare,
    render_apple_plan_destroy: render_apple::mradm_render_apple_plan_destroy,
    tests: &raw const TEST_SERVICES,
};
