//! Triple Balance preparation goldens (PCM consistency 4.6). `triple-balance-v1/expected.jsonl`
//! was produced once by the C++ `triple_balance_renderer` / `semantics` / `bed` this module
//! replaced (main `8c406e9`), through `mradm render --renderer triple-balance
//! --write-semantic-report -v`, for every case of `cases.jsonl`: each scene of `scenes/` under
//! the three layouts, both modes, both spread modes, both 22.2 LFE routings and six policies,
//! plus options the backend rejects. A record keeps the exit code, the error line, the backend's
//! preparation log lines and the FNV-1a 64 of its canonical `renderer_effective` report
//! (`samples.jsonl` keeps a few complete reports); the CLI-only rejections have none.
mod common;

use common::{ObjectSpec, TrackSpec, direct_block, input, render_offline};
use mradm_adm::records::Gain;
use mradm_json::Value;
use mradm_render::offline::{Kernel, Window};
use mradm_render::routing::Level;
use mradm_render::triple_balance::{self, Mode, Request, Routing, Spread};
use mradm_scene::policy::UserLevel;
use mradm_scene::{ImportOptions, RenderScene, import, policy};
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::collections::HashMap;
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

fn dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/triple-balance-v1")
}

fn lines(name: &str) -> Vec<Value> {
    std::fs::read_to_string(dir().join(name))
        .unwrap()
        .lines()
        .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
        .collect()
}

fn get<'a>(v: &'a Value, key: &str) -> &'a Value {
    &v.as_object().unwrap()[key]
}

fn fixed<const N: usize>(text: &str) -> [u8; N] {
    let mut out = [b' '; N];
    let n = text.len().min(N);
    out[..n].copy_from_slice(&text.as_bytes()[..n]);
    out
}

/// The scene as the collector's silent PCM24 file: BW64 with its AXML and CHNA, or a plain WAVE.
fn scene_wav(name: &str, out: &Path) -> PathBuf {
    let meta =
        mradm_json::parse(&std::fs::read(dir().join(format!("scenes/{name}.json"))).unwrap())
            .unwrap();
    let number = |key| get(&meta, key).as_f64().unwrap();
    let channels = number("channels") as u16;
    let frames = number("frames") as usize;
    let plain = meta.as_object().unwrap().contains_key("plain");
    let options = if plain {
        WriterOptions::default()
    } else {
        let entries = get(&meta, "chna")
            .as_array()
            .unwrap()
            .iter()
            .map(|e| {
                let e = e.as_array().unwrap();
                ChnaEntry {
                    track_index: e[0].as_f64().unwrap() as u16,
                    uid: fixed::<12>(e[1].as_str().unwrap()),
                    track_format: fixed::<14>(e[2].as_str().unwrap()),
                    pack_format: fixed::<11>(e[3].as_str().unwrap()),
                    pad: 0,
                }
            })
            .collect::<Vec<_>>();
        WriterOptions {
            container: None,
            axml: Some(std::fs::read(dir().join(format!("scenes/{name}.xml"))).unwrap()),
            chna: Some(Chna {
                tracks: channels,
                entries,
            }),
        }
    };
    let path = out.join(format!("{name}.wav"));
    let format = Format {
        channels,
        sample_rate: number("sample_rate") as u32,
        sample_format: SampleFormat::Pcm24,
        channel_mask: None,
    };
    let mut writer = Writer::new(File::create(&path).unwrap(), format, options).unwrap();
    writer
        .write_frames(&vec![0.0; usize::from(channels) * frames])
        .unwrap();
    writer.finish().unwrap();
    path
}

/// Objects with keys in byte order, strings as hex UTF-8, integers in decimal, floats as their
/// f64 bit pattern.
fn canon(v: &Value, out: &mut String) {
    let hex = |s: &str| s.bytes().map(|b| format!("{b:02x}")).collect::<String>();
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Unsigned(n) => out.push_str(&n.to_string()),
        Value::Integer(n) => out.push_str(&n.to_string()),
        Value::Float(f) => out.push_str(&format!("f{:016x}", f.to_bits())),
        Value::String(s) => out.push_str(&format!("s{}", hex(s))),
        Value::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                canon(item, out);
            }
            out.push(']');
        }
        Value::Object(map) => {
            out.push('{');
            for (i, (k, item)) in map.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                out.push_str(&format!("s{}:", hex(k)));
                canon(item, out);
            }
            out.push('}');
        }
    }
}

