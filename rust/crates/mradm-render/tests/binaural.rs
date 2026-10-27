//! Binaural source plan goldens and rules (PCM consistency 4.7 B). `binaural-v1/expected.jsonl`
//! was produced once by the C++ `build_sources`, `build_spreader_tracks` and
//! `build_spreader_groups` this module replaced (main `c967d84`), with a temporary generator that
//! compiled `binaural_renderer.cpp` into itself: for every case of `cases.jsonl` (each scene of
//! `scenes/` under the three spread modes, the policy-v1 policies, live topology scales and
//! spreader group budgets) it kept the import or policy error, the warnings, the counts and the
//! FNV-1a 64 of the plan text below (`samples.jsonl` keeps a few complete texts). NaN directions
//! (from a 1e10 policy offset) print as `nan`: their sign differs between inlined copies of the
//! same Rust arithmetic and never reaches PCM, as the HRTF lookup rejects them. `bin-extent-edge`
//! (extent exactly at and one ulp below the 1 degree threshold) was added by the same generator
//! against the same commit after the first set left the threshold comparison unobserved.
//!
//! `renders.jsonl` holds 54 renders of the same commit's CLI (`--renderer saf-binaural
//! --no-peak-limit --output-bit-depth f32`, every spread, smoothing, window and listener pose
//! variant listed there) of each scene carrying the LCG signal of `signal`: the frame count and
//! the FNV-1a 64 of the float output's little-endian bytes.
mod common;

use common::{ObjectSpec, TrackSpec, direct_block, object_block, scene};
use mradm_json::Value;
use mradm_render::binaural::{self, Input, Scales, Source, Spread};
use mradm_render::routing::Level;
use mradm_scene::{ImportOptions, RenderScene, import, policy};
use mradm_wav::{Chna, ChnaEntry, Format, SampleFormat, Writer, WriterOptions};
use std::collections::{BTreeMap, HashMap};
use std::fs::File;
use std::path::{Path, PathBuf};

fn dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/binaural-v1")
}

fn lines(path: &Path) -> Vec<Value> {
    std::fs::read_to_string(path)
        .unwrap()
        .lines()
        .map(|l| mradm_json::parse(l.as_bytes()).unwrap())
        .collect()
}

fn get<'a>(v: &'a Value, key: &str) -> Option<&'a Value> {
    v.as_object().unwrap().get(key)
}

fn fixed<const N: usize>(text: &str) -> [u8; N] {
    let mut out = [b' '; N];
    let n = text.len().min(N);
    out[..n].copy_from_slice(&text.as_bytes()[..n]);
    out
}

/// The scene as the generator's silent PCM24 file with its AXML and CHNA.
fn scene_wav(name: &str, out: &Path) -> PathBuf {
    write_scene_wav(name, out, false)
}

