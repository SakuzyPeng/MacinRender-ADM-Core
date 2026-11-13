//! The `mradm` command line: the subcommands, their options and messages, exit codes and output,
//! moved from the C++ CLI (CLI11 and spdlog) with the same contract. Parsing is clap's, checked in
//! CLI11's order with its exit codes ([`args`]); the engine work goes through a [`Service`], which
//! the C ABI crate implements with the same entries its exports call (the render step machine with
//! the islands, the scene import, the room package), so the CLI and the C ABI cannot drift apart.
#![forbid(unsafe_code)]

mod args;
mod catalog;
mod convert;
mod output;

use mradm_dsp::room::screen::RoomScreenGeometry;
pub use mradm_engine::catalog::Platform;
use output::{Level, Logger, stderr, stdout};
use std::ffi::OsString;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

/// The project version (`project(VERSION)` of the top-level CMakeLists.txt).
pub const VERSION: &str = env!("MRADM_VERSION");

/// The platform facts a command reads: a render's, or those of discovery document `kind` (0
/// capabilities, 1 layouts, 2 output formats).
#[derive(Clone, Copy)]
pub enum Facts {
    Render,
    Document(u32),
}

/// A render's progress or log line, as the engine and the islands report it.
pub enum Event<'a> {
    Progress {
        fraction: f64,
        message: &'a [u8],
    },
    /// `level` is an `adm::LogLevel` value.
    Log {
        level: u32,
        module: &'a [u8],
        message: &'a [u8],
    },
}

/// A finished render: the error code (0 on success) and message, and the written path.
pub struct Outcome {
    pub code: i32,
    pub message: Vec<u8>,
    pub output_path: Option<Vec<u8>>,
}

/// The reference and reproduction screens of `--screen-config`.
#[derive(Clone, Copy, Default)]
pub struct Screens {
    pub reference: Option<RoomScreenGeometry>,
    pub reproduction: Option<RoomScreenGeometry>,
}

/// The room of a `room-binaural` render.
pub struct Room<'a, R> {
    pub package: &'a R,
    pub hrir: &'a [u8],
    pub metres: f64,
    /// 0 exact, 1 realtime.
    pub quality: u32,
}

/// A render request (`RenderRequest`): paths are the command line's bytes (UTF-8 on Windows);
/// enumerations are the `adm::` values.
pub struct Request<'a, R> {
    pub input: &'a [u8],
    pub output: Option<&'a [u8]>,
    pub renderer: u32,
    pub output_layout: &'a [u8],
    pub input_layout: Option<&'a [u8]>,
    pub input_channel_labels: Vec<Vec<u8>>,
    pub measure_loudness: bool,
    pub loudness_target_lufs: f32,
    pub peak_limit: bool,
    pub peak_limit_dbtp: f32,
    pub peak_normalize_to_limit: bool,
    pub final_gain_db: f64,
    pub output_bit_depth: u32,
    pub opus_bitrate_per_ch_kbps: u32,
    pub apac_bitrate_kbps: u32,
    pub apac_drc_music: bool,
    pub apac_caf: bool,
    pub sofa: Option<&'a [u8]>,
    pub semantic_policy: Option<&'a [u8]>,
    pub semantic_report: Option<&'a [u8]>,
    pub default_interp_ms: u32,
    pub object_smoothing_frames: u32,
    pub apple_geometry: bool,
    pub routing: u32,
    pub matrix: Option<&'a [u8]>,
    pub speaker_spread: u32,
    pub triple_balance_mode: u32,
    pub binaural_spread: u32,
    pub split_power: bool,
    pub apple_spatial_preset: u32,
    pub apple_speaker_rendering_flags: bool,
    /// Yaw, pitch, roll in degrees.
    pub listener_orientation: [f32; 3],
    pub render_start_sec: f64,
    pub render_end_sec: Option<f64>,
    pub room: Option<Room<'a, R>>,
    pub screens: Screens,
    pub iamf_mp4: bool,
    pub iamf_layers: Vec<Vec<u8>>,
}

/// A SIGINT handler installed for one render; dropping it restores the previous handler.
pub trait Interrupt: Sync {
    /// Whether Ctrl-C was pressed since the handler was installed.
    fn requested(&self) -> bool;
}

/// What the commands need from the engine. Error values are the messages the CLI prints.
pub trait Service {
    /// A loaded room package.
    type Room;
    type Interrupt: Interrupt;

