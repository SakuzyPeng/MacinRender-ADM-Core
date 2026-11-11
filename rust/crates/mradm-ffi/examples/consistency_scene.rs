//! `mr_adm_consistency_scene`: sample-clock replay of the Scene stream worker and the device-bound
//! output DSP (PCM consistency 1.5–4.2; in Rust since 5.4 D, formerly
//! `tests/tools/phase2_scene.cpp`). A fixed control script (generations, pose, semantic policy,
//! backend switches, HpTF and volume) lands at fixed input samples under a manual clock, so the
//! delivered PCM of both epochs and both passes is a deterministic function of the case file.
//! Errors go to stderr with exit status 2; a passing replay prints nothing.
use mradm_dsp::hptf::{self, Band, BandKind, PreampMode, StereoSnapshot};
use mradm_dsp::room::loading;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, RoomRequest, Screens, Sofa, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::output::{DeviceProgress, Session};
use mradm_engine::scene::renderer;
use mradm_engine::scene::stream::{
    Config, Descriptor, Entity, FrameInput, Identity, PlaneView, Stream, Submit, frame_flag,
    pull_flag,
};
use mradm_ffi::FfiHost;
use mradm_json::{Map, Value};
use mradm_render::live::{self, field};
use mradm_render::routing::Error;
use std::path::{Component, Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;
use std::time::{Duration, Instant};

#[path = "consistency/io.rs"]
mod consistency_io;
use consistency_io::{Failure, pcm, require};

const SIGNAL_SEED: u32 = 0x1234_5678;
const MAIN_EPOCH_END: u32 = 12289;
const SEEK_TARGET: u32 = 257;
const SEEK_END: u32 = 290;

fn signal_scale(device_dsp: bool) -> f32 {
    if device_dsp { 4.0 } else { 0.125 }
}

/// `phase2::signal`: the lcg32-v1 generator.
fn signal(size: usize, mut seed: u32, scale: f32) -> Vec<f32> {
    (0..size)
        .map(|_| {
            seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            ((seed >> 8) as i32 - 8_388_608) as f32 / 8_388_608.0 * scale
        })
        .collect()
}

fn take<T>(result: Result<T, Error>) -> Result<T, Failure> {
    result.map_err(|e| String::from_utf8_lossy(&e.message).into_owned())
}

// The nlohmann accessors the C++ tool used, with their exception texts.
fn at<'a>(value: &'a Value, key: &str) -> Result<&'a Value, Failure> {
    mradm_json::at(value, key)
}
fn contains(value: &Value, key: &str) -> bool {
    value.as_object().is_some_and(|m| m.contains_key(key))
}
fn string(value: &Value) -> Result<&str, Failure> {
    value
        .as_str()
        .ok_or_else(|| mradm_json::type_must_be("string", value))
}
fn boolean(value: &Value) -> Result<bool, Failure> {
    value
        .as_bool()
        .ok_or_else(|| mradm_json::type_must_be("boolean", value))
}
fn number<T>(value: &Value, convert: impl Fn(f64, i128) -> T) -> Result<T, Failure> {
    match *value {
        Value::Unsigned(u) => Ok(convert(u as f64, i128::from(u))),
        Value::Integer(i) => Ok(convert(i as f64, i128::from(i))),
        Value::Float(f) => Ok(convert(f, f as i128)),
        Value::Bool(b) => Ok(convert(f64::from(u8::from(b)), i128::from(b))),
        _ => Err(mradm_json::type_must_be("number", value)),
    }
}
fn u32_of(value: &Value) -> Result<u32, Failure> {
    number(value, |_, i| i as u32)
}
fn i32_of(value: &Value) -> Result<i32, Failure> {
    number(value, |_, i| i as i32)
}
fn f32_of(value: &Value) -> Result<f32, Failure> {
    match value {
        Value::Bool(b) => Ok(f32::from(u8::from(*b))),
        _ => value
            .as_f32()
            .ok_or_else(|| mradm_json::type_must_be("number", value)),
    }
}
fn is(value: &Value, text: &str) -> bool {
    value.as_str() == Some(text)
}

/// nlohmann's `operator==`: numbers of any class compare by value.
fn json_eq(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y).all(|(p, q)| json_eq(p, q))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter()
                    .zip(y)
                    .all(|((k, p), (l, q))| k == l && json_eq(p, q))
        }
        _ if a.is_number() && b.is_number() => a.number_eq(b),
        _ => a == b,
    }
}

