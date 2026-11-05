//! `mr_adm_phase2_stream`: release replay of file-streaming renders (PCM consistency 3.1–4.9; in
//! Rust since 5.3 B, formerly `tests/tools/phase2_stream.cpp`). The backend comes from the same
//! renderer resolution and preparation as the monitor; its file stream is pulled in a fixed request
//! partition while live overrides, head orientations and seeks land at fixed output frames, so the
//! delivered PCM is a deterministic function of the case file. Only error-level log lines reach
//! stderr, as `module: message`.
use mradm_dsp::room::loading::{self, Package};
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{
    self, BinauralSpreadMode, Kind, RoomRequest, Routing, Screens, SpeakerSpread,
};
use mradm_engine::request::{self, Level, Selection};
use mradm_engine::stream::{self, Body, Settings, Validator};
use mradm_ffi::FfiHost;
use mradm_render::binaural::Spread;
use mradm_render::routing::{LiveOverride, Override};
use mradm_scene::RenderScene;
use serde_json::Value;
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;

#[path = "phase2/io.rs"]
mod phase2_io;
use phase2_io::{Failure, pcm, require};

fn renderer(name: &str) -> Result<Selection, Failure> {
    Ok(match name {
        "ear" => Selection::Ear,
        "hoa" => Selection::Hoa,
        "saf" => Selection::Saf,
        "saf-binaural" => Selection::SafBinaural,
        "triple-balance" => Selection::TripleBalance,
        "room-binaural" => Selection::RoomBinaural,
        _ => return Err("unsupported file-streaming renderer".into()),
    })
}

fn spread_mode(name: &str) -> Result<BinauralSpreadMode, Failure> {
    Ok(match name {
        "none" => BinauralSpreadMode::None,
        "cloud" => BinauralSpreadMode::Cloud,
        "saf-spreader" => BinauralSpreadMode::SafSpreader,
        _ => return Err("unknown binaural spread mode".into()),
    })
}

fn kind(backend: Selection) -> Kind {
    match backend {
        Selection::Ear => Kind::Ear,
        Selection::Hoa => Kind::Hoa,
        Selection::Saf => Kind::Vbap,
        Selection::TripleBalance => Kind::TripleBalance,
        Selection::RoomBinaural => Kind::Room,
        _ => Kind::Binaural,
    }
}

fn u64_of(v: &Value, key: &str) -> Result<u64, Failure> {
    v.get(key)
        .and_then(Value::as_u64)
        .ok_or_else(|| format!("case field {key} is missing or not an unsigned integer"))
}

fn f32_or(v: &Value, key: &str, default: f32) -> f32 {
    v.get(key)
        .and_then(Value::as_f64)
        .map_or(default, |x| x as f32)
}

/// One event's `objects` list replaces the whole live-override snapshot.
fn overrides<'a>(
    objects: &[Value],
    ids: &'a [String],
    labels: &'a [String],
) -> Result<Vec<LiveOverride<'a>>, Failure> {
    objects
        .iter()
        .zip(labels)
        .map(|(row, label)| {
            let index = row
                .get("index")
                .and_then(Value::as_u64)
                .ok_or("override needs an object index")?;
            let index = usize::try_from(index).unwrap_or(usize::MAX);
            require(index < ids.len(), "override object index out of range")?;
            Ok(LiveOverride {
                base: Override {
                    object_id: ids[index].as_bytes(),
                    speaker_label: label.as_bytes(),
                    mute: row.get("mute").and_then(Value::as_bool).unwrap_or(false),
                    gain_db: f32_or(row, "gain_db", 0.),
                    head_locked: None,
                },
                extent_scale: f32_or(row, "extent_scale", 1.),
                extent_width_scale: f32_or(row, "width_scale", 1.),
                extent_height_scale: f32_or(row, "height_scale", 1.),
                extent_depth_scale: f32_or(row, "depth_scale", 1.),
                diffuse_scale: f32_or(row, "diffuse_scale", 1.),
                divergence_scale: f32_or(row, "divergence_scale", 1.),
            })
        })
        .collect()
}

/// Version 4: the room pack at the exact tier (relative, normalised, not leaving the fixtures).
fn room_package(spec: &Value, fixtures: &Path) -> Result<Arc<Package>, Failure> {
    let name = spec
        .get("room")
        .and_then(Value::as_str)
        .ok_or("invalid room fixture name")?;
    let path = Path::new(name);
    require(
        path.is_relative()
            && !name.is_empty()
            && !name.ends_with('/')
            && !name.contains("//")
            && path.components().all(|c| matches!(c, Component::Normal(_)))
            && name.split('/').all(|part| part != "."),
        "invalid room fixture name",
    )?;
    let manifest = fixtures.join(path);
    loading::load_package(manifest.to_string_lossy().as_bytes())
        .map(Arc::new)
        .map_err(|_| "cannot load the room fixture".to_owned())
}

