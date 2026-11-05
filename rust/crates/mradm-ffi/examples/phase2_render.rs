//! Repeat the manifest's exact offline render requests twice in one process (PCM consistency
//! 5.2 D, formerly `tests/tools/phase2_render.cpp`). The request is the one the C++
//! `RenderService` built from the same `RenderOptions` fields; the render runs on the production
//! engine host. Outputs are WAV or CAF only, so no encoder or Apple island is ever requested.
use mradm_dsp::decimal;
use mradm_dsp::room::loading;
use mradm_dsp::room::screen::{RoomScreenGeometry, RoomScreens};
use mradm_engine::backend::{BinauralSpreadMode, Host, Screens, SpeakerSpread};
use mradm_engine::render::{Event, PathText, Render, Request, Reuse, RoomOptions, SofaPath};
use mradm_engine::request::{BitDepth, DsRouting, Selection};
use mradm_ffi::FfiHost;
use mradm_wav::{Reader, SampleFormat, caf};
use serde_json::Value;
use std::fs::{self, File};
use std::io::{BufReader, Read, Write};
use std::path::{Component, Path, PathBuf};
use std::process::ExitCode;

#[path = "phase2/io.rs"]
mod phase2_io;
use phase2_io::{Failure, pcm, require};

// The same locale-independent, correctly rounded parser as the mradm command line (3.5).
fn f64_of(text: &str) -> Result<f64, Failure> {
    decimal::parse_f64(text).ok_or_else(|| format!("invalid decimal option value: {text}"))
}
fn f32_of(text: &str) -> Result<f32, Failure> {
    decimal::parse_f32(text).ok_or_else(|| format!("invalid decimal option value: {text}"))
}

fn text(path: &Path) -> PathText {
    let utf8 = path.to_string_lossy().into_owned();
    PathText {
        display: utf8.clone(),
        utf8,
    }
}

/// `RenderOptions` with the tool's float32 default, as `RequestTexts::record` passes it
/// (defaults: `include/adm/options.h` `RenderOptions` and `include/adm/room.h` `RoomConfig`).
fn request(input: &Path, output: &Path) -> Request {
    Request {
        input: text(input),
        output: Some(text(output)),
        renderer: Selection::Automatic as u32,
        output_layout: "binaural".to_owned(),
        input_layout: None,
        input_channel_labels: Vec::new(),
        measure_loudness: false,
        loudness_target_lufs: -23.,
        peak_limit: true,
        peak_limit_dbtp: -1.,
        peak_normalize_to_limit: false,
        final_gain_db: 0.,
        output_bit_depth: BitDepth::F32 as u32,
        opus_bitrate_per_ch_kbps: 0,
        apac_bitrate_kbps: 0,
        apac_drc_music: true,
        apac_caf: false,
        sofa: None,
        semantic_policy_path: None,
        semantic_policy_json: None,
        matrix_path: None,
        matrix_json: None,
        semantic_report_path: None,
        capture_semantic_report: false,
        default_interp_ms: 5,
        object_smoothing_frames: 0,
        apple_geometry: false,
        routing: DsRouting::Automatic as u32,
        speaker_spread: SpeakerSpread::Automatic as u32,
        // `TripleBalanceMode::standard`.
        triple_balance_mode: 0,
        binaural_spread: BinauralSpreadMode::Automatic as u32,
        split_power: false,
        // `AppleSpatialPreset::off`.
        apple_spatial_preset: 0,
        apple_speaker_rendering_flags: false,
        listener_orientation: [0.; 3],
        render_start_sec: 0.,
        render_end_sec: None,
        room: RoomOptions {
            package: None,
            hrir: Vec::new(),
            metres: 0.,
            // `RoomQuality::exact`.
            quality: 0,
        },
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry::default(),
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        internal_allow_speaker_stereo: false,
        iamf_mp4: false,
        iamf_layers: Vec::new(),
    }
}