fn object(entries: Vec<(&str, Value)>) -> Value {
    Value::Object(
        entries
            .into_iter()
            .map(|(k, v)| (k.to_owned(), v))
            .collect::<Map>(),
    )
}

fn validate_spec(spec: &Value) -> Result<(), Failure> {
    // Version 1 describes a fixed control script. Reject divergent descriptions before writing
    // evidence; metadata, partition and renderer parameters are the fields the replay reads.
    require(
        contains(spec, "device_dsp") && at(spec, "device_dsp")?.as_bool().is_some(),
        "invalid replay device DSP flag",
    )?;
    let device_dsp = boolean(at(spec, "device_dsp")?)?;
    let backend = at(spec, "backend")?;
    let triple = is(backend, "triple-balance");
    let sofa = contains(spec, "sofa");
    require(
        !sofa || is(backend, "binaural"),
        "SOFA replay requires binaural rendering",
    )?;
    let room = is(backend, "room-binaural");
    require(
        room == contains(spec, "room") && room == contains(spec, "tail_frames"),
        "room replay names its room pack and tail",
    )?;
    let mut version: i64 = if triple { 2 } else { 1 };
    let mut switch_control = if triple {
        "switch speaker backend"
    } else {
        "switch stereo backend"
    };
    if sofa {
        version = 3;
        switch_control = "switch SOFA to built-in KEMAR";
    }
    // Version 4: device DSP with an external SOFA set, a VBAP feed or resampled output.
    if device_dsp
        && (sofa
            || is(backend, "vbap")
            || !json_eq(at(spec, "input_rate")?, at(spec, "output_rate")?))
    {
        version = 4;
    }
    // Version 5: the case's own semantic policy and identity. Version 6: room-binaural at the
    // exact tier, one room and no backend switch.
    if room {
        let rate = Value::Integer(48000);
        require(
            !device_dsp
                && !sofa
                && json_eq(at(spec, "input_rate")?, &rate)
                && json_eq(at(spec, "output_rate")?, &rate),
            "room replay renders 48 kHz to the application output",
        )?;
        version = 6;
        switch_control = "no backend switch";
    }
    let mut semantic_control = "semantic gain scale=0.5";
    if contains(spec, "semantic") {
        require(
            !device_dsp && !sofa,
            "semantic replay uses the application output",
        )?;
        let value = at(spec, "semantic")?;
        require(
            value.as_object().is_some_and(|m| m.len() == 2)
                && contains(value, "identity")
                && contains(value, "policy")
                && at(value, "policy")?.as_object().is_some(),
            "invalid semantic replay field",
        )?;
        version = 5;
        semantic_control = "semantic policy and identity from the case";
    }
    let epoch = |e: u32, target: u32, end: u32| {
        object(vec![
            ("epoch", e.into()),
            ("target", target.into()),
            ("end", end.into()),
        ])
    };
    let mut fixed = vec![
        ("version", Value::Integer(version)),
        (
            "clock",
            "epoch*2s + input_sample/input_rate; +1s at sample 10240".into(),
        ),
        ("state_complete_on_every_frame", true.into()),
        (
            "signal",
            object(vec![
                ("generator", "lcg32-v1".into()),
                ("seed", SIGNAL_SEED.into()),
                ("scale", signal_scale(device_dsp).into()),
            ]),
        ),
        (
            "epochs",
            Value::Array(vec![
                epoch(1, 0, MAIN_EPOCH_END),
                epoch(2, SEEK_TARGET, SEEK_END),
            ]),
        ),
        (
            "controls",
            object(vec![
                ("2048", "pose(37,23,-19)".into()),
                ("4096", "generation=2".into()),
                ("6144", semantic_control.into()),
                ("8192", switch_control.into()),
                (
                    "10240",
                    "advance virtual clock by 1s; expire tracking".into(),
                ),
            ]),
        ),
    ];
    if device_dsp {
        fixed.push((
            "output_controls",
            object(vec![
                ("0", "volume=0.875; HpTF bypass".into()),
                (
                    "2048",
                    "HpTF peak 1700Hz +6dB Q0.75 preamp -3dB auto-trim".into(),
                ),
                (
                    "4096",
                    "HpTF high-shelf 4200Hz -4dB Q0.75 preamp -6dB".into(),
                ),
                ("6144", "HpTF bypass".into()),
                ("8192", "volume=0.5".into()),
            ]),
        ));
    } else {
        require(
            !contains(spec, "output_controls"),
            "replay output controls require device DSP",
        )?;
    }
    // nlohmann iterates an object's items in key order.
    fixed.sort_by(|a, b| a.0.cmp(b.0));
    for (name, expected) in &fixed {
        require(contains(spec, name), "missing fixed replay field")?;
        if !json_eq(at(spec, name)?, expected) {
            return Err(format!("unsupported replay field: {name}"));
        }
    }
    require(
        triple || room || is(backend, "vbap") || is(backend, "binaural"),
        "unknown replay backend",
    )
}