fn fingerprint(v: &Value) -> String {
    let mut text = String::new();
    canon(v, &mut text);
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for b in text.bytes() {
        h = (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
    }
    format!("{h:016x}")
}

/// The CLI options of a case as the backend sees them (layouts in their internal ids).
struct Options {
    layout: String,
    mode: Mode,
    spread: Spread,
    split_power: bool,
    standard_geometry: bool,
    smoothing_frames: u32,
    routing: Routing,
    input_layout: Option<String>,
    input_channels: Vec<String>,
}

fn options(args: &[Value]) -> Options {
    let mut o = Options {
        layout: String::new(),
        mode: Mode::Standard,
        spread: Spread::Automatic,
        split_power: false,
        standard_geometry: true,
        smoothing_frames: 0,
        routing: Routing::Automatic,
        input_layout: None,
        input_channels: Vec::new(),
    };
    let args: Vec<&str> = args.iter().map(|a| a.as_str().unwrap()).collect();
    for pair in args.chunks(2) {
        let v = pair[1];
        match pair[0] {
            "--output-layout" => {
                o.layout = match v {
                    "7.1.4" => "4+7+0",
                    "22.2" => "9+10+3",
                    "5.1" => "0+5+0",
                    other => other,
                }
                .to_owned();
            }
            "--triple-balance-mode" => o.mode = if v == "d" { Mode::D } else { Mode::Standard },
            "--speaker-spread-mode" => {
                o.spread = match v {
                    "none" => Spread::None,
                    "mdap" => Spread::Mdap,
                    _ => Spread::Automatic,
                }
            }
            "--lfe-routing" => o.split_power = v == "split-power",
            "--speaker-geometry" => o.standard_geometry = v == "standard",
            "--object-smoothing-frames" => o.smoothing_frames = v.parse().unwrap(),
            "--direct-speakers-routing" => {
                o.routing = match v {
                    "label" => Routing::Label,
                    "position" => Routing::Position,
                    "matrix" => Routing::Matrix,
                    _ => Routing::Automatic,
                }
            }
            "--input-layout" => o.input_layout = Some(v.to_owned()),
            "--input-channels" => o.input_channels = v.split(',').map(str::to_owned).collect(),
            other => panic!("unexpected option {other}"),
        }
    }
    o
}

fn level(l: Level) -> &'static str {
    match l {
        Level::Info => "info",
        Level::Warning => "warning",
    }
}

