//! The `mradm` command line: `mradm_cli_main`, which the `mradm` executable's `main` calls, runs
//! `mradm-cli` with this crate's engine entries (the same render driver, scene import, AXML reader,
//! export and room package entries the C ABI exports use), the APAC encode worker of the audio
//! island and a SIGINT handler.
use crate::error::{Failure, with_utf8_paths};
use crate::options::{Path, adm_render_options_t, file_name};
use crate::query::{BAD_ALLOC, axml, import_with, owned};
use crate::render::{Sink, drive, export_logged, islands};
use crate::room::{Config, inspect};
use crate::scene::screens;
use mradm_cli::{Event, Facts, Interrupt, Outcome, Platform, Request, Screens, Service};
use mradm_ffi::adm::Text;
use mradm_ffi::engine_service::{EnginePath, EngineProgress, EngineRenderRequest};
use mradm_ffi::room::RoomProfileHandle;
use std::ffi::{CString, c_char, c_int};
use std::io::{Read, Write};
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};

/// The message of a failure the C++ would have thrown (an allocation failure).
fn message(failure: Failure) -> Vec<u8> {
    match failure {
        Failure::Error(error) => error.message,
        Failure::Exception => BAD_ALLOC.to_vec(),
    }
}

/// A command-line path as the file system takes it.
fn fs_path(bytes: &[u8]) -> std::path::PathBuf {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        std::ffi::OsStr::from_bytes(bytes).into()
    }
    #[cfg(not(unix))]
    {
        String::from_utf8_lossy(bytes).into_owned().into()
    }
}

/// A loaded room package.
pub(crate) struct RoomPackage(*mut RoomProfileHandle);

impl Drop for RoomPackage {
    fn drop(&mut self) {
        unsafe { crate::options::release_profile(self.0) };
    }
}

/// The SIGINT handler of one render.
pub(crate) struct Sigint {
    previous: usize,
}

static SIGINT_REQUESTED: AtomicBool = AtomicBool::new(false);

extern "C" fn on_sigint(_: c_int) {
    SIGINT_REQUESTED.store(true, Ordering::Release);
}

const SIGINT: c_int = 2;
const SIG_ERR: usize = usize::MAX;

unsafe extern "C" {
    fn signal(signum: c_int, handler: usize) -> usize;
}

impl Interrupt for Sigint {
    fn requested(&self) -> bool {
        SIGINT_REQUESTED.load(Ordering::Acquire)
    }
}

impl Drop for Sigint {
    fn drop(&mut self) {
        unsafe { signal(SIGINT, self.previous) };
    }
}

#[cfg(not(test))]
unsafe extern "C" {
    /// `run_apac_encode_child` with its protocol lines on stdout; the exit code.
    fn mradm_audio_apac_child(
        src: *const c_char,
        dst: *const c_char,
        layout: *const c_char,
        bitrate: u32,
        drc_music: u32,
        caf: u32,
    ) -> i32;
}

#[cfg(test)]
unsafe fn mradm_audio_apac_child(
    _: *const c_char,
    _: *const c_char,
    _: *const c_char,
    _: u32,
    _: u32,
    _: u32,
) -> i32 {
    1
}

/// The command line's render events.
struct Events<'a> {
    cancelled: &'a (dyn Fn() -> bool + Sync),
    events: &'a (dyn Fn(Event<'_>) + Sync),
}

fn bytes<'a>(t: &Text) -> &'a [u8] {
    if t.len == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(t.data, t.len) }
    }
}

impl Sink for Events<'_> {
    fn cancelled(&self) -> bool {
        (self.cancelled)()
    }

    fn progress(&self, event: &EngineProgress) -> Result<(), Failure> {
        (self.events)(Event::Progress {
            fraction: event.fraction,
            message: bytes(&event.message),
        });
        Ok(())
    }

    fn log(&self, level: u32, module: &[u8], message: &[u8]) -> Result<(), Failure> {
        (self.events)(Event::Log {
            level,
            module,
            message,
        });
        Ok(())
    }
}

fn engine_path(bytes: &[u8]) -> EnginePath {
    EnginePath {
        utf8: Text::bytes(bytes),
        display: Text::bytes(bytes),
    }
}