/// The scene's PCM24 file, silent or carrying `signal`.
fn write_scene_wav(name: &str, out: &Path, with_signal: bool) -> PathBuf {
    let meta =
        mradm_json::parse(&std::fs::read(dir().join(format!("scenes/{name}.json"))).unwrap())
            .unwrap();
    let number = |key| get(&meta, key).unwrap().as_f64().unwrap();
    let channels = number("channels") as u16;
    let frames = number("frames") as usize;
    let entries = get(&meta, "chna")
        .unwrap()
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
    let options = WriterOptions {
        container: None,
        axml: Some(std::fs::read(dir().join(format!("scenes/{name}.xml"))).unwrap()),
        chna: Some(Chna {
            tracks: channels,
            entries,
        }),
    };
    let path = out.join(format!(
        "{name}{}.wav",
        if with_signal { "-signal" } else { "" }
    ));
    let format = Format {
        channels,
        sample_rate: number("sample_rate") as u32,
        sample_format: SampleFormat::Pcm24,
        channel_mask: None,
    };
    let mut writer = Writer::new(File::create(&path).unwrap(), format, options).unwrap();
    let samples = usize::from(channels) * frames;
    let pcm = if with_signal {
        signal(samples)
    } else {
        vec![0.0; samples]
    };
    writer.write_frames(&pcm).unwrap();
    writer.finish().unwrap();
    path
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// The sign and payload of a NaN result are unspecified (LLVM): every NaN prints the same.
fn bits(v: f32) -> String {
    if v.is_nan() {
        return "nan".to_owned();
    }
    format!("{:08x}", v.to_bits())
}

/// The generator's plan text: sources with their blocks, spreader tracks, groups.
fn plan_text(sources: &[Source], tracks: &[binaural::Track], groups: &[binaural::Group]) -> String {
    let flag = |b: bool| if b { "1" } else { "0" };
    let mut t = String::new();
    for s in sources {
        t += &format!(
            "S {} {} {} {} {}{}{}\n",
            s.channel,
            bits(s.gain),
            hex(&s.object_id),
            hex(&s.label_key),
            flag(s.bypass_lfe),
            flag(s.smoothable),
            flag(s.diffuse_bus)
        );
        for b in &s.blocks {
            t += &format!(
                "B {} {} {} {} {} {} {} {}\n",
                bits(b.az),
                bits(b.el),
                bits(b.gain),
                b.start,
                b.end,
                flag(b.jump),
                b.interpolation.map_or("-".to_owned(), |v| v.to_string()),
                flag(b.head_locked)
            );
        }
    }
    for tr in tracks {
        t += &format!(
            "T {} {} {} {}\n",
            tr.seed,
            tr.channel,
            bits(tr.object_gain),
            tr.sources
        );
        for b in &tr.blocks {
            t += &format!("b {} {} {}\n", b.start, b.end, flag(b.head_locked));
            for s in &b.sources {
                t += &format!(
                    "s {} {} {} {}\n",
                    bits(s.az),
                    bits(s.el),
                    bits(s.spread),
                    bits(s.gain)
                );
            }
        }
    }
    for g in groups {
        t += "G";
        for i in &g.tracks {
            t += &format!(" {i}");
        }
        t += " |";
        for l in &g.lanes {
            t += &format!(" {}:{}", l.track_slot, l.source);
        }
        t += "\n";
    }
    t
}

fn fnv(text: &str) -> String {
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for b in text.bytes() {
        h = (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
    }
    format!("{h:016x}")
}

fn spread(name: &str) -> Spread {
    match name {
        "none" => Spread::None,
        "saf" => Spread::SafSpreader,
        _ => Spread::Cloud,
    }
}

type Plan = (
    Vec<Source>,
    Vec<binaural::Track>,
    Vec<binaural::Group>,
    Vec<Value>,
);

fn plan(input: &Input, spread: Spread, scales: &BTreeMap<Vec<u8>, Scales>, budget: usize) -> Plan {
    let mut logs = Vec::new();
    let mut log = |level: Level, text: Vec<u8>| {
        logs.push(Value::Array(vec![
            Value::Unsigned(if level == Level::Info { 1 } else { 2 }),
            "binaural".into(),
            String::from_utf8(text).unwrap().into(),
        ]))
    };
    let sources = binaural::sources(input, spread, scales, &mut log);
    let (mut tracks, mut groups) = (Vec::new(), Vec::new());
    if spread == Spread::SafSpreader {
        tracks = binaural::tracks(input, &mut log);
        groups = binaural::groups(&tracks, budget);
    }
    (sources, tracks, groups, logs)
}

#[test]
fn the_plan_reproduces_the_cpp_goldens() {
    let temp = std::env::temp_dir().join(format!("mradm-render-binaural-{}", std::process::id()));
    std::fs::create_dir_all(&temp).unwrap();
    let policies = lines(
        &Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../mradm-scene/tests/policy-v1/policies.jsonl"),
    );
    let cases = lines(&dir().join("cases.jsonl"));
    let expected = lines(&dir().join("expected.jsonl"));
    let samples: HashMap<u64, String> = lines(&dir().join("samples.jsonl"))
        .into_iter()
        .map(|s| {
            (
                get(&s, "case").unwrap().as_f64().unwrap() as u64,
                get(&s, "text").unwrap().as_str().unwrap().to_owned(),
            )
        })
        .collect();
    let mut wavs: HashMap<String, PathBuf> = HashMap::new();
    let (mut planned, mut rejected, mut sourced) = (0, 0, 0);
    for (index, (case, want)) in cases.iter().zip(&expected).enumerate() {
        let name = get(case, "scene").unwrap().as_str().unwrap();
        let path = wavs
            .entry(name.to_owned())
            .or_insert_with(|| scene_wav(name, &temp))
            .clone();
        let mut scene: RenderScene = import(
            path.to_str().unwrap().as_bytes(),
            ImportOptions {
                input_layout: None,
                input_channel_labels: &[],
            },
        )
        .unwrap();
        if let Some(line) = get(case, "policy_line") {
            let text = get(&policies[line.as_f64().unwrap() as usize], "policy")
                .unwrap()
                .as_str()
                .unwrap();
            match policy::parse(text.as_bytes()) {
                Ok(p) => {
                    scene.apply_policy(p);
                }
                Err(_) => {
                    assert!(get(want, "error").is_some(), "case {index} policy");
                    rejected += 1;
                    continue;
                }
            }
        }
        assert!(get(want, "error").is_none(), "case {index} prepared");
        let mut scales = BTreeMap::new();
        if let Some(s) = get(case, "scales") {
            let s: Vec<f32> = s
                .as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_f64().unwrap() as f32)
                .collect();
            for object in &scene.snapshot.objects {
                let id = scene.snapshot.strings[object.id as usize]
                    .as_bytes()
                    .to_vec();
                scales.insert(id, [s[0], s[1], s[2], s[3], s[4]]);
            }
        }
        let budget = get(case, "budget").map_or(8, |b| b.as_f64().unwrap() as usize);
        let input = Input::of(&scene);
        let (sources, tracks, groups, logs) = plan(
            &input,
            spread(get(case, "spread").unwrap().as_str().unwrap()),
            &scales,
            budget,
        );
        let text = plan_text(&sources, &tracks, &groups);
        if let Some(sample) = samples.get(&(index as u64)) {
            assert_eq!(&text, sample, "case {index} plan");
        }
        assert_eq!(
            Some(fnv(&text).as_str()),
            get(want, "plan").unwrap().as_str(),
            "case {index} plan"
        );
        let counts = get(want, "counts").unwrap().as_array().unwrap();
        assert_eq!(
            [sources.len(), tracks.len(), groups.len()].map(|n| n as f64),
            [0, 1, 2].map(|i| counts[i].as_f64().unwrap()),
            "case {index} counts"
        );
        assert_eq!(
            &Value::Array(logs),
            get(want, "logs").unwrap(),
            "case {index} logs"
        );
        planned += 1;
        sourced += usize::from(!sources.is_empty());
    }
    std::fs::remove_dir_all(&temp).unwrap();
    assert_eq!((planned, rejected), (2031, 480));
    assert!(sourced > 1400);
}

// ── Rules the imported scenes cannot reach (every imported DirectSpeakers block has a position) ──

fn object(id: &'static str, track: TrackSpec) -> ObjectSpec {
    ObjectSpec {
        id,
        gain: 0.5,
        end: 4000,
        tracks: vec![track],
    }
}

fn sources_of(scene: &RenderScene, spread: Spread) -> (Vec<Source>, Vec<String>) {
    let mut logs = Vec::new();
    let sources = binaural::sources(&Input::of(scene), spread, &BTreeMap::new(), &mut |_, t| {
        logs.push(String::from_utf8(t).unwrap())
    });
    (sources, logs)
}

#[test]
fn positionless_direct_speakers_use_the_first_known_label_or_are_skipped() {
    let mut first = direct_block(0, 9000);
    first.gain = 0.25;
    let mut second = direct_block(100, 9000);
    second.head_locked = 1;
    let track = TrackSpec {
        blocks: Vec::new(),
        direct: vec![
            (vec!["urn:x", "U-135", "M+030"], first),
            (vec!["urn:y"], second),
        ],
    };
    let s = scene(vec![object("DS", track)], 4000);
    let (sources, logs) = sources_of(&s, Spread::Cloud);
    assert_eq!(sources.len(), 1);
    let b = sources[0].blocks[0];
    assert_eq!(
        (b.az, b.el, b.gain, b.end, b.jump),
        (-135., 30., 0.25, 4000, true)
    );
    assert_eq!(
        sources[0].label_key,
        mradm_render::routing::canonical_label(b"urn:x")
    );
    assert_eq!(
        logs,
        ["DirectSpeakers channel 0 has no resolvable position, skipping"]
    );
}

#[test]
fn lfe_bypasses_the_hrtf_and_position_wins_over_labels() {
    let mut lfe = direct_block(0, 9000);
    lfe.low_pass_present = 1;
    lfe.low_pass = 120.;
    let mut placed = direct_block(0, 9000);
    placed.has_position = 1;
    placed.azimuth = 12.5;
    placed.elevation = -4.;
    let s = scene(
        vec![
            object(
                "L",
                TrackSpec {
                    blocks: Vec::new(),
                    direct: vec![(vec!["M+030"], lfe)],
                },
            ),
            object(
                "P",
                TrackSpec {
                    blocks: Vec::new(),
                    direct: vec![(vec!["M+030"], placed)],
                },
            ),
        ],
        4000,
    );
    let (sources, _) = sources_of(&s, Spread::None);
    assert!(sources[0].bypass_lfe && !sources[0].smoothable);
    assert_eq!((sources[0].blocks[0].az, sources[0].blocks[0].el), (0., 0.));
    assert_eq!(
        (sources[1].blocks[0].az, sources[1].blocks[0].el),
        (12.5, -4.)
    );
}

#[test]
fn diffuse_and_extent_split_into_the_direct_and_diffuse_banks() {
    let mut b = object_block(0, 4000, 30.);
    b.diffuse = 0.5;
    b.width = 40.;
    b.height = 20.;
    let s = scene(
        vec![object(
            "O",
            TrackSpec {
                blocks: vec![b],
                direct: Vec::new(),
            },
        )],
        4000,
    );
    let (none, _) = sources_of(&s, Spread::None);
    assert_eq!(none.len(), 2);
    assert!(!none[0].diffuse_bus && none[1].diffuse_bus);
    let g = 0.5f32.sqrt();
    assert_eq!((none[0].blocks[0].gain, none[1].blocks[0].gain), (g, g));
    let (cloud, _) = sources_of(&s, Spread::Cloud);
    assert!(cloud.len() > 2, "an extent cloud has several slots");
    // The SAF spreader takes the extended direct source; only the diffuse bank stays.
    let (saf, _) = sources_of(&s, Spread::SafSpreader);
    assert_eq!(saf.len(), 1);
    assert!(saf[0].diffuse_bus);
    let tracks = binaural::tracks(&Input::of(&s), &mut |_, _| {});
    assert_eq!(tracks.len(), 1);
    assert_eq!(tracks[0].blocks[0].sources[0].gain, g);
}

#[test]
fn group_packing_follows_the_lane_budget() {
    let track = |sources| binaural::Track {
        seed: 0,
        channel: 0,
        object_gain: 1.,
        blocks: Vec::new(),
        sources,
    };
    let few: Vec<_> = (0..3).map(|_| track(3)).collect();
    // 9 lanes do not exceed twice the budget of 8: one adapter per track.
    assert_eq!(binaural::groups(&few, 8).len(), 3);
    let many: Vec<_> = (0..12).map(|_| track(3)).collect();
    // 36 lanes > 16: packed to ceil(36 / 8) = 5 lanes, so one 3-lane track per group.
    let groups = binaural::groups(&many, 8);
    assert_eq!(groups.len(), 12);
    let packed = binaural::groups(&(0..12).map(|_| track(1)).collect::<Vec<_>>(), 2);
    assert_eq!(packed.len(), 2);
    assert_eq!(packed[0].tracks, (0..6).collect::<Vec<_>>());
    assert!(binaural::groups(&[], 8).is_empty());
}

// ── Offline rendering ─────────────────────────────────────────────────────────

use common::{input, sine};
use mradm_dsp::dataset::Dataset;
use mradm_render::binaural_hrtf;
use mradm_render::binaural_render::{self, Bank, Offline, Settings};
use mradm_render::offline::Window;
use std::sync::Arc;

struct Render {
    spread: Spread,
    smoothing: u32,
    window: Option<Window>,
    pose: Option<[f32; 3]>,
}

fn render(scene: &RenderScene, file: &str, r: &Render) -> Result<Vec<f32>, String> {
    let block = u64::from(r.smoothing).max(1024);
    let hrtf = Arc::new(binaural_hrtf::build(Dataset::kemar(), block, false).unwrap());
    let prepared =
        binaural_render::prepare(scene, hrtf, r.spread, r.smoothing, &mut |_, _| {}).unwrap();
    let reader = Offline::open_reader(file).unwrap();
    let mut driver = Offline::new(
        Arc::new(prepared),
        reader,
        Settings {
            inputs: usize::from(scene.info.channels),
            frames: scene.info.frames,
            sample_rate: scene.info.sample_rate,
            window: r.window,
            smoothing_frames: r.smoothing,
            pose: r.pose,
        },
    )
    .map_err(|e| e.message)?;
    let mut out = Vec::new();
    while let Some(step) = driver.next_step().map_err(|e| e.message)? {
        out.extend_from_slice(driver.output(step.emit));
    }
    Ok(out)
}

fn words(v: &[f32]) -> Vec<u32> {
    v.iter().map(|x| x.to_bits()).collect()
}

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Point,
    Cloud,
    Diffuse,
    Direct,
}