/// The replayed element's ADM identity (version 5), as the stream receives it from an import.
fn identity(value: &Value) -> Result<Identity, Failure> {
    let entities = |key: &str| -> Result<Vec<Entity>, Failure> {
        let list = at(value, key)?;
        let items: Vec<&Value> = match list {
            Value::Null => Vec::new(),
            Value::Array(items) => items.iter().collect(),
            Value::Object(map) => map.values().collect(),
            _ => vec![list],
        };
        items
            .into_iter()
            .map(|e| {
                Ok(Entity {
                    id: string(at(e, "id")?)?.as_bytes().to_vec(),
                    name: string(at(e, "name")?)?.as_bytes().to_vec(),
                })
            })
            .collect()
    };
    Ok(Identity {
        object_id: string(at(value, "object_id")?)?.as_bytes().to_vec(),
        object_name: string(at(value, "object_name")?)?.as_bytes().to_vec(),
        track_uid: string(at(value, "track_uid")?)?.as_bytes().to_vec(),
        importance: Some(i32_of(at(value, "importance")?)?),
        dialogue_id: Some(i32_of(at(value, "dialogue_id")?)?),
        contents: entities("contents")?,
        programmes: entities("programmes")?,
    })
}

fn save_json(path: &Path, data: &Value) -> Result<(), Failure> {
    let mut text = mradm_json::dump(data, Some(2));
    text.push('\n');
    std::fs::write(path, text).map_err(|e| e.to_string())
}

/// The renderer configuration as the C++ `RendererConfig` defaults and case fields set it.
#[derive(Clone)]
struct RendererSpec {
    selection: Selection,
    layout: String,
    sofa: Option<(String, String)>,
    binaural_spread: BinauralSpreadMode,
    sample_rate: u32,
    room: Option<Arc<loading::Package>>,
}

impl RendererSpec {
    fn config(&self) -> renderer::Config<'_> {
        renderer::Config {
            selection: self.selection,
            layout_id: self.layout.as_bytes(),
            apple_geometry: false,
            speaker_spread: SpeakerSpread::Automatic,
            binaural_spread: self.binaural_spread,
            split_power: false,
            smoothing_frames: 0,
            sample_rate: self.sample_rate,
            sofa: self.sofa.as_ref().map(|(path, file_name)| Sofa {
                path: path.as_bytes(),
                file_name: file_name.as_bytes(),
            }),
            screens: Screens {
                has_reference: false,
                reference: RoomScreenGeometry::default(),
                has_reproduction: false,
                reproduction: RoomScreenGeometry::default(),
            },
            room: RoomRequest {
                package: self.room.clone(),
                hrir: b"",
                metres: 0.,
                quality: 0,
            },
            sink: true,
        }
    }
}

fn state(changed: u64) -> live::State {
    live::State {
        valid_fields: changed,
        ..live::State::default()
    }
}

struct Replay<'a> {
    spec: &'a Value,
    device_dsp: bool,
    renderer: RendererSpec,
    output_rate: u32,
    time_ns: i64,
    // Dropped before the stream, as the C++ members were.
    output: Option<Session>,
    stream: Arc<Stream>,
    updates: Vec<live::Update>,
    captured: Vec<f32>,
    channels: u32,
    next_device_control: usize,
    ended: bool,
}