#[test]
fn preparation_reproduces_the_cpp_goldens() {
    let temp = std::env::temp_dir().join(format!("mradm-render-tb-{}", std::process::id()));
    std::fs::create_dir_all(&temp).unwrap();
    let cases = lines("cases.jsonl");
    let expected = lines("expected.jsonl");
    let samples: HashMap<u64, Value> = lines("samples.jsonl")
        .into_iter()
        .map(|s| {
            (
                get(&s, "case").as_f64().unwrap() as u64,
                get(&s, "renderer_effective").clone(),
            )
        })
        .collect();
    let mut wavs: HashMap<String, PathBuf> = HashMap::new();
    let (mut checked, mut prepared) = (0, 0);
    for (index, (case, want)) in cases.iter().zip(&expected).enumerate() {
        let Some(effective) = get(want, "renderer_effective").as_str() else {
            continue;
        };
        let name = get(case, "scene").as_str().unwrap();
        let path = wavs
            .entry(name.to_owned())
            .or_insert_with(|| scene_wav(name, &temp))
            .clone();
        let o = options(get(case, "args").as_array().unwrap());
        let mut scene: RenderScene = import(
            path.to_str().unwrap().as_bytes(),
            ImportOptions {
                input_layout: o.input_layout.as_deref(),
                input_channel_labels: &o.input_channels,
            },
        )
        .unwrap();
        if let Some(text) = case.as_object().unwrap().get("policy") {
            scene.apply_policy(policy::parse(text.as_str().unwrap().as_bytes()).unwrap());
        }
        let request = Request {
            scene: &scene,
            layout_id: o.layout.as_bytes(),
            mode: o.mode,
            spread: o.spread,
            split_power: o.split_power,
            standard_geometry: o.standard_geometry,
            smoothing_frames: o.smoothing_frames,
            routing: o.routing,
            has_matrix: false,
            input_path: path.to_str().unwrap().as_bytes(),
        };
        let mut logs = Vec::new();
        let outcome = triple_balance::prepare(&request, &mut |l, text| {
            logs.push(Value::Array(vec![
                level(l).into(),
                String::from_utf8(text).unwrap().into(),
            ]))
        });
        let report = mradm_json::parse(outcome.report.as_bytes()).unwrap();
        if let Some(sample) = samples.get(&(index as u64)) {
            assert_eq!(
                mradm_json::dump(&report, Some(1)),
                mradm_json::dump(sample, Some(1)),
                "case {index} report"
            );
        }
        assert_eq!(fingerprint(&report), effective, "case {index} report");
        assert_eq!(&Value::Array(logs), get(want, "logs"), "case {index} logs");
        match &outcome.result {
            Ok(_) => {
                prepared += 1;
                assert_eq!(
                    get(want, "exit").as_f64(),
                    Some(0.),
                    "case {index} prepared"
                );
            }
            Err(e) => assert_eq!(
                get(want, "error").as_str().map(str::as_bytes),
                Some(&e.message[..]),
                "case {index} error"
            ),
        }
        checked += 1;
    }
    std::fs::remove_dir_all(&temp).unwrap();
    assert_eq!((checked, prepared), (1294, 470));
}

fn request<'a>(scene: &'a RenderScene, layout: &'a str) -> Request<'a> {
    Request {
        scene,
        layout_id: layout.as_bytes(),
        mode: Mode::Standard,
        spread: Spread::Automatic,
        split_power: false,
        standard_geometry: true,
        smoothing_frames: 0,
        routing: Routing::Automatic,
        has_matrix: false,
        input_path: b"input.wav",
    }
}

fn fixture(path: &str) -> Value {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/fixtures");
    mradm_json::parse(&std::fs::read(root.join(path)).unwrap()).unwrap()
}

fn zero_gain() -> Gain {
    Gain {
        present: 1,
        ..Gain::default()
    }
}

/// The captured bed: native gain 0, muted, starting at 1 s, each channel in two timed blocks of
/// gain 0 (moved from the C++ `triple_balance_bed_test`).
fn captured_bed(labels: &[&'static str]) -> RenderScene {
    let tracks = labels
        .iter()
        .map(|&label| {
            let mut direct = Vec::new();
            for (start, rtime) in [(96000u64, 48000u64), (144000, 96000)] {
                let mut b = direct_block(start, start + 48000);
                b.gain = 0.;
                b.source.gain = zero_gain();
                b.source.rtime_present = 1;
                b.source.rtime_samples = rtime;
                b.source.duration_present = 1;
                b.source.duration_samples = 48000;
                direct.push((vec![label], b));
            }
            TrackSpec {
                blocks: Vec::new(),
                direct,
            }
        })
        .collect();
    let mut scene = common::scene(
        vec![ObjectSpec {
            id: "bed",
            gain: 0.,
            end: 96000,
            tracks,
        }],
        240000,
    );
    scene.info.channels = labels.len() as u16;
    let object = &mut scene.snapshot.objects[0];
    object.source_gain = zero_gain();
    object.mute = 1;
    object.start = 48000;
    scene
}

const RC: [&str; 10] = [
    "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts",
];

fn report(r: &Request) -> (bool, Value) {
    let outcome = triple_balance::prepare(r, &mut |_, _| {});
    (
        outcome.result.is_ok(),
        mradm_json::parse(outcome.report.as_bytes()).unwrap(),
    )
}

fn at<'a>(v: &'a Value, path: &[&str]) -> &'a Value {
    path.iter().fold(v, |v, key| match key.parse::<usize>() {
        Ok(i) => &v.as_array().unwrap()[i],
        Err(_) => get(v, key),
    })
}