/// `RequestTexts::record` of a command-line request; the views borrow `request` and `views`.
fn record<'a>(
    request: &'a Request<'_, RoomPackage>,
    labels: &'a [Text],
    layers: &'a [Text],
) -> EngineRenderRequest {
    let optional = |p: Option<&'a [u8]>| (u32::from(p.is_some()), engine_path(p.unwrap_or(&[])));
    let (has_sofa, sofa) = optional(request.sofa);
    let (has_policy, policy) = optional(request.semantic_policy);
    let (has_matrix, matrix) = optional(request.matrix);
    let (has_report, report) = optional(request.semantic_report);
    let room = request.room.as_ref();
    let screens = request.screens;
    EngineRenderRequest {
        input: engine_path(request.input),
        has_output: u32::from(request.output.is_some()),
        output: engine_path(request.output.unwrap_or(&[])),
        renderer: request.renderer,
        output_layout: Text::bytes(request.output_layout),
        has_input_layout: u32::from(request.input_layout.is_some()),
        input_layout: Text::bytes(request.input_layout.unwrap_or(&[])),
        input_channel_labels: labels.as_ptr(),
        input_channel_labels_len: labels.len(),
        measure_loudness: u32::from(request.measure_loudness),
        loudness_target_lufs: request.loudness_target_lufs,
        peak_limit: u32::from(request.peak_limit),
        peak_limit_dbtp: request.peak_limit_dbtp,
        peak_normalize_to_limit: u32::from(request.peak_normalize_to_limit),
        final_gain_db: request.final_gain_db,
        output_bit_depth: request.output_bit_depth,
        opus_bitrate_per_ch_kbps: request.opus_bitrate_per_ch_kbps,
        apac_bitrate_kbps: request.apac_bitrate_kbps,
        apac_drc_music: u32::from(request.apac_drc_music),
        apac_caf: u32::from(request.apac_caf),
        has_sofa,
        sofa,
        sofa_file_name: Text::bytes(file_name(request.sofa.unwrap_or(&[]))),
        has_policy_path: has_policy,
        policy_path: policy,
        has_policy_json: 0,
        policy_json: Text::bytes(&[]),
        has_matrix_path: has_matrix,
        matrix_path: matrix,
        has_matrix_json: 0,
        matrix_json: Text::bytes(&[]),
        has_report_path: has_report,
        report_path: report,
        capture_report: 0,
        default_interp_ms: request.default_interp_ms,
        object_smoothing_frames: request.object_smoothing_frames,
        apple_geometry: u32::from(request.apple_geometry),
        routing: request.routing,
        speaker_spread: request.speaker_spread,
        triple_balance_mode: request.triple_balance_mode,
        binaural_spread: request.binaural_spread,
        split_power: u32::from(request.split_power),
        apple_spatial_preset: request.apple_spatial_preset,
        apple_speaker_rendering_flags: u32::from(request.apple_speaker_rendering_flags),
        listener_orientation: request.listener_orientation,
        render_start_sec: request.render_start_sec,
        has_end: u32::from(request.render_end_sec.is_some()),
        render_end_sec: request.render_end_sec.unwrap_or(0.0),
        room_profile: room.map_or(ptr::null(), |r| r.package.0.cast_const()),
        room_hrir: Text::bytes(room.map_or(&[][..], |r| r.hrir)),
        room_metres: room.map_or(0.0, |r| r.metres),
        room_quality: room.map_or(0, |r| r.quality),
        has_reference: u32::from(screens.reference.is_some()),
        reference: screens.reference.unwrap_or_default(),
        has_reproduction: u32::from(screens.reproduction.is_some()),
        reproduction: screens.reproduction.unwrap_or_default(),
        internal_allow_speaker_stereo: 0,
        iamf_mp4: u32::from(request.iamf_mp4),
        iamf_layers: layers.as_ptr(),
        iamf_layers_len: layers.len(),
    }
}

/// The engine behind the command line.
struct Engine;

impl Service for Engine {
    type Room = RoomPackage;
    type Interrupt = Sigint;

    fn platform(&self, facts: Facts) -> Platform {
        let facts = match facts {
            Facts::Render => mradm_ffi::engine::Facts::Render,
            Facts::Document(kind) => mradm_ffi::engine::Facts::Document(kind),
        };
        let islands = unsafe { islands::mradm_platform_islands().as_ref() };
        islands
            .and_then(|p| unsafe { mradm_ffi::engine::platform(p, facts) })
            .unwrap_or_default()
    }

    fn load_screens(&self, path: &[u8]) -> Result<Screens, Vec<u8>> {
        const MAX: usize = 1024 * 1024;
        let Ok(mut file) = std::fs::File::open(fs_path(path)) else {
            return Err("无法读取屏幕配置文件".as_bytes().to_vec());
        };
        let mut text = Vec::new();
        let mut buffer = [0u8; 4096];
        loop {
            match file.read(&mut buffer) {
                Ok(0) => break,
                Ok(n) => {
                    text.extend_from_slice(&buffer[..n]);
                    if text.len() > MAX {
                        return Err("屏幕配置超过 1 MiB".as_bytes().to_vec());
                    }
                }
                Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
                Err(_) => return Err("读取屏幕配置失败".as_bytes().to_vec()),
            }
        }
        let (reference, reproduction) = unsafe { screens(&text) }.map_err(message)?;
        Ok(Screens {
            reference,
            reproduction,
        })
    }

    fn load_room(&self, manifest: &[u8]) -> Result<RoomPackage, Vec<u8>> {
        match RoomProfileHandle::load(manifest) {
            Ok(profile) => Ok(RoomPackage(Box::into_raw(profile))),
            Err(error) => Err(mradm_ffi::room::error_code(&error).1.as_bytes().to_vec()),
        }
    }