/// Two starts in reverse chronological order; each winner is shorter than its overwritten peers,
/// which must not resume when it ends.
fn same_start(kind: Kind, include_overwritten: bool) -> TrackSpec {
    let mut track = TrackSpec::default();
    for i in if include_overwritten { 0 } else { 39 }..40 {
        for group in [1u64, 0] {
            let winner = i == 39;
            let start = group * 2048;
            let end = start + if winner { 1024 } else { 2048 };
            let azimuth = if winner {
                if group == 0 { 97.5 } else { -37.5 }
            } else {
                -97.5 + 5. * i as f32
            };
            let head_locked = u32::from(winner && group == 1);
            if kind == Kind::Direct {
                let mut b = direct_block(start, end);
                b.azimuth = azimuth;
                b.has_position = 1;
                b.gain = if winner { 0.375 } else { 0.9 };
                b.head_locked = head_locked;
                if !winner {
                    b.low_pass_present = 1;
                    b.low_pass = 120.;
                }
                track.direct.push((Vec::new(), b));
                continue;
            }
            let mut b = object_block(start, end, azimuth);
            b.gain = if winner { 0.375 } else { 0.9 };
            b.head_locked = head_locked;
            if !winner || kind == Kind::Cloud {
                b.width = 0.4;
                b.height = 0.2;
                b.diffuse = 0.25;
                b.divergence = 0.5;
            } else if kind == Kind::Diffuse {
                b.diffuse = 1.;
            }
            track.blocks.push(b);
        }
    }
    track
}