#[test]
fn the_bed_routes_as_the_captured_native_renderer() {
    let reference = fixture("room_compat_bed.json");
    let scene = captured_bed(&RC);
    for layout in ["4+7+0", "9.1.6"] {
        let (ok, doc) = report(&request(&scene, layout));
        assert!(ok, "captured bed source fields prepare successfully");
        let key = if layout == "4+7+0" { "7.1.4" } else { layout };
        for (i, _) in RC.iter().enumerate() {
            let gains = at(
                &doc,
                &[
                    "beds",
                    "0",
                    "tracks",
                    &i.to_string(),
                    "gains_internal_layout_order",
                ],
            );
            let want = at(&reference, &["layouts", key, &i.to_string()]);
            let as_f32 = |v: &Value| -> Vec<f32> {
                v.as_array()
                    .unwrap()
                    .iter()
                    .map(|g| g.as_f64().unwrap() as f32)
                    .collect()
            };
            assert_eq!(
                as_f32(gains),
                as_f32(want),
                "bed route matches the captured OAR gains"
            );
        }
        assert_eq!(
            at(
                &doc,
                &[
                    "beds",
                    "0",
                    "tracks",
                    "0",
                    "events",
                    "0",
                    "source_rtime_sample"
                ]
            )
            .as_f64(),
            Some(48000.),
            "the report keeps the authored relative time"
        );
    }
    // The object and RC_Lts at half user level: both apply once after the ignored native gain.
    let mut scene = captured_bed(&RC);
    let half = UserLevel {
        gain_multiplier: 0.5,
        mute: Some(false),
    };
    scene.overlay.objects[0].user = half;
    let lts = scene.snapshot.tracks[8].ds_blocks;
    for state in &mut scene.overlay.direct_blocks[lts.offset..lts.offset + lts.len] {
        state.user = half;
    }
    let (ok, doc) = report(&request(&scene, "9.1.6"));
    assert!(ok);
    assert_eq!(
        at(&doc, &["beds", "0", "tracks", "8", "output_gain"]).as_f64(),
        Some(0.25)
    );
    let rejected = |r: &Request| {
        let outcome = triple_balance::prepare(r, &mut |_, _| {});
        let doc = mradm_json::parse(outcome.report.as_bytes()).unwrap();
        outcome.result.is_err_and(|e| e.code == 2)
            && get(&doc, "status").as_str() == Some("unsupported")
    };
    let incomplete = captured_bed(&RC[..9]);
    assert!(rejected(&request(&incomplete, "9.1.6")), "incomplete bed");
    let mut labels = RC;
    labels[0] = "RC_R";
    let reordered = captured_bed(&labels);
    assert!(rejected(&request(&reordered, "9.1.6")), "label/PCM reorder");
    let mut labels = RC;
    labels[3] = "rc_lfe";
    let lowercase = captured_bed(&labels);
    assert!(
        rejected(&request(&lowercase, "9.1.6")),
        "labels match exactly"
    );
    let scene = captured_bed(&RC);
    let mut position = request(&scene, "9.1.6");
    position.routing = Routing::Position;
    assert!(rejected(&position), "geometry-based routing");
    let mut rate = captured_bed(&RC);
    rate.info.sample_rate = 44100;
    assert!(rejected(&request(&rate, "9.1.6")), "bed sample rate");
    let mut aimed = captured_bed(&RC);
    aimed.overlay.direct_blocks[0].position_override = true;
    assert!(rejected(&request(&aimed, "9.1.6")), "explicit user re-aim");
}