impl<'a> Replay<'a> {
    fn new(spec: &'a Value, sofa: Option<&Path>, room: Option<&Path>) -> Result<Self, Failure> {
        let device_dsp = boolean(at(spec, "device_dsp")?)?;
        let backend = at(spec, "backend")?;
        let mut selection = if is(backend, "vbap") {
            Selection::Saf
        } else {
            Selection::SafBinaural
        };
        if is(backend, "triple-balance") {
            selection = Selection::TripleBalance;
        }
        let mut package = None;
        if is(backend, "room-binaural") {
            selection = Selection::RoomBinaural;
            let path = room.unwrap_or(Path::new(""));
            package = Some(Arc::new(
                loading::load_package(path.to_string_lossy().as_bytes())
                    .map_err(|e| e.to_string())?,
            ));
        }
        let renderer = RendererSpec {
            selection,
            layout: string(at(spec, "layout")?)?.to_owned(),
            sofa: sofa.map(|p| {
                (
                    p.to_string_lossy().into_owned(),
                    p.file_name()
                        .map(|n| n.to_string_lossy().into_owned())
                        .unwrap_or_default(),
                )
            }),
            binaural_spread: if boolean(at(spec, "cloud")?)? {
                BinauralSpreadMode::Cloud
            } else {
                BinauralSpreadMode::None
            },
            sample_rate: u32_of(at(spec, "input_rate")?)?,
            room: package,
        };
        let output_rate = u32_of(at(spec, "output_rate")?)?;
        let time_ns = 1_000_000_000;
        let config = Config {
            output_sample_rate: output_rate,
            output_ring_frames: 8192,
            startup_watermark_frames: 1,
            manual_clock: Some(time_ns),
            ..Config::default()
        };
        let stream = Arc::new(take(Stream::create(
            &config,
            &renderer.config(),
            Arc::new(FfiHost),
        ))?);
        let channels = stream.output_format().1;
        let output = if device_dsp {
            require(channels == 2, "device DSP needs stereo")?;
            Some(take(Session::new(stream.clone(), true, false))?)
        } else {
            None
        };
        let mut updates = Vec::new();
        let mut previous = 0;
        let metadata = at(spec, "metadata")?;
        let items: Vec<&Value> = match metadata {
            Value::Null => Vec::new(),
            Value::Array(items) => items.iter().collect(),
            Value::Object(map) => map.values().collect(),
            _ => vec![metadata],
        };
        for item in items {
            let offset = u32_of(at(item, "sample")?)?;
            require(
                offset > previous && offset < MAIN_EPOCH_END,
                "invalid replay event order",
            )?;
            previous = offset;
            let ramp = u32_of(at(item, "ramp")?)?;
            let name = string(at(item, "field")?)?;
            let value = at(item, "value")?;
            let update_state = match name {
                "gain" => live::State {
                    linear_gain: f32_of(value)?,
                    ..state(field::LINEAR_GAIN)
                },
                "position" => {
                    let p = |i: usize| -> Result<f32, Failure> {
                        let items = value
                            .as_array()
                            .ok_or_else(|| mradm_json::cannot_use_at(value))?;
                        f32_of(items.get(i).ok_or("array index out of range")?)
                    };
                    live::State {
                        position: [p(0)?, p(1)?, p(2)?],
                        ..state(field::POSITION)
                    }
                }
                "size" => {
                    let size = f32_of(value)?;
                    live::State {
                        width: size,
                        height: size,
                        depth: size,
                        ..state(field::EXTENT)
                    }
                }
                "head_locked" => live::State {
                    head_locked: boolean(value)?,
                    ..state(field::HEAD_LOCKED)
                },
                _ => return Err("unknown metadata event".into()),
            };
            updates.push(live::Update {
                element_id: 1,
                offset,
                ramp,
                jump: false,
                changed: update_state.valid_fields,
                cleared: 0,
                state: update_state,
            });
        }
        Ok(Self {
            spec,
            device_dsp,
            renderer,
            output_rate,
            time_ns,
            output,
            stream,
            updates,
            captured: Vec::new(),
            channels,
            next_device_control: 2048,
            ended: false,
        })
    }

    fn healthy(&self) -> Result<(), Failure> {
        let s = self.stream.status();
        require(!s.failed, "Scene worker failed")?;
        require(s.underruns == 0, "unexpected replay underrun")?;
        if let Some(output) = &self.output {
            require(
                !output.status(DeviceProgress::default()).failed,
                "device session failed",
            )?;
        }
        Ok(())
    }

    fn device_controls(&mut self) -> Result<(), Failure> {
        let Some(output) = &self.output else {
            return Ok(());
        };
        let frames = self.captured.len() / self.channels as usize;
        if frames != self.next_device_control {
            return Ok(());
        }
        let parameters = |preamp: f64, band: Band, mode: PreampMode, revision: u64| {
            let coefficients = hptf::design(&[band], preamp, output.sample_rate(), mode)
                .map_err(|e| e.to_string())?;
            take(output.publish_hptf(StereoSnapshot {
                coefficients: [coefficients; 2],
                revision,
                independent: false,
            }))
        };
        let band = |kind: BandKind, frequency: f64, gain_db: f64| Band {
            kind,
            enabled: true,
            frequency,
            gain_db,
            q: 0.75,
        };
        match frames {
            2048 => parameters(
                -3.,
                band(BandKind::Peaking, 1700., 6.),
                PreampMode::AutoTrim,
                1,
            )?,
            4096 => parameters(
                -6.,
                band(BandKind::HighShelf, 4200., -4.),
                PreampMode::WarnOnly,
                2,
            )?,
            6144 => take(output.set_hptf(None, 3))?,
            8192 => take(output.set_volume(0.5))?,
            _ => {}
        }
        self.next_device_control += 2048;
        Ok(())
    }