fn one_object(track: TrackSpec, frames: u64) -> RenderScene {
    scene(
        vec![ObjectSpec {
            id: "AO_1001",
            gain: 1.,
            end: u64::MAX,
            tracks: vec![track],
        }],
        frames,
    )
}

#[test]
fn only_the_last_block_at_each_start_renders() {
    let file = input(&sine(4096, 0.5));
    for kind in [Kind::Point, Kind::Cloud, Kind::Diffuse, Kind::Direct] {
        for spread in [Spread::Cloud, Spread::SafSpreader] {
            for smoothing in [0, 1024] {
                let r = Render {
                    spread,
                    smoothing,
                    window: None,
                    pose: Some([37., 13., -9.]),
                };
                let expected =
                    render(&one_object(same_start(kind, false), 4096), file.path(), &r).unwrap();
                let actual =
                    render(&one_object(same_start(kind, true), 4096), file.path(), &r).unwrap();
                assert_eq!(expected.len(), 4096 * 2);
                assert_eq!(words(&actual), words(&expected));
            }
        }
    }
}

#[test]
fn live_keys_bind_to_the_winning_direct_speakers_block() {
    for label in ["M+030", ""] {
        let mut first = direct_block(0, 2048);
        first.has_position = 1;
        first.azimuth = 30.;
        let mut second = first;
        second.start = 2048;
        second.end = 4096;
        let mut stale_first = first;
        stale_first.azimuth = -30.;
        let mut stale_second = stale_first;
        stale_second.start = 2048;
        stale_second.end = 4096;
        let labels = || {
            if label.is_empty() {
                Vec::new()
            } else {
                vec![label]
            }
        };
        let winners = TrackSpec {
            blocks: Vec::new(),
            direct: vec![(labels(), first), (labels(), second)],
        };
        let all = TrackSpec {
            blocks: Vec::new(),
            direct: vec![
                (vec!["M-030"], stale_second),
                (vec!["M-030"], stale_first),
                (labels(), second),
                (labels(), first),
            ],
        };
        let (expected, _) = sources_of(&one_object(winners, 4096), Spread::Cloud);
        let (actual, _) = sources_of(&one_object(all, 4096), Spread::Cloud);
        assert_eq!(actual, expected);
        assert_eq!(
            actual[0].label_key,
            mradm_render::routing::canonical_label(label.as_bytes())
        );
    }
}