/// A point object starting at 1000 samples with native gain 0, mute and a short duration (moved
/// from the C++ `triple_balance_semantics_test`).
fn native_object(rtimes: &[(u64, [f32; 3])], frames: u64) -> RenderScene {
    let blocks = rtimes
        .iter()
        .map(|&(rtime, [x, y, z])| {
            let mut b = common::object_block(rtime + 1000, rtime + 2000, 0.);
            b.position.cartesian = 1;
            (b.position.x, b.position.y, b.position.z) = (x, y, z);
            b.gain = 0.25;
            b.source.gain = Gain {
                present: 1,
                decibels: 0,
                value: 0.25,
                linear: 0.25,
            };
            b.source.rtime_present = 1;
            b.source.rtime_samples = rtime;
            b.source.duration_present = 1;
            b.source.duration_samples = 1000;
            b
        })
        .collect();
    let mut scene = common::scene(
        common::single(
            "AO_1001",
            0.,
            TrackSpec {
                blocks,
                direct: Vec::new(),
            },
        ),
        frames,
    );
    let object = &mut scene.snapshot.objects[0];
    object.end = 4000;
    object.source_gain = zero_gain();
    object.mute = 1;
    object.start = 1000;
    scene
}

#[test]
fn point_motion_keeps_the_reference_state_below_a_quantization_boundary() {
    let reference = fixture("room_compat_semantics/point-motion-half-code.json");
    let rtime = get(&reference, "rtime_target").as_f64().unwrap() as u64;
    let frames = 145_000u64;
    let scene = native_object(
        &[(0, [0.23, 0.51, 0.37]), (rtime, [-0.2, 0.8, 0.5])],
        frames,
    );
    let prepared = triple_balance::prepare(&request(&scene, "9.1.6"), &mut |_, _| {})
        .result
        .unwrap();
    let amplitude = 0.0625f32;
    let source = input(&vec![amplitude; frames as usize]);
    let pcm = render_offline(
        Kernel::TripleBalance(prepared.plan),
        &source,
        16,
        frames,
        0,
        Some(Window {
            start: 120_000,
            count: 24_000,
        }),
    )
    .unwrap();
    assert_eq!(pcm.len(), 24_000 * 16);
    let gains = get(&reference, "signed_gains").as_array().unwrap();
    for (channel, want) in gains.iter().enumerate() {
        let error = (pcm[channel] / amplitude - want.as_f64().unwrap() as f32).abs();
        assert!(error < 2e-4, "channel {channel}: {error}");
    }
}

/// A 0.25-size Cartesian object over two events of a mono sine (moved from the C++
/// `vbap_smoke_test` size checks).
fn sized_object(frames: u64) -> RenderScene {
    let blocks = [(0u64, [0f32, 1., 0.]), (4096, [0.5, 0.5, 0.25])]
        .iter()
        .map(|&(start, [x, y, z])| {
            let mut b = common::object_block(start, u64::MAX, 0.);
            b.position.cartesian = 1;
            (b.position.x, b.position.y, b.position.z) = (x, y, z);
            (b.width, b.height, b.depth) = (0.25, 0.25, 0.25);
            b.source.rtime_present = 1;
            b.source.rtime_samples = start;
            b
        })
        .collect();
    common::scene(
        common::single(
            "AO_1001",
            1.,
            TrackSpec {
                blocks,
                direct: Vec::new(),
            },
        ),
        frames,
    )
}