    fn drain(&mut self, ending: bool) -> Result<bool, Failure> {
        self.healthy()?;
        let status = self.stream.status();
        let finished = status.production_complete;
        let channels = self.channels as usize;
        let mut available = status.buffered_output_frames as usize;
        if self.output.is_some() {
            let consumed = self.captured.len() / channels;
            require(
                status.media_frames_pulled >= consumed as u64,
                "invalid device media accounting",
            )?;
            available += (status.media_frames_pulled - consumed as u64) as usize;
            let lookahead = (self.output_rate / 200).max(1) as usize;
            if !finished {
                available = available.saturating_sub(lookahead);
            }
        }
        if available == 0 && !(ending && finished) {
            return Ok(false);
        }
        self.device_controls()?;
        let mut count = available.min(512);
        if self.output.is_some() && count != 0 {
            count = count.min(
                self.next_device_control
                    .wrapping_sub(self.captured.len() / channels),
            );
        }
        // A one-frame read is permitted only after production completes, to observe EOS.
        if count == 0 {
            count = 1;
        }
        let mut data = vec![0f32; count * channels];
        let got = if let Some(output) = &self.output {
            let pulled = output.pull(&mut data, count);
            if pulled.active {
                output.release();
            }
            self.ended = output.status(DeviceProgress::default()).ended;
            pulled.frames
        } else {
            let result = self.stream.pull(&mut data, count as u32);
            require(
                result.flags & (pull_flag::FAILED | pull_flag::UNDERRUN) == 0,
                "invalid replay pull flags",
            )?;
            self.ended = result.flags & pull_flag::EOS != 0;
            result.media_frames as usize
        };
        require(got == count || finished, "short replay pull before EOS")?;
        self.captured.extend_from_slice(&data[..got * channels]);
        self.healthy()?;
        Ok(got != 0 || self.ended)
    }

    fn fence(&mut self) -> Result<(), Failure> {
        let deadline = Instant::now() + Duration::from_secs(20);
        while self.stream.status().queued_input_samples != 0 {
            self.drain(false)?;
            self.healthy()?;
            require(
                Instant::now() < deadline,
                "replay input processing timed out",
            )?;
            std::thread::yield_now();
        }
        take(self.stream.wait_idle(20_000))?;
        while self.drain(false)? {}
        Ok(())
    }

    fn controls(
        &mut self,
        epoch: u64,
        position: u32,
        generation: &mut u64,
        element: &Descriptor,
    ) -> Result<bool, Failure> {
        let mut reconfigure = false;
        if epoch != 1 {
            return Ok(reconfigure);
        }
        let s = &self.stream;
        if position == 2048 {
            s.set_listener_orientation([37., 23., -19.]);
        }
        if position == 4096 {
            *generation += 1;
            take(s.configure_generation(epoch, *generation, std::slice::from_ref(element)))?;
            reconfigure = true;
        }
        let triple = self.renderer.selection == Selection::TripleBalance;
        if position == 6144 && contains(self.spec, "semantic") {
            let policy = mradm_json::dump(at(at(self.spec, "semantic")?, "policy")?, None);
            take(s.set_semantic_policy_json(policy.as_bytes(), 99))?;
        } else if position == 6144 {
            // The Rust policy arithmetic: scale, a dB value where libm and glibc powf differ by
            // one ulp, the extent/diffuse/divergence products and clamps, and a polar position
            // offset through the portable scene_math. Triple Balance keeps Cartesian room
            // positions: a polar offset would move them beyond the room cube, which it rejects.
            let policy = format!(
                "{}{}}}}}",
                r#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":0.5,"gain_db":-4.6},"extent":{"scale":1.5,"max":0.6},"diffuse":{"scale":0.5},"divergence":{"range_scale":0.5}"#,
                if triple {
                    ""
                } else {
                    r#","position":{"offset":{"azimuth":12.5,"elevation":-4}}"#
                }
            );
            take(s.set_semantic_policy_json(policy.as_bytes(), 99))?;
        }
        if position == 8192 && triple {
            let mut next = self.renderer.clone();
            next.selection = Selection::Saf;
            if next.layout == "7.1.4" {
                next.layout = "4+7+0".into();
            }
            if next.layout == "22.2" {
                next.layout = "9+10+3".into();
            }
            take(s.switch_backend(&next.config()))?;
        }
        if position == 8192
            && self.channels == 2
            && self.renderer.selection != Selection::RoomBinaural
        {
            let mut next = self.renderer.clone();
            if contains(self.spec, "sofa") {
                next.sofa = None;
            } else {
                next.selection = if next.selection == Selection::Saf {
                    Selection::SafBinaural
                } else {
                    Selection::Saf
                };
                next.layout = if next.selection == Selection::Saf {
                    "0+2+0"
                } else {
                    "binaural"
                }
                .into();
            }
            take(s.switch_backend(&next.config()))?;
        }
        Ok(reconfigure)
    }