fn apply_args(args: &Value, r: &mut Request) -> Result<(), Failure> {
    let values = args
        .as_array()
        .ok_or("case args must be an array")?
        .iter()
        .map(|v| {
            v.as_str()
                .map(str::to_owned)
                .ok_or("case args must be strings")
        })
        .collect::<Result<Vec<_>, _>>()?;
    let mut i = 0;
    while i < values.len() {
        let key = values[i].as_str();
        i += 1;
        if key == "--no-peak-limit" {
            r.peak_limit = false;
            continue;
        }
        if key == "--peak-normalize-to-limit" {
            r.peak_normalize_to_limit = true;
            continue;
        }
        require(i < values.len(), "missing option value")?;
        let v = values[i].as_str();
        i += 1;
        match key {
            "--renderer" => {
                r.renderer = match v {
                    "ear" => Selection::Ear,
                    "saf" => Selection::Saf,
                    "saf-binaural" => Selection::SafBinaural,
                    "hoa" => Selection::Hoa,
                    "triple-balance" => Selection::TripleBalance,
                    "room-binaural" => Selection::RoomBinaural,
                    _ => return Err("map::at".to_owned()),
                } as u32;
            }
            "--output-layout" => r.output_layout = v.to_owned(),
            "--object-smoothing-frames" => {
                r.object_smoothing_frames = v.parse().map_err(|_| "stoul".to_owned())?;
            }
            "--start" => r.render_start_sec = f64_of(v)?,
            "--end" => r.render_end_sec = Some(f64_of(v)?),
            "--speaker-spread-mode" if v == "none" => {
                r.speaker_spread = SpeakerSpread::None as u32;
            }
            "--binaural-spread-mode" => {
                require(
                    v == "cloud" || v == "saf-spreader",
                    "unsupported spread mode",
                )?;
                r.binaural_spread = if v == "cloud" {
                    BinauralSpreadMode::Cloud
                } else {
                    BinauralSpreadMode::SafSpreader
                } as u32;
            }
            "--listener-yaw" => r.listener_orientation[0] = f32_of(v)?,
            "--listener-pitch" => r.listener_orientation[1] = f32_of(v)?,
            "--listener-roll" => r.listener_orientation[2] = f32_of(v)?,
            "--loudness-target" => {
                r.measure_loudness = true;
                r.loudness_target_lufs = f32_of(v)?;
            }
            "--peak-limit-dbtp" => r.peak_limit_dbtp = f32_of(v)?,
            "--final-gain-db" => r.final_gain_db = f64_of(v)?,
            "--output-bit-depth" => {
                r.output_bit_depth = match v {
                    "f32" => BitDepth::F32,
                    "i24" => BitDepth::I24,
                    "i16" => BitDepth::I16,
                    _ => return Err("map::at".to_owned()),
                } as u32;
            }
            "--direct-speakers-routing" => {
                require(v == "matrix", "unsupported DirectSpeakers routing")?;
                r.routing = DsRouting::Matrix as u32;
            }
            "--room-quality" => {
                require(v == "exact" || v == "realtime", "unsupported room quality")?;
                r.room.quality = u32::from(v == "realtime");
            }
            "--triple-balance-mode" => {
                require(
                    v == "standard" || v == "d",
                    "unsupported Triple Balance mode",
                )?;
                r.triple_balance_mode = u32::from(v == "d");
            }
            _ => return Err(format!("unsupported measurement option: {key}")),
        }
    }
    Ok(())
}

/// `load_screen_config`: at most 1 MiB, parsed and validated as `parse_screen_config_json`.
fn screens(path: &Path) -> Option<Screens> {
    let mut data = Vec::new();
    File::open(path)
        .ok()?
        .take((1 << 20) + 1)
        .read_to_end(&mut data)
        .ok()?;
    let parsed = mradm_scene::screen::parse(&data).ok()?;
    let reference = parsed.reference.unwrap_or_default();
    let check = RoomScreens {
        enabled: 1,
        reserved: 0,
        reference,
        reproduction: parsed.reproduction.unwrap_or(reference),
    };
    FfiHost.validate_screens(&check).ok()?;
    Some(Screens {
        has_reference: parsed.reference.is_some(),
        reference,
        has_reproduction: parsed.reproduction.is_some(),
        reproduction: parsed.reproduction.unwrap_or_default(),
    })
}

// Fixture files named by the case (4.2): the same request fields the mradm command line fills
// from these flags, resolved inside the collection's fixtures directory.
fn apply_files(files: &Value, fixtures: &Path, r: &mut Request) -> Result<(), Failure> {
    for (flag, value) in files.as_object().ok_or("case files must be an object")? {
        let name = Path::new(value.as_str().ok_or("case file names must be strings")?);
        let normal = name.components().all(|c| matches!(c, Component::Normal(_)));
        require(
            !name.as_os_str().is_empty() && normal,
            "invalid fixture file name",
        )?;
        let path = fixtures.join(name);
        match flag.as_str() {
            "--semantic-policy" => r.semantic_policy_path = Some(text(&path)),
            "--direct-speakers-matrix" => r.matrix_path = Some(text(&path)),
            "--screen-config" => {
                r.screens = screens(&path).ok_or("cannot load screen configuration")?;
            }
            "--room" => {
                let package = loading::load_package(text(&path).utf8.as_bytes())
                    .map_err(|_| "cannot load room profile".to_owned())?;
                r.room.package = Some(package.into());
            }
            "--room-hrir" => r.room.hrir = text(&path).utf8.into_bytes(),
            _ => return Err(format!("unsupported fixture file option: {flag}")),
        }
    }
    Ok(())
}