/// The object ids in scene order, as `AdmScene::objects`.
fn object_ids(scene: &RenderScene) -> Vec<String> {
    let s = &scene.snapshot;
    s.objects
        .iter()
        .map(|o| s.strings[o.id as usize].clone())
        .collect()
}

fn replay(
    spec: &Value,
    input: &Path,
    fixtures: &Path,
    scene: &RenderScene,
) -> Result<(Vec<f32>, u32), Failure> {
    // Version 4 (PCM consistency 4.2) adds a room pack and the tail the room appends after EOS;
    // version 5 (4.9) adds object smoothing.
    let version = spec.get("version").and_then(Value::as_i64).unwrap_or(0);
    require(
        (3..=5).contains(&version),
        "unsupported file-streaming replay version",
    )?;
    require(
        (version == 4) == (spec.get("room").is_some() && spec.get("tail_frames").is_some()),
        "room replays are version 4 and name their room pack and tail",
    )?;
    require(
        (version == 5) == spec.get("object_smoothing_frames").is_some(),
        "smoothed replays are version 5 and name their smoothing length",
    )?;
    let info = &scene.info;
    require(
        info.frames == u64_of(spec, "input_frames")?,
        "file-streaming input frame count differs from the case",
    )?;
    require(
        u64::from(info.sample_rate) == u64_of(spec, "sample_rate")?,
        "file-streaming input sample rate differs from the case",
    )?;
    let layout = spec
        .get("output_layout")
        .and_then(Value::as_str)
        .ok_or("case has no output layout")?;
    let selection = renderer(
        spec.get("renderer")
            .and_then(Value::as_str)
            .ok_or("case has no renderer")?,
    )?;
    let resolved = request::resolve_renderer(
        selection,
        &request::normalize_output_layout(layout),
        false,
        false,
    )
    .map_err(|_| "cannot resolve the file-streaming renderer")?;
    let binaural_spread = match spec.get("spread") {
        Some(s) => spread_mode(s.as_str().unwrap_or(""))?,
        None => BinauralSpreadMode::Automatic,
    };
    let smoothing = if version == 5 {
        u32::try_from(u64_of(spec, "object_smoothing_frames")?)
            .map_err(|_| "invalid smoothing length")?
    } else {
        0
    };
    let (package, tail) = if version == 4 {
        (
            Some(room_package(spec, fixtures)?),
            u64_of(spec, "tail_frames")?,
        )
    } else {
        (None, 0)
    };
    let media_frames = info.frames + tail;
    let input_text = input.to_string_lossy().into_owned();
    // RenderPlan's defaults: no screens (the C++ ScreenGeometry defaults), 5 ms interpolation.
    let screen = RoomScreenGeometry {
        cartesian: 0,
        reserved: 0,
        aspect_ratio: 1.78,
        centre: [0., 0., 1.],
        width: 58.,
    };
    let request = backend::Request {
        kind: kind(resolved.backend),
        layout_id: resolved.effective_output_layout.as_bytes(),
        apple_geometry: false,
        routing: Routing::Automatic,
        matrix: None,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread,
        triple_balance_d: false,
        split_power: false,
        smoothing_frames: smoothing,
        default_interp_ms: 5,
        input_path: input_text.as_bytes(),
        sofa: None,
        screens: Screens {
            has_reference: false,
            reference: screen,
            has_reproduction: false,
            reproduction: screen,
        },
        room: RoomRequest {
            package,
            hrir: b"",
            metres: 0.,
            quality: 0,
        },
    };
    let preparation = backend::prepare(scene, &request, &FfiHost);
    for line in &preparation.outcome.logs {
        if line.level == Level::Error {
            eprintln!("{}: {}", line.module, line.message);
        }
    }
    let prepared = preparation
        .outcome
        .result
        .map_err(|_| "prepare failed".to_owned())?;
    let settings = Settings {
        path: &input_text,
        channels: usize::from(info.channels),
        frames: info.frames,
        sample_rate: info.sample_rate,
        default_interp_ms: 5,
        smoothing_frames: smoothing,
        // `numerical_spread` of the plan's mode: none 1, SAF spreader 2, otherwise the cloud.
        spread: match binaural_spread {
            BinauralSpreadMode::None => Spread::None,
            BinauralSpreadMode::SafSpreader => Spread::SafSpreader,
            _ => Spread::Cloud,
        },
        pose: [0.; 3],
    };
    let (validator, mut body, stream_info) =
        stream::open(&prepared, &settings, &FfiHost).map_err(|_| "open_stream failed")?;
    let channels = stream_info.outputs;
    require(
        channels as u64 == u64_of(spec, "channels")?,
        "stream channel count differs from the case",
    )?;
    let sample_rate = stream_info.sample_rate;
    require(
        sample_rate == info.sample_rate,
        "file-streaming output sample rate differs from the input",
    )?;

    let partition: Vec<usize> = spec
        .get("partition")
        .and_then(Value::as_array)
        .and_then(|p| {
            p.iter()
                .map(|n| n.as_u64().map(|n| n as usize))
                .collect::<Option<Vec<_>>>()
        })
        .unwrap_or_default();
    require(
        !partition.is_empty() && partition.iter().all(|&n| n != 0),
        "invalid request partition",
    )?;
    let empty = Vec::new();
    let events = spec
        .get("events")
        .and_then(Value::as_array)
        .unwrap_or(&empty);
    let frame_of = |e: &Value| u64_of(e, "frame");
    let total = u64_of(spec, "frames")?;
    let ids = object_ids(scene);
    let mut delivered = Vec::new();
    let mut block = Vec::new();
    let (mut position, mut media_position) = (0u64, 0u64);
    let (mut next_event, mut request) = (0usize, 0usize);
    while position < total {
        while next_event < events.len() && frame_of(&events[next_event])? == position {
            let event = &events[next_event];
            next_event += 1;
            if let Some(objects) = event.get("objects") {
                let objects = objects.as_array().ok_or("invalid override list")?;
                let labels: Vec<String> = objects
                    .iter()
                    .map(|o| {
                        o.get("speaker_label")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned()
                    })
                    .collect();
                let snapshot = overrides(objects, &ids, &labels)?;
                require(validate(&validator, &snapshot), "override rejected")?;
                if body.set_overrides(&snapshot).is_err() {
                    std::process::abort();
                }
            }
            if let Some(o) = event.get("orientation") {
                let angles: Vec<f32> = o
                    .as_array()
                    .map(|a| {
                        a.iter()
                            .filter_map(Value::as_f64)
                            .map(|x| x as f32)
                            .collect()
                    })
                    .unwrap_or_default();
                require(angles.len() == 3, "orientation needs yaw, pitch and roll")?;
                body.set_pose([angles[0], angles[1], angles[2]]);
            }
            if event.get("seek").is_some() {
                let target = u64_of(event, "seek")?;
                require(seek(&mut body, target), "seek failed")?;
                media_position = target.min(media_frames);
            }
        }
        require(
            next_event == events.len() || frame_of(&events[next_event])? > position,
            "events must be ordered by output frame",
        )?;
        let limit = if next_event < events.len() {
            total.min(frame_of(&events[next_event])?)
        } else {
            total
        };
        let frames = (partition[request % partition.len()] as u64).min(limit - position) as usize;
        request += 1;
        block.clear();
        block.resize(frames * channels, 0.);
        let produced = body
            .process(&mut block, frames)
            .map_err(|_| "stream process failed")?;
        // Check the media timeline independently of the stream's reported progress. In particular,
        // a seek that reports success without moving must not pass as a shorter, repeatable replay.
        let expected = (frames as u64).min(media_frames - media_position);
        require(
            produced as u64 == expected,
            "file-streaming frame count differs from replay timeline",
        )?;
        delivered.extend_from_slice(&block[..produced * channels]);
        position += produced as u64;
        media_position += produced as u64;
        if produced < frames {
            break; // end of material
        }
    }
    require(
        next_event == events.len(),
        "replay ended before every event was applied",
    )?;
    require(
        body.tail_frames() == tail,
        "room tail differs from the case",
    )?;
    Ok((delivered, sample_rate))
}