/// Every cloud slot stays active across the blocks: a diffuse slot's delay line only advances
/// while its source plays, so a window whose pre-roll falls in a slot's gap differs from the full
/// render after the gap (as in the C++ loop).
fn moving_scene(frames: u64) -> RenderScene {
    let mut blocks = Vec::new();
    for (i, az) in [0f32, 40., -60., 120.].into_iter().enumerate() {
        let mut b = object_block(i as u64 * 3000, (i as u64 + 1) * 3000, az);
        b.width = 30. + 10. * i as f32;
        b.diffuse = 0.3;
        blocks.push(b);
    }
    one_object(
        TrackSpec {
            blocks,
            direct: Vec::new(),
        },
        frames,
    )
}

#[test]
fn a_window_equals_the_slice_of_the_full_render() {
    let frames = 12000;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving_scene(frames);
    for spread in [Spread::Cloud, Spread::None, Spread::SafSpreader] {
        for smoothing in [0, 1500] {
            let full = render(
                &s,
                file.path(),
                &Render {
                    spread,
                    smoothing,
                    window: None,
                    pose: None,
                },
            )
            .unwrap();
            assert_eq!(
                full.len(),
                frames as usize * 2,
                "the spreader tail keeps the length"
            );
            for (start, count) in [(0, 500), (2500, 3333), (5000, 100_000), (11_999, 1)] {
                let part = render(
                    &s,
                    file.path(),
                    &Render {
                        spread,
                        smoothing,
                        window: Some(Window { start, count }),
                        pose: None,
                    },
                )
                .unwrap();
                let end = (start + count).min(frames) as usize;
                let want = words(&full[start as usize * 2..end * 2]);
                let got = words(&part);
                let first = got.iter().zip(&want).position(|(a, b)| a != b);
                assert!(
                    got == want,
                    "spread {spread:?} smoothing {smoothing} window {start}+{count}: {} vs {} frames, first difference at {first:?}",
                    got.len() / 2,
                    want.len() / 2
                );
            }
        }
    }
}