    fn set_clock(&mut self, ns: i64) {
        self.time_ns = ns;
        self.stream.set_clock(ns);
    }

    /// One complete replay transaction including the timeline and EOS checks.
    fn run_epoch(&mut self, epoch: u64, target: u32, end: u32) -> Result<(), Failure> {
        self.captured.clear();
        self.ended = false;
        self.next_device_control = 2048;
        self.set_clock(epoch as i64 * 2_000_000_000);
        if let Some(output) = &self.output {
            take(output.check_epoch(epoch))?;
            output.park();
            take(output.reset_epoch(epoch, i64::from(target)))?;
            take(output.set_hptf(None, 0))?;
            take(output.set_volume(0.875))?;
            output.play();
        } else {
            take(self.stream.begin_epoch(epoch, i64::from(target)))?;
        }
        take(self.stream.set_semantic_policy_json(b"", epoch * 10))?;
        take(self.stream.switch_backend(&self.renderer.config()))?;
        take(self.stream.wait_idle(20_000))?;
        let element = Descriptor {
            element_id: 1,
            role: 0,
            speaker_label: Vec::new(),
            has_position: false,
            position: [0., 1., 0.],
            flags: 0,
            identity: if contains(self.spec, "semantic") {
                Some(identity(at(at(self.spec, "semantic")?, "identity")?)?)
            } else {
                None
            },
        };
        let mut generation = 1;
        take(
            self.stream
                .configure_generation(epoch, generation, std::slice::from_ref(&element)),
        )?;
        let cloud = boolean(at(self.spec, "cloud")?)?;
        let mut initial_state = live::State {
            valid_fields: field::KNOWN,
            position: [0.25, 0.75, 0.125],
            width: if cloud { 0.5 } else { 0. },
            diffuse: if cloud { 0.25 } else { 0. },
            ..live::State::default()
        };
        initial_state.height = initial_state.width / 2.;
        if self.renderer.selection == Selection::TripleBalance {
            initial_state.height = initial_state.width;
            initial_state.depth = initial_state.width;
            initial_state.diffuse = 0.;
        }
        let initial = [live::Entry {
            element_id: 1,
            state: initial_state,
        }];
        let source = signal(end as usize, SIGNAL_SEED, signal_scale(self.device_dsp));
        let chunks = match at(self.spec, "partition")? {
            Value::Array(items) => items.iter().map(u32_of).collect::<Result<Vec<_>, _>>()?,
            other => return Err(mradm_json::type_must_be("array", other)),
        };
        require(
            !chunks.is_empty() && chunks.iter().all(|&n| n > 0 && n <= 1024),
            "invalid replay partition",
        )?;
        let rate = i64::from(self.renderer.sample_rate);
        let mut step = 0;
        let mut complete = true;
        let mut position = 0u32;
        while position < end {
            self.fence()?;
            self.set_clock(
                epoch as i64 * 2_000_000_000
                    + i64::from(position) * 1_000_000_000 / rate
                    + if position >= 10240 { 1_000_000_000 } else { 0 },
            );
            complete = self.controls(epoch, position, &mut generation, &element)? || complete;
            take(self.stream.wait_idle(20_000))?;
            let mut count = chunks[step % chunks.len()].min(end - position);
            step += 1;
            require(count > 0, "empty partition")?;
            for boundary in [target, 2048, 4096, 6144, 8192, 10240] {
                if boundary > position {
                    count = count.min(boundary - position);
                }
            }
            let frame_updates: Vec<live::Update> = if epoch == 1 {
                self.updates
                    .iter()
                    .filter(|u| u.offset >= position && u.offset < position + count)
                    .map(|u| live::Update {
                        offset: u.offset - position,
                        ..*u
                    })
                    .collect()
            } else {
                Vec::new()
            };
            let planes = [PlaneView {
                element_id: 1,
                sample_count: count,
                stride: 1,
                has_signal: true,
                samples: Some(&source[position as usize..]),
            }];
            // Completeness describes the retained semantic state, not whether this frame carries
            // a new initial-state snapshot. Clearing it mutes audio.
            let frame = FrameInput {
                epoch,
                generation,
                start: i64::from(position),
                duration: count,
                flags: frame_flag::STATE_COMPLETE,
                planes: &planes[..],
                initial: if complete { &initial } else { &[] },
                updates: &frame_updates,
            };
            require(
                take(self.stream.submit_frame(&frame, 20_000))? == Submit::Accepted,
                "replay submit rejected",
            )?;
            complete = false;
            position += count;
        }
        self.fence()?;
        take(self.stream.signal_end(epoch, i64::from(end)))?;
        let deadline = Instant::now() + Duration::from_secs(20);
        while !self.ended {
            self.drain(true)?;
            require(Instant::now() < deadline, "replay EOS timed out")?;
            std::thread::yield_now();
        }
        self.healthy()?;
        // A room-binaural replay (version 6, 48 kHz) also delivers the room tail after EOS.
        let tail = if contains(self.spec, "tail_frames") {
            number(at(self.spec, "tail_frames")?, |_, i| i as u64)?
        } else {
            0
        };
        let in_rate = u64::from(self.renderer.sample_rate);
        let out_rate = u64::from(self.output_rate);
        let expected = (u64::from(end - target) * out_rate).div_ceil(in_rate) + tail;
        require(
            self.captured.len() as u64 == expected * u64::from(self.channels),
            "replay rational frame count mismatch",
        )?;
        if epoch == 1 {
            let channels = self.channels as usize;
            for input_start in [1024u64, 3072, 6144, 10240] {
                let first = (input_start * out_rate / in_rate) as usize;
                let last = ((input_start + 512) * out_rate / in_rate) as usize;
                let window = &self.captured[first * channels..last * channels];
                if !window.iter().any(|v| v.abs() > 1.0e-9) {
                    return Err(format!(
                        "active replay interval unexpectedly silent at input sample {input_start}"
                    ));
                }
            }
        }
        Ok(())
    }
}