fn validate(validator: &Validator, overrides: &[LiveOverride]) -> bool {
    validator.validate(overrides).is_ok()
}

/// `IRenderStream::seek`: never cancelled.
fn seek(body: &mut Body, frame: u64) -> bool {
    let Ok(mut pending) = body.seek_begin(frame, false) else {
        return false;
    };
    while pending {
        match body.seek_step(false) {
            Ok(more) => pending = more,
            Err(_) => return false,
        }
    }
    true
}

fn run(args: &[PathBuf]) -> Result<(), Failure> {
    require(
        args.len() == 3,
        "usage: mr_adm_phase2_stream case.json input.wav output-directory",
    )?;
    let (case, input, directory) = (&args[0], &args[1], &args[2]);
    let spec: Value = serde_json::from_slice(&fs::read(case).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    fs::create_dir_all(directory).map_err(|e| e.to_string())?;
    let scene = mradm_scene::import_adm(input.to_string_lossy().as_bytes())
        .map_err(|_| "cannot import the replay fixture")?;
    let fixtures = case
        .parent()
        .and_then(Path::parent)
        .unwrap_or(Path::new(""))
        .join("fixtures");
    let channels = u32::try_from(u64_of(&spec, "channels")?).map_err(|_| "invalid channels")?;
    for pass in 1..=2 {
        let (values, sample_rate) = replay(&spec, input, &fixtures, &scene)?;
        pcm(
            &directory.join(format!("pass-{pass}.pcmbits")),
            channels,
            sample_rate,
            &values,
        )?;
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