/// One render on the production host; the engine's log lines go to stdout as the C++ log sink
/// printed them, progress is discarded.
fn render(request: Request) -> Result<(), Failure> {
    let mut render = Render::new(request, base_platform(), Reuse::default());
    let mut out = std::io::stdout().lock();
    loop {
        match render.next(false, &FfiHost) {
            Event::Log(l) => {
                writeln!(out, "{}: {}", l.module, l.message).map_err(|e| e.to_string())?;
            }
            Event::Progress(_) | Event::Continue => {}
            Event::Island(_) => return Err("unexpected render island".to_owned()),
            Event::Done => break,
        }
    }
    out.flush().map_err(|e| e.to_string())?;
    match render.outcome().and_then(|o| o.error.as_ref()) {
        Some(error) => Err(error.message.clone()),
        None => Ok(()),
    }
}

/// The C++ `engine::RenderPlatform` without the Apple backend: the tool's renderers and WAV/CAF
/// outputs read only the SOFA build switch.
fn base_platform() -> mradm_engine::catalog::Platform {
    mradm_engine::catalog::Platform {
        sofa: cfg!(feature = "sofa"),
        ..Default::default()
    }
}

// The final file's samples as float32. Integer WAV must keep the requested depth; 16- and 24-bit
// samples convert to float32 exactly, so equal floats mean equal integers.
fn samples_caf(path: &Path) -> Result<(u32, u32, Vec<f32>), Failure> {
    let mut reader = caf::Reader::open(path).map_err(|_| "cannot open repeat output")?;
    let mut values =
        vec![0.; usize::try_from(reader.frame_count()).unwrap() * reader.channels() as usize];
    let got = reader.read(&mut values).map_err(|e| e.to_string())?;
    require(got == reader.frame_count(), "short repeat output")?;
    Ok((reader.channels(), reader.sample_rate(), values))
}

fn samples_wav(path: &Path, depth: u32) -> Result<(u32, u32, Vec<f32>), Failure> {
    let file = File::open(path).map_err(|_| "cannot open repeat output")?;
    let mut reader = Reader::new(BufReader::new(file)).map_err(|_| "cannot open repeat output")?;
    let format = reader.info().format;
    let floating = depth == BitDepth::F32 as u32;
    let bits = match depth {
        d if d == BitDepth::I16 as u32 => 16,
        d if d == BitDepth::I24 as u32 => 24,
        _ => 32,
    };
    require(
        (format.sample_format == SampleFormat::Float32) == floating
            && format.sample_format.bits() == bits,
        "repeat output has the requested sample format",
    )?;
    let channels = u32::from(format.channels);
    let frames = reader.info().frames;
    let mut values = vec![0.; usize::try_from(frames).unwrap() * channels as usize];
    let got = reader.read_frames(&mut values).map_err(|e| e.to_string())?;
    require(got == frames, "short repeat output")?;
    Ok((channels, format.sample_rate, values))
}

fn run(args: &[PathBuf]) -> Result<(), Failure> {
    require(
        args.len() == 3,
        "usage: mr_adm_phase2_render case.json input.wav output-directory",
    )?;
    let (case, input, directory) = (&args[0], &args[1], &args[2]);
    let spec: Value = serde_json::from_slice(&fs::read(case).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    fs::create_dir_all(directory).map_err(|e| e.to_string())?;
    let fixtures = case
        .parent()
        .and_then(Path::parent)
        .unwrap_or(Path::new(""))
        .join("fixtures");
    for pass in 1..=2 {
        // Optional final container: "caf" writes float32 CAF instead of WAV (3.6).
        let caf = spec.get("container").and_then(Value::as_str) == Some("caf");
        let output = directory.join(format!("pass-{pass}.{}", if caf { "caf" } else { "wav" }));
        let mut r = request(input, &output);
        apply_args(spec.get("args").ok_or("case has no args")?, &mut r)?;
        if let Some(sofa) = spec.get("sofa") {
            let name = Path::new(sofa.as_str().ok_or("invalid SOFA fixture name")?);
            require(
                name.file_name() == Some(name.as_os_str())
                    && name.extension().is_some_and(|e| e == "sofa"),
                "invalid SOFA fixture name",
            )?;
            let path = fixtures.join(name);
            r.sofa = Some(SofaPath {
                file_name: name.to_string_lossy().into_owned().into_bytes(),
                path: text(&path),
            });
        }
        if let Some(files) = spec.get("files") {
            apply_files(files, &fixtures, &mut r)?;
        }
        let depth = r.output_bit_depth;
        render(r)?;
        let (channels, rate, values) = if caf {
            samples_caf(&output)?
        } else {
            samples_wav(&output, depth)?
        };
        pcm(
            &directory.join(format!("pass-{pass}.pcmbits")),
            channels,
            rate,
            &values,
        )?;
        fs::remove_file(&output).map_err(|e| e.to_string())?;
    }
    Ok(())
}

fn main() -> ExitCode {
    let args: Vec<PathBuf> = std::env::args_os().skip(1).map(PathBuf::from).collect();
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("{message}");
            ExitCode::from(2)
        }
    }
}