#[test]
fn size_windows_native_fields_and_user_levels() {
    let frames = 10_037u64;
    let source = input(&common::sine(frames as usize, 0.5));
    for (layout, outputs) in [("4+7+0", 12usize), ("9.1.6", 16)] {
        let render = |scene: &RenderScene, window: Option<Window>| {
            let prepared = triple_balance::prepare(&request(scene, layout), &mut |_, _| {})
                .result
                .unwrap();
            render_offline(
                Kernel::TripleBalance(prepared.plan),
                &source,
                outputs,
                frames,
                0,
                window,
            )
            .unwrap()
        };
        let scene = sized_object(frames);
        let whole = render(&scene, None);
        assert_eq!(
            whole.len(),
            frames as usize * outputs,
            "the EOF partial block"
        );
        assert!(whole.iter().any(|&v| v != 0.));
        let crop = render(
            &scene,
            Some(Window {
                start: 4097,
                count: 4321,
            }),
        );
        assert_eq!(
            crop,
            whole[4097 * outputs..(4097 + 4321) * outputs],
            "a stateful crop equals the full render's slice after warm-up"
        );
        // One prepared plan, two drivers: each starts a fresh size state.
        let prepared = triple_balance::prepare(&request(&scene, layout), &mut |_, _| {})
            .result
            .unwrap();
        for _ in 0..2 {
            let again = render_offline(
                Kernel::TripleBalance(Arc::clone(&prepared.plan)),
                &source,
                outputs,
                frames,
                0,
                None,
            )
            .unwrap();
            assert_eq!(again, whole, "reused prepared metadata");
        }
        // Native zero gain, mute and a short lifetime, as the copy ignores them.
        let mut native = sized_object(frames);
        let s = &mut native.snapshot;
        s.objects[0].source_gain = zero_gain();
        s.objects[0].mute = 1;
        s.objects[0].end = 2000;
        s.objects[0].start = 1000;
        for b in &mut s.object_blocks {
            b.start += 1000;
            b.end = 1500;
        }
        assert_eq!(render(&native, None), whole, "ignored native fields");
        let mut half = sized_object(frames);
        half.overlay.objects[0].user.gain_multiplier = 0.5;
        let scaled: Vec<f32> = whole.iter().map(|v| v * 0.5).collect();
        assert_eq!(
            render(&half, None),
            scaled,
            "user volume scales dry and size output once"
        );
        let mut muted = sized_object(frames);
        muted.overlay.objects[0].user.mute = Some(true);
        assert!(
            render(&muted, None).iter().all(|&v| v == 0.),
            "user mute includes the filter tail"
        );
        let fails = |scene: &RenderScene, edit: &dyn Fn(&mut Request)| {
            let mut r = request(scene, layout);
            edit(&mut r);
            triple_balance::prepare(&r, &mut |_, _| {}).result.is_err()
        };
        let mut rate = sized_object(frames);
        rate.info.sample_rate = 44100;
        assert!(fails(&rate, &|_| {}), "unverified size sample rate");
        let mut axes = sized_object(frames);
        for b in &mut axes.snapshot.object_blocks {
            b.height = 0.5;
        }
        assert!(fails(&axes, &|_| {}), "independent axis extent");
        assert!(
            !fails(&axes, &|r| r.spread = Spread::None),
            "spread=none preserves the point fallback"
        );
        assert!(
            fails(&scene, &|r| r.spread = Spread::Mdap),
            "room size and MDAP are exclusive"
        );
    }
}

#[test]
fn a_late_first_event_snaps_to_zero_below_one_control_block_else_follows_a_default() {
    let frames = 48_000u64;
    let source = input(&common::sine(frames as usize, 0.5));
    let render = |rtime: u64| {
        let mut b = common::object_block(rtime, u64::MAX, 0.);
        b.position.cartesian = 1;
        (b.position.x, b.position.y, b.position.z) = (0.5, 0.5, 0.25);
        b.source.rtime_present = 1;
        b.source.rtime_samples = rtime;
        let scene = common::scene(
            common::single(
                "AO_1001",
                1.,
                TrackSpec {
                    blocks: vec![b],
                    direct: Vec::new(),
                },
            ),
            frames,
        );
        let prepared = triple_balance::prepare(&request(&scene, "9.1.6"), &mut |_, _| {})
            .result
            .unwrap();
        render_offline(
            Kernel::TripleBalance(prepared.plan),
            &source,
            16,
            frames,
            0,
            None,
        )
        .unwrap()
    };
    let at_zero = render(0);
    for rtime in [31, 511] {
        assert_eq!(render(rtime), at_zero, "rtime {rtime} starts the timeline");
    }
    for rtime in [512, 513] {
        let late = render(rtime);
        assert_ne!(
            late[..512 * 16],
            at_zero[..512 * 16],
            "rtime {rtime} follows the default first"
        );
    }
}