fn verify_controls() -> Result<(), Failure> {
    let renderer = RendererSpec {
        selection: Selection::Saf,
        layout: "0+2+0".into(),
        sofa: None,
        binaural_spread: BinauralSpreadMode::Automatic,
        sample_rate: 48000,
        room: None,
    };
    let config = Config {
        output_sample_rate: 48000,
        output_ring_frames: 16,
        startup_watermark_frames: 1,
        manual_clock: Some(1_000_000_000),
        ..Config::default()
    };
    let stream = take(Stream::create(
        &config,
        &renderer.config(),
        Arc::new(FfiHost),
    ))?;
    take(stream.begin_epoch(1, 0))?;
    let element = Descriptor {
        element_id: 1,
        role: 0,
        speaker_label: Vec::new(),
        has_position: false,
        position: [0., 1., 0.],
        flags: 0,
        identity: None,
    };
    take(stream.configure_generation(1, 1, std::slice::from_ref(&element)))?;
    stream.set_listener_orientation([1., 0., 0.]);
    let input = vec![0.125f32; 64];
    let planes = [PlaneView {
        element_id: 1,
        sample_count: 64,
        stride: 1,
        has_signal: true,
        samples: Some(&input),
    }];
    let initial = [live::Entry {
        element_id: 1,
        state: state(field::KNOWN),
    }];
    let frame = FrameInput {
        epoch: 1,
        generation: 1,
        start: 0,
        duration: 64,
        flags: frame_flag::STATE_COMPLETE,
        planes: &planes[..],
        initial: &initial,
        updates: &[],
    };
    require(
        take(stream.submit_frame(&frame, 0))? == Submit::Accepted,
        "control test submit",
    )?;
    let timed_out = stream.wait_idle(10);
    require(
        timed_out
            .as_ref()
            .is_err_and(|e| String::from_utf8_lossy(&e.message).contains("timed out"))
            && !stream.status().failed,
        "fence must time out behind full ring",
    )?;
    take(stream.begin_epoch(2, 0))?;
    take(stream.wait_idle(5000))?;
    require(
        !stream.status().failed && stream.status().queued_input_samples == 0,
        "fence/reset failed to recover",
    )
}