    fn render(
        &self,
        request: &Request<'_, RoomPackage>,
        cancelled: &(dyn Fn() -> bool + Sync),
        events: &(dyn Fn(Event<'_>) + Sync),
    ) -> Outcome {
        let labels: Vec<Text> = request
            .input_channel_labels
            .iter()
            .map(|l| Text::bytes(l))
            .collect();
        let layers: Vec<Text> = request.iamf_layers.iter().map(|l| Text::bytes(l)).collect();
        let record = record(request, &labels, &layers);
        let sink = Events { cancelled, events };
        match drive(&record, &sink, None, None) {
            Ok(outcome) => Outcome {
                code: outcome.code,
                message: outcome.message,
                output_path: outcome.output_path,
            },
            Err(failure) => Outcome {
                code: crate::guard::INTERNAL,
                message: message(failure),
                output_path: None,
            },
        }
    }

    fn inspect_text(&self, input: &[u8]) -> Result<Vec<u8>, Vec<u8>> {
        with_utf8_paths(|| {
            let scene = import_with(input, true, None, &[])?;
            owned(mradm_engine::scene_json::inspect_text(scene.render_scene()).as_bytes())
        })
        .map_err(message)
    }

    fn axml(&self, input: &[u8]) -> Result<Vec<u8>, Vec<u8>> {
        with_utf8_paths(|| axml(input)).map_err(message)
    }

    fn write_policy_template(&self, input: &[u8], output: &[u8]) -> Result<(), Vec<u8>> {
        let json = with_utf8_paths(|| {
            let scene = import_with(input, true, None, &[])?;
            owned(mradm_scene::report::template(scene.render_scene()).as_bytes())
        })
        .map_err(message)?;
        // `write_policy_json`.
        let Ok(mut file) = std::fs::File::create(fs_path(output)) else {
            return Err(
                b"semantic policy I/O error: cannot open policy template for writing".to_vec(),
            );
        };
        if file
            .write_all(&json)
            .and_then(|()| file.write_all(b"\n"))
            .is_err()
        {
            return Err(b"semantic policy I/O error: failed to write policy template".to_vec());
        }
        Ok(())
    }

    fn export(
        &self,
        input: &[u8],
        output: &[u8],
        policy: Option<&[u8]>,
        warning: &mut dyn FnMut(&[u8]),
    ) -> Result<(), Vec<u8>> {
        let mut options = adm_render_options_t::defaults().ok_or_else(|| BAD_ALLOC.to_vec())?;
        options.semantic_policy_path = policy.map(|p| Path { utf8: p.to_vec() });
        with_utf8_paths(|| {
            export_logged(input, output, &options, |_, module, line| {
                if module == b"semantic-policy" {
                    warning(line);
                }
            })
        })
        .map_err(message)
    }

    fn inspect_room(
        &self,
        room: &RoomPackage,
        hrir: &[u8],
        quality: u32,
    ) -> Result<Vec<u8>, Vec<u8>> {
        let config = Config {
            profile: room.0,
            hrir,
            metres: 0.0,
        };
        unsafe { inspect(&config, quality as i32) }.map_err(message)
    }

    fn apac_encode_child(
        &self,
        input: &[u8],
        output: &[u8],
        layout: &[u8],
        bitrate: u32,
        drc_music: bool,
        caf: bool,
    ) -> i32 {
        // Command-line arguments hold no NUL.
        let (Ok(input), Ok(output), Ok(layout)) = (
            CString::new(input),
            CString::new(output),
            CString::new(layout),
        ) else {
            return 1;
        };
        unsafe {
            mradm_audio_apac_child(
                input.as_ptr(),
                output.as_ptr(),
                layout.as_ptr(),
                bitrate,
                u32::from(drc_music),
                u32::from(caf),
            )
        }
    }

    fn interrupt(&self) -> Option<Sigint> {
        SIGINT_REQUESTED.store(false, Ordering::Release);
        let previous = unsafe { signal(SIGINT, on_sigint as extern "C" fn(c_int) as usize) };
        (previous != SIG_ERR).then_some(Sigint { previous })
    }
}

/// Runs the `mradm` command line of this process; the exit code (101 for a panic).
#[unsafe(no_mangle)]
pub extern "C" fn mradm_cli_main() -> c_int {
    crate::guard::guard(101, || {
        mradm_cli::run(&Engine, std::env::args_os().collect())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn screen_files_report_the_cpp_messages() {
        let missing = Engine.load_screens(b"/nonexistent/screen.json");
        assert_eq!(missing.err().unwrap(), "无法读取屏幕配置文件".as_bytes());
    }

    #[test]
    fn missing_room_packages_report_the_loader_message() {
        let error = Engine.load_room(b"/nonexistent/room.json").err().unwrap();
        assert!(!error.is_empty());
    }
}