#[test]
fn worker_threads_do_not_change_the_sources() {
    let frames = 4096;
    let samples = sine(frames, 0.5);
    let s = moving_scene(frames as u64);
    let hrtf = Arc::new(binaural_hrtf::build(Dataset::kemar(), 1024, false).unwrap());
    let plan =
        binaural_render::prepare(&s, Arc::clone(&hrtf), Spread::Cloud, 0, &mut |_, _| {}).unwrap();
    let rotation = mradm_dsp::scene_math::Rotation::new([20., 5., 0.]).unwrap();
    let mut outputs = Vec::new();
    for threads in [0, 3] {
        let mut bank = Bank::new(&hrtf, plan.sources.len(), 1024, threads).unwrap();
        let mut out = Vec::new();
        for start in (0..frames).step_by(1024) {
            bank.render(
                &plan.sources,
                &hrtf.filters,
                &samples[start..start + 1024],
                1,
                start as u64,
                1024,
                0,
                Some(&rotation),
                &|i| (i % 2 == 0).then_some(false),
            )
            .unwrap();
            for st in &bank.states {
                out.extend(words(&st.left));
                out.extend(words(&st.right));
            }
        }
        outputs.push(out);
    }
    assert_eq!(outputs[0], outputs[1]);
}

#[test]
fn a_nonfinite_direction_is_an_error() {
    let mut b = object_block(0, 4096, f32::NAN);
    b.width = 0.;
    let s = one_object(
        TrackSpec {
            blocks: vec![b],
            direct: Vec::new(),
        },
        4096,
    );
    let file = input(&sine(4096, 0.5));
    let r = Render {
        spread: Spread::None,
        smoothing: 0,
        window: None,
        pose: None,
    };
    assert_eq!(
        render(&s, file.path(), &r).err().as_deref(),
        Some("Invalid HRTF query arguments")
    );
}