/// `case/../../fixtures/<name>`, with `std::filesystem::parent_path` semantics.
fn fixtures(case: &Path) -> PathBuf {
    let parent = |p: &Path| p.parent().map(Path::to_path_buf).unwrap_or_default();
    parent(&parent(case)).join("fixtures")
}

fn run(args: &[String]) -> Result<(), Failure> {
    if args.len() == 2 && args[1] == "--sofa-supported" {
        println!("{}", if cfg!(feature = "sofa") { "yes" } else { "no" });
        return Ok(());
    }
    if args.len() == 2 && args[1] == "--self-test" {
        return verify_controls();
    }
    require(
        args.len() == 3,
        "usage: mr_adm_consistency_scene case.json output-directory",
    )?;
    let case = Path::new(&args[1]);
    let text = std::fs::read(case).map_err(|e| e.to_string())?;
    let spec = mradm_json::parse(&text).map_err(|e| e.to_string())?;
    validate_spec(&spec)?;
    let sofa = if contains(&spec, "sofa") {
        require(
            cfg!(feature = "sofa"),
            "SOFA replay is disabled in this build",
        )?;
        let name = string(at(&spec, "sofa")?)?;
        let path = Path::new(name);
        require(
            path.file_name().is_some_and(|n| n == path.as_os_str())
                && path.extension().is_some_and(|e| e == "sofa"),
            "invalid replay SOFA fixture name",
        )?;
        let sofa = fixtures(case).join(path);
        require(sofa.is_file(), "missing replay SOFA fixture")?;
        Some(sofa)
    } else {
        None
    };
    let room = if contains(&spec, "room") {
        let name = string(at(&spec, "room")?)?;
        let path = Path::new(name);
        require(
            path.is_relative()
                && !name.is_empty()
                && path.components().all(|c| matches!(c, Component::Normal(_)))
                // Components collapses repeated separators and internal dots. C++ path equality
                // also collapses separators, but lexically_normal() removes the dot components.
                && name.split(std::path::is_separator).all(|part| part != "."),
            "invalid replay room fixture name",
        )?;
        let room = fixtures(case).join(path);
        require(room.is_file(), "missing replay room fixture")?;
        Some(room)
    } else {
        None
    };
    let directory = Path::new(&args[2]);
    std::fs::create_dir_all(directory).map_err(|e| e.to_string())?;
    save_json(&directory.join("events.json"), &spec)?;
    let device_dsp = boolean(at(&spec, "device_dsp")?)?;
    let input = signal(
        MAIN_EPOCH_END as usize,
        SIGNAL_SEED,
        signal_scale(device_dsp),
    );
    pcm(
        &directory.join("input.pcmbits"),
        1,
        u32_of(at(&spec, "input_rate")?)?,
        &input,
    )?;
    for pass in 1..=2 {
        // A second replay can use different callback segment boundaries even with identical
        // PCM. Never mix its checkpoints into the first pass.
        if pass == 2 {
            // SAFETY: the first pass's stream and session are gone, their worker threads joined;
            // no other thread reads the environment.
            unsafe { std::env::remove_var("MR_ADM_TRACE_DIR") };
        }
        let mut replay = Replay::new(&spec, sofa.as_deref(), room.as_deref())?;
        let mut lengths = Vec::new();
        for epoch in [1u64, 2] {
            let (target, end) = if epoch == 1 {
                (0, MAIN_EPOCH_END)
            } else {
                (SEEK_TARGET, SEEK_END)
            };
            replay.run_epoch(epoch, target, end)?;
            let name = format!("pass-{pass}-epoch-{epoch}");
            pcm(
                &directory.join(format!("{name}.pcmbits")),
                replay.channels,
                replay.output_rate,
                &replay.captured,
            )?;
            lengths.push(object(vec![
                ("epoch", epoch.into()),
                (
                    "frames",
                    (replay.captured.len() / replay.channels as usize).into(),
                ),
            ]));
        }
        save_json(
            &directory.join(format!("pass-{pass}.json")),
            &object(vec![
                ("epochs", Value::Array(lengths)),
                ("underruns", replay.stream.status().underruns.into()),
                ("status", "passed".into()),
            ]),
        )?;
    }
    Ok(())
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}