    fn platform(&self, facts: Facts) -> Platform;
    /// `load_screen_config`.
    fn load_screens(&self, path: &[u8]) -> Result<Screens, Vec<u8>>;
    /// `RoomProfile::load`.
    fn load_room(&self, manifest: &[u8]) -> Result<Self::Room, Vec<u8>>;
    /// `RenderService::render`: `cancelled` is polled before every step and by the islands;
    /// `events` may be called from an island's threads.
    fn render(
        &self,
        request: &Request<'_, Self::Room>,
        cancelled: &(dyn Fn() -> bool + Sync),
        events: &(dyn Fn(Event<'_>) + Sync),
    ) -> Outcome;
    /// `RenderScene::inspect_text` of the imported input.
    fn inspect_text(&self, input: &[u8]) -> Result<Vec<u8>, Vec<u8>>;
    /// `io::get_axml`.
    fn axml(&self, input: &[u8]) -> Result<Vec<u8>, Vec<u8>>;
    /// `RenderService::write_policy_template`.
    fn write_policy_template(&self, input: &[u8], output: &[u8]) -> Result<(), Vec<u8>>;
    /// `RenderService::export_file`; `warning` receives the policy's log lines.
    fn export(
        &self,
        input: &[u8],
        output: &[u8],
        policy: Option<&[u8]>,
        warning: &mut dyn FnMut(&[u8]),
    ) -> Result<(), Vec<u8>>;
    /// `inspect_room`: the prepared room's report.
    fn inspect_room(
        &self,
        room: &Self::Room,
        hrir: &[u8],
        quality: u32,
    ) -> Result<Vec<u8>, Vec<u8>>;
    /// The APAC encode worker (`run_apac_encode_child`), writing its protocol lines on stdout;
    /// the exit code.
    fn apac_encode_child(
        &self,
        input: &[u8],
        output: &[u8],
        layout: &[u8],
        bitrate: u32,
        drc_music: bool,
        caf: bool,
    ) -> i32;
    /// Installs the SIGINT handler; `None` where it could not be installed.
    fn interrupt(&self) -> Option<Self::Interrupt>;
}

/// Runs the command line `args` (the program name first); the exit code.
pub fn run(service: &impl Service, args: Vec<OsString>) -> i32 {
    let parsed = match args::parse(args, VERSION) {
        Ok(parsed) => parsed,
        Err(exit) => {
            stdout(exit.stdout.as_bytes());
            stderr(exit.stderr.as_bytes());
            return exit.code;
        }
    };
    let (name, values) = parsed.command();
    let log = Logger::new(name == "render" && values.flag("--verbose", false));
    let text = |name: &str| values.text(name).unwrap_or_default();
    match name {
        "render" => render(service, &values, &log),
        "inspect" => inspect(
            service,
            &text("file"),
            values.flag("--xml", false),
            &text("--write-semantic-policy-template"),
            &log,
        ),
        "backends" => catalog::backends(service),
        "layouts" => catalog::layouts(
            service,
            &String::from_utf8_lossy(&text("--format")),
            &String::from_utf8_lossy(&text("--layout")),
            &String::from_utf8_lossy(&text("--renderer")),
        ),
        "input-layouts" => catalog::input_layouts(
            &String::from_utf8_lossy(&values.text_or("--format", "text")),
            &String::from_utf8_lossy(&text("--layout")),
        ),
        "formats" => catalog::formats(service),
        "export" => {
            let policy = values.text("--semantic-policy").filter(|p| !p.is_empty());
            let output = text("--output");
            let mut warning = |message: &[u8]| log.warn(message);
            match service.export(&text("--input"), &output, policy.as_deref(), &mut warning) {
                Ok(()) => {
                    log.info(&[b"wrote ", &output[..]].concat());
                    0
                }
                Err(message) => {
                    log.error(&message);
                    1
                }
            }
        }
        "room-check" => {
            let quality = u32::from(values.text_or("--quality", "exact") == b"realtime");
            let room = match service.load_room(&text("manifest")) {
                Ok(room) => room,
                Err(message) => {
                    log.error(&message);
                    return 1;
                }
            };
            match service.inspect_room(&room, &text("--hrir"), quality) {
                Ok(report) => {
                    stdout(&[&report[..], b"\n"].concat());
                    0
                }
                Err(message) => {
                    log.error(&message);
                    1
                }
            }
        }
        "__apac-encode" => service.apac_encode_child(
            &text("--in"),
            &text("--out"),
            &text("--layout"),
            values.uint("--bitrate").unwrap_or(0),
            values
                .text("--drc")
                .is_none_or(|v| convert::boolean(&v).unwrap_or(true)),
            values.text_or("--container", "mpeg4") == b"caf",
        ),
        _ => 0,
    }
}

fn inspect(service: &impl Service, input: &[u8], xml: bool, template: &[u8], log: &Logger) -> i32 {
    if xml && !template.is_empty() {
        log.error(b"--xml cannot be combined with --write-semantic-policy-template");
        return 1;
    }
    let result = if xml {
        service.axml(input)
    } else if !template.is_empty() {
        service
            .write_policy_template(input, template)
            .map(|()| Vec::new())
    } else {
        service
            .inspect_text(input)
            .map(|text| [b"File: ", input, b"\n", &text[..]].concat())
    };
    match result {
        Ok(text) if !xml && !template.is_empty() => {
            debug_assert!(text.is_empty());
            log.info(&[b"wrote ", template].concat());
            0
        }
        Ok(text) => {
            stdout(&text);
            0
        }
        Err(message) => {
            log.error(&message);
            1
        }
    }
}

/// `parse_input_channel_csv`: comma-separated labels, trimmed, empty items kept (none for "").
fn input_channels(csv: &[u8]) -> Vec<Vec<u8>> {
    if csv.is_empty() {
        return Vec::new();
    }
    csv.split(|&b| b == b',').map(trim).collect()
}

/// `parse_csv_list`: comma-separated items, trimmed, empty items dropped.
fn csv_list(csv: &[u8]) -> Vec<Vec<u8>> {
    csv.split(|&b| b == b',')
        .map(trim)
        .filter(|item| !item.is_empty())
        .collect()
}

fn trim(item: &[u8]) -> Vec<u8> {
    let blank = |b: &u8| matches!(b, b' ' | b'\t' | b'\r' | b'\n');
    match (
        item.iter().position(|b| !blank(b)),
        item.iter().rposition(|b| !blank(b)),
    ) {
        (Some(first), Some(last)) => item[first..=last].to_vec(),
        _ => Vec::new(),
    }
}

fn renderer(value: &[u8]) -> u32 {
    match value {
        b"ear" => 1,
        b"saf" => 2,
        b"hoa" => 3,
        b"apple" => 4,
        b"binaural" => 5,
        b"saf-binaural" => 6,
        b"triple-balance" => 7,
        b"room-binaural" => 8,
        _ => 0,
    }
}

fn index_of(value: &[u8], names: &[&str]) -> u32 {
    names
        .iter()
        .position(|n| n.as_bytes() == value)
        .map_or(0, |p| p as u32)
}

/// `std::lround(fraction * 100.0)` as the `int` the progress line printed.
fn percent(fraction: f64) -> i32 {
    let rounded = (fraction * 100.0).round();
    if rounded.is_nan() || rounded.abs() >= 9.2e18 {
        i64::MIN as i32
    } else {
        rounded as i64 as i32
    }
}

fn render(service: &impl Service, v: &args::Values<'_>, log: &Logger) -> i32 {
    let text = |name: &str| v.text(name).filter(|t| !t.is_empty());
    let input = v.text("--input").unwrap_or_default();
    let output = v.text("--output").filter(|o| !o.is_empty());
    let layout = v.text_or("--output-layout", "binaural");
    let input_layout = v.text("--input-layout").filter(|l| l != b"auto");
    let sofa = text("--sofa");
    let policy = text("--semantic-policy");
    let report = text("--write-semantic-report");
    let matrix = text("--direct-speakers-matrix");
    let loudness = v.f32("--loudness-target");
    let mut request: Request<'_, _> = Request {
        input: &input,
        output: output.as_deref(),
        renderer: renderer(&v.text_or("--renderer", "auto")),
        output_layout: &layout,
        input_layout: input_layout.as_deref(),
        input_channel_labels: input_channels(&v.text_or("--input-channels", "")),
        measure_loudness: loudness.is_some(),
        loudness_target_lufs: loudness.unwrap_or(-23.0),
        peak_limit: !v.flag("--no-peak-limit", false),
        peak_limit_dbtp: v.f32("--peak-limit-dbtp").unwrap_or(-1.0),
        peak_normalize_to_limit: v.flag("--peak-normalize-to-limit", false),
        final_gain_db: v.f64("--final-gain-db").unwrap_or(0.0),
        output_bit_depth: index_of(
            &v.text_or("--output-bit-depth", "f32"),
            &["f32", "i24", "i16"],
        ),
        opus_bitrate_per_ch_kbps: v.uint("--opus-bitrate-per-ch").unwrap_or(0),
        apac_bitrate_kbps: v.uint("--apac-bitrate").unwrap_or(0),
        apac_drc_music: v.flag("--apac-drc-none", true),
        apac_caf: v.text_or("--apac-container", "mpeg4") == b"caf",
        sofa: sofa.as_deref(),
        semantic_policy: policy.as_deref(),
        semantic_report: report.as_deref(),
        default_interp_ms: v.uint("--interp-ms").unwrap_or(5),
        object_smoothing_frames: v.uint("--object-smoothing-frames").unwrap_or(0),
        apple_geometry: v.text_or("--speaker-geometry", "standard") == b"apple",
        routing: index_of(
            &v.text_or("--direct-speakers-routing", "auto"),
            &["auto", "label", "position", "matrix"],
        ),
        matrix: matrix.as_deref(),
        speaker_spread: index_of(
            &v.text_or("--speaker-spread-mode", "auto"),
            &["auto", "none", "mdap"],
        ),
        triple_balance_mode: u32::from(v.text_or("--triple-balance-mode", "standard") == b"d"),
        binaural_spread: index_of(
            &v.text_or("--binaural-spread-mode", "auto"),
            &["auto", "none", "cloud", "saf-spreader"],
        ),
        split_power: v.text_or("--lfe-routing", "direct") == b"split-power",
        apple_spatial_preset: index_of(
            &v.text_or("--apple-spatial-preset", "off"),
            &["off", "headphone-default", "headphone-movie"],
        ),
        apple_speaker_rendering_flags: v.flag("--apple-speaker-rendering-flags", false),
        listener_orientation: [
            v.f32("--listener-yaw").unwrap_or(0.0),
            v.f32("--listener-pitch").unwrap_or(0.0),
            v.f32("--listener-roll").unwrap_or(0.0),
        ],
        render_start_sec: v.f64("--start").unwrap_or(0.0),
        render_end_sec: v.f64("--end"),
        room: None,
        screens: Screens::default(),
        iamf_mp4: v.text_or("--iamf-container", "obu") == b"mp4",
        iamf_layers: csv_list(&v.text_or("--iamf-layers", "")),
    };
    if let Some(path) = text("--screen-config") {
        match service.load_screens(&path) {
            Ok(screens) => request.screens = screens,
            Err(message) => {
                log.error(&message);
                return 1;
            }
        }
    }
    let package;
    let hrir = v.text_or("--room-hrir", "");
    if let Some(path) = text("--room") {
        if request.renderer != 8 {
            log.error(b"--room requires --renderer room-binaural");
            return 1;
        }
        package = match service.load_room(&path) {
            Ok(package) => package,
            Err(message) => {
                log.error(&message);
                return 1;
            }
        };
        request.room = Some(Room {
            package: &package,
            hrir: &hrir,
            metres: v.f64("--room-metres-per-unit").unwrap_or(0.0),
            quality: u32::from(v.text_or("--room-quality", "exact") == b"realtime"),
        });
    }
    let events = |event: Event<'_>| match event {
        Event::Progress { fraction, message } => log.debug(
            &[
                format!("progress {}%: ", percent(fraction)).as_bytes(),
                message,
            ]
            .concat(),
        ),
        Event::Log {
            level,
            module,
            message,
        } => {
            if let Some(level) = Level::of(level) {
                log.log(level, &[b"[", module, b"] ", message].concat());
            }
        }
    };
    let cancel = AtomicBool::new(false);
    let done = AtomicBool::new(false);
    let interrupt = service.interrupt();
    if interrupt.is_none() {
        log.warn(b"Ctrl-C cancellation is unavailable: failed to install SIGINT handler");
    }
    let outcome = std::thread::scope(|scope| {
        if let Some(interrupt) = &interrupt {
            scope.spawn(|| {
                while !done.load(Ordering::Acquire) {
                    if interrupt.requested() {
                        log.info(b"Ctrl-C received; cancelling render...");
                        cancel.store(true, Ordering::Release);
                        return;
                    }
                    std::thread::sleep(Duration::from_millis(25));
                }
            });
        }
        let outcome = service.render(&request, &|| cancel.load(Ordering::Acquire), &events);
        done.store(true, Ordering::Release);
        outcome
    });
    drop(interrupt);
    if outcome.code != 0 {
        if outcome.code == 5 {
            log.info(&outcome.message);
            return 130;
        }
        log.error(&outcome.message);
        return 1;
    }
    if let Some(path) = outcome.output_path {
        log.info(&[&b"wrote "[..], &path].concat());
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn csv_lists_split_as_the_cpp_did() {
        assert_eq!(input_channels(b""), Vec::<Vec<u8>>::new());
        assert_eq!(
            input_channels(b" L ,,R"),
            [b"L".to_vec(), vec![], b"R".to_vec()]
        );
        assert_eq!(input_channels(b","), [Vec::<u8>::new(), Vec::new()]);
        assert_eq!(
            csv_list(b"stereo, 5.1 ,,"),
            [b"stereo".to_vec(), b"5.1".to_vec()]
        );
        assert_eq!(csv_list(b""), Vec::<Vec<u8>>::new());
    }

    #[test]
    fn progress_rounds_half_away_from_zero() {
        assert_eq!(percent(0.125), 13);
        assert_eq!(percent(1.0), 100);
        assert_eq!(percent(0.0), 0);
    }
}