/// The LCG signal of `renders.jsonl`, as floats the PCM24 writer stores exactly.
fn signal(n: usize) -> Vec<f32> {
    let mut x = 1u64;
    (0..n)
        .map(|_| {
            x = (x * 1103515245 + 12345) % (1 << 31);
            let v = ((((x >> 7) & 0xFF_FFFF) as i32) - 0x80_0000) >> 2;
            // The writer scales by 8388607 and truncates toward zero: aim at the middle.
            ((f64::from(v) + 0.5 * f64::from(v.signum())) / 8_388_607.) as f32
        })
        .collect()
}

#[test]
fn renders_reproduce_the_cpp_cli() {
    let temp =
        std::env::temp_dir().join(format!("mradm-render-binaural-pcm-{}", std::process::id()));
    std::fs::create_dir_all(&temp).unwrap();
    let mut files: HashMap<String, PathBuf> = HashMap::new();
    let records = lines(&dir().join("renders.jsonl"));
    for (index, c) in records.iter().enumerate() {
        let name = get(c, "scene").unwrap().as_str().unwrap();
        let path = files
            .entry(name.to_owned())
            .or_insert_with(|| write_scene_wav(name, &temp, true))
            .clone();
        let scene = import(
            path.to_str().unwrap().as_bytes(),
            ImportOptions {
                input_layout: None,
                input_channel_labels: &[],
            },
        )
        .unwrap();
        let floats = |v: &Value| -> Vec<f64> {
            v.as_array()
                .unwrap()
                .iter()
                .map(|x| x.as_f64().unwrap())
                .collect()
        };
        let window = get(c, "window")
            .filter(|v| !matches!(v, Value::Null))
            .map(|w| {
                let w = floats(w);
                Window {
                    start: w[0] as u64,
                    count: w[1] as u64,
                }
            });
        let pose = get(c, "pose")
            .filter(|v| !matches!(v, Value::Null))
            .map(|p| {
                let p = floats(p);
                [p[0] as f32, p[1] as f32, p[2] as f32]
            });
        let spread = match get(c, "spread").unwrap().as_str().unwrap() {
            "none" => Spread::None,
            "saf-spreader" => Spread::SafSpreader,
            _ => Spread::Cloud,
        };
        let out = render(
            &scene,
            path.to_str().unwrap(),
            &Render {
                spread,
                smoothing: get(c, "smoothing").unwrap().as_f64().unwrap() as u32,
                window,
                pose,
            },
        )
        .unwrap();
        let bytes: Vec<u8> = out.iter().flat_map(|v| v.to_le_bytes()).collect();
        let mut h = 0xcbf2_9ce4_8422_2325u64;
        for b in &bytes {
            h = (h ^ u64::from(*b)).wrapping_mul(0x0100_0000_01b3);
        }
        assert_eq!(
            out.len() / 2,
            get(c, "frames").unwrap().as_f64().unwrap() as usize,
            "render {index} frames"
        );
        assert_eq!(
            format!("{h:016x}"),
            get(c, "pcm").unwrap().as_str().unwrap(),
            "render {index} PCM"
        );
    }
    std::fs::remove_dir_all(&temp).unwrap();
}

/// The OLA preroll rounds its warm-up up to whole blocks; with the built-in HRTF the warm-up is
/// one block, so only a long (SOFA) filter makes the rounding observable.
#[test]
fn the_preroll_covers_the_warm_up_in_whole_blocks() {
    use binaural_render::preroll_start;
    assert_eq!(preroll_start(10_000, 1024, 255), 8 * 1024);
    assert_eq!(preroll_start(10_000, 1024, 1500), 7 * 1024);
    assert_eq!(preroll_start(3072, 1024, 1500), 1024);
    assert_eq!(preroll_start(2048, 1024, 1500), 0);
    assert_eq!(preroll_start(1023, 1024, 0), 0);
    assert_eq!(preroll_start(100_000, 4096, 9000), (24 - 3) * 4096);
}
