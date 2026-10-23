//! The VBAP backend on hand-built render scenes (PCM consistency 4.5), moved from the C++ tests
//! that edited `AdmScene` directly: same-start blocks leave no trace (S08), live controls bind to
//! the winning label, blocks clipped by the object end still prepare, DirectSpeakers matrix and
//! 22.2 split-power LFE routing, and file streaming equals the offline driver.
use mradm_adm::Snapshot;
use mradm_adm::records::{DirectBlock, Object, ObjectBlock, Span, Track};
use mradm_render::offline::{self, Driver};
use mradm_render::routing::{MatrixRoute, Override, canonical_label};
use mradm_render::stream::{self, SpeakerStream};
use mradm_render::vbap::{self, Prepared, Request, Routing, Speaker};
use mradm_scene::layouts::{Geometry, find};
use mradm_scene::{Info, RenderScene, SourceKind};
use mradm_wav::{Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::PathBuf;
use std::sync::Arc;

const RATE: u32 = 48000;

struct Temp(PathBuf);

impl Temp {
    fn new(name: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        Self(std::env::temp_dir().join(format!(
            "mradm-render-vbap-{}-{name}-{n}.wav",
            std::process::id()
        )))
    }
    fn path(&self) -> &str {
        self.0.to_str().unwrap()
    }
}

impl Drop for Temp {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

/// A mono float input of `frames` frames.
fn input(samples: &[f32]) -> Temp {
    let temp = Temp::new("input");
    let format = Format {
        channels: 1,
        sample_rate: RATE,
        sample_format: SampleFormat::Float32,
        channel_mask: None,
    };
    let mut writer = Writer::new(
        File::create(&temp.0).unwrap(),
        format,
        WriterOptions::default(),
    )
    .unwrap();
    writer.write_frames(samples).unwrap();
    writer.finish().unwrap();
    temp
}

fn sine(frames: usize, amplitude: f32) -> Vec<f32> {
    (0..frames)
        .map(|i| amplitude * (2. * std::f32::consts::PI * 440. * i as f32 / RATE as f32).sin())
        .collect()
}

/// Objects and DirectSpeakers blocks of one track (labels beside each DirectSpeakers block).
#[derive(Default)]
struct TrackSpec {
    blocks: Vec<ObjectBlock>,
    direct: Vec<(Vec<&'static str>, DirectBlock)>,
}

struct ObjectSpec {
    id: &'static str,
    gain: f32,
    end: u64,
    tracks: Vec<TrackSpec>,
}

/// One object per spec, track `i` on input channel `i` of a mono file.
fn scene(objects: Vec<ObjectSpec>, frames: u64) -> RenderScene {
    let mut s = Snapshot::default();
    let string = |s: &mut Snapshot, v: &str| {
        s.strings.push(v.to_owned());
        (s.strings.len() - 1) as u32
    };
    for object in objects {
        let tracks_offset = s.tracks.len();
        for (i, track) in object.tracks.into_iter().enumerate() {
            let blocks = Span {
                offset: s.object_blocks.len(),
                len: track.blocks.len(),
            };
            s.object_blocks.extend(track.blocks);
            let ds_offset = s.direct_blocks.len();
            for (labels, mut block) in track.direct {
                let offset = s.indices.len();
                for label in &labels {
                    let id = string(&mut s, label);
                    s.indices.push(id);
                }
                block.labels = Span {
                    offset,
                    len: labels.len(),
                };
                s.direct_blocks.push(block);
            }
            let name = format!("ATU_{:08}", s.tracks.len() + 1);
            let uid = string(&mut s, &name);
            s.tracks.push(Track {
                uid,
                channel: i as i32,
                blocks,
                ds_blocks: Span {
                    offset: ds_offset,
                    len: s.direct_blocks.len() - ds_offset,
                },
            });
        }
        let id = string(&mut s, object.id);
        s.objects.push(Object {
            id,
            gain: object.gain,
            end: object.end,
            tracks: Span {
                offset: tracks_offset,
                len: s.tracks.len() - tracks_offset,
            },
            ..Object::default()
        });
    }
    RenderScene::new(
        Info {
            sample_rate: RATE,
            channels: 1,
            frames,
            source_kind: SourceKind::Adm,
            input_layout: String::new(),
            input_channel_labels: Vec::new(),
        },
        s,
    )
}

/// The block defaults of the C++ `SceneObjectBlock`.
fn object_block(start: u64, end: u64, azimuth: f32) -> ObjectBlock {
    let mut b = ObjectBlock {
        start,
        end,
        gain: 1.,
        azimuth_range: 45.,
        ..ObjectBlock::default()
    };
    b.position.azimuth = azimuth;
    b.position.distance = 1.;
    b
}

fn direct_block(start: u64, end: u64) -> DirectBlock {
    DirectBlock {
        start,
        end,
        gain: 1.,
        distance: 1.,
        ..DirectBlock::default()
    }
}

fn speakers(layout: &str) -> Vec<Speaker<'static>> {
    find(layout, Geometry::Standard)
        .unwrap()
        .speakers
        .iter()
        .map(|s| Speaker {
            label: s.label.as_bytes(),
            azimuth: s.azimuth,
            elevation: s.elevation,
            is_lfe: s.is_lfe,
        })
        .collect()
}

struct Case<'a> {
    layout: &'a str,
    routing: Routing,
    matrix: Option<&'a [MatrixRoute<'a>]>,
    split_power: bool,
}

impl Case<'_> {
    fn new(layout: &str) -> Case<'_> {
        Case {
            layout,
            routing: Routing::Label,
            matrix: None,
            split_power: false,
        }
    }

    fn prepare(&self, scene: &RenderScene) -> Prepared {
        let speakers = speakers(self.layout);
        let request = Request {
            scene,
            speakers: &speakers,
            layout_id: self.layout.as_bytes(),
            input_path: b"input.wav",
            spread: true,
            routing: self.routing,
            matrix: self.matrix,
            split_power: self.split_power,
        };
        vbap::prepare(&request, &mut |_, _| {})
            .unwrap_or_else(|e| panic!("{}: {}", self.layout, String::from_utf8_lossy(&e.message)))
    }
}

/// The offline driver's PCM, as `render_speaker_pcm` writes it.
fn offline(p: &Prepared, input: &Temp, outputs: usize, frames: u64, smoothing: u64) -> Vec<f32> {
    let mut driver = Driver::new(
        Driver::open_reader(input.path()).unwrap(),
        offline::Settings {
            plan: Arc::clone(&p.plan),
            inputs: 1,
            outputs,
            frames,
            sample_rate: RATE,
            default_interp_ms: 5,
            smoothing_frames: smoothing,
            window: None,
            meter_window: None,
            stateful: false,
            backend: "saf-vbap".to_owned(),
        },
    )
    .unwrap();
    let mut pcm = Vec::new();
    while let Some(b) = driver.render().unwrap() {
        pcm.extend_from_slice(
            &driver.output()[b.emit_offset * outputs..][..b.emit_frames * outputs],
        );
        driver.commit().unwrap();
    }
    driver.finish().unwrap();
    pcm
}

fn streamed(
    p: &Prepared,
    input: &Temp,
    outputs: usize,
    frames: u64,
    smoothing: u64,
    chunks: &[usize],
    overrides: &[Override],
) -> Vec<f32> {
    let mut s = SpeakerStream::open(
        input.path(),
        Arc::clone(&p.plan),
        Arc::from(p.bindings.clone()),
        stream::Settings {
            inputs: 1,
            outputs,
            frames,
            sample_rate: RATE,
            default_interp_ms: 5,
            smoothing_frames: smoothing,
        },
    )
    .unwrap();
    s.set_overrides(overrides).unwrap();
    let mut pcm = Vec::new();
    for chunk in chunks.iter().cycle() {
        let mut buffer = vec![0.; chunk * outputs];
        let got = s.process(&mut buffer, *chunk).unwrap();
        if got == 0 {
            break;
        }
        pcm.extend_from_slice(&buffer[..got * outputs]);
    }
    pcm
}

fn bits(v: &[f32]) -> Vec<u32> {
    v.iter().map(|x| x.to_bits()).collect()
}

/// Two starts written in reverse chronological order. Each winner is shorter than its overwritten
/// peers and interpolates from its predecessor, so a peer would show up either as the ramp origin
/// or by resuming after the winner ends.
fn same_start_track(direct: bool, include_overwritten: bool) -> TrackSpec {
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
            if direct {
                let mut b = direct_block(start, end);
                b.azimuth = azimuth;
                b.has_position = 1;
                b.gain = if winner { 0.375 } else { 0.9 };
                let mut labels = Vec::new();
                if !winner {
                    // An overwritten LFE1 next to a winning LFE2 must not make 22.2 split-power
                    // routing see two semantic LFE channels.
                    b.low_pass_present = 1;
                    b.low_pass = 120.;
                } else if group == 0 {
                    labels.push("LFE2");
                }
                track.direct.push((labels, b));
                continue;
            }
            let mut b = object_block(start, end, azimuth);
            b.position.elevation = if winner { 0. } else { 30. };
            b.gain = if winner { 0.375 } else { 0.9 };
            if !winner {
                b.width = 40.;
                b.height = 20.;
                b.diffuse = 0.25;
                b.divergence = 0.5;
                b.azimuth_range = 30.;
                b.jump = u32::from(i % 2 == 0);
            }
            track.blocks.push(b);
        }
    }
    track
}

fn single(id: &'static str, gain: f32, track: TrackSpec) -> Vec<ObjectSpec> {
    vec![ObjectSpec {
        id,
        gain,
        end: u64::MAX,
        tracks: vec![track],
    }]
}

#[test]
fn same_start_blocks_leave_no_trace() {
    const FRAMES: u64 = 4096;
    let file = input(&sine(FRAMES as usize, 0.25));
    for (layout, direct, split_power) in [("4+5+0", false, false), ("9+10+3", true, true)] {
        let case = Case {
            split_power,
            ..Case::new(layout)
        };
        let outputs = speakers(layout).len();
        for smoothing in [0, 1024] {
            let label = format!("{layout} direct={direct} smoothing={smoothing}");
            let winners = case.prepare(&scene(
                single("AO_1", 1., same_start_track(direct, false)),
                FRAMES,
            ));
            let all = case.prepare(&scene(
                single("AO_1", 1., same_start_track(direct, true)),
                FRAMES,
            ));
            let expected = offline(&winners, &file, outputs, FRAMES, smoothing);
            let actual = offline(&all, &file, outputs, FRAMES, smoothing);
            assert!(
                expected.iter().any(|v| *v != 0.),
                "{label}: winners produce output"
            );
            assert_eq!(bits(&actual), bits(&expected), "{label}: offline PCM");
            let chunks = [7, 333, 1024, 19];
            assert_eq!(
                bits(&streamed(
                    &all,
                    &file,
                    outputs,
                    FRAMES,
                    smoothing,
                    &chunks,
                    &[]
                )),
                bits(&streamed(
                    &winners,
                    &file,
                    outputs,
                    FRAMES,
                    smoothing,
                    &chunks,
                    &[]
                )),
                "{label}: file-streaming PCM"
            );
        }
    }
}

#[test]
fn live_controls_bind_to_the_winning_label() {
    const FRAMES: u64 = 4096;
    let file = input(&sine(FRAMES as usize, 0.25));
    let case = Case::new("9+10+3");
    let outputs = speakers("9+10+3").len();
    for winning in ["M+030", ""] {
        let block = |start, end, azimuth, label: &'static str| {
            let mut b = direct_block(start, end);
            b.has_position = 1;
            b.azimuth = azimuth;
            (
                if label.is_empty() {
                    vec![]
                } else {
                    vec![label]
                },
                b,
            )
        };
        let first = block(0, 2048, 30., winning);
        let second = block(2048, FRAMES, 30., winning);
        let stale_first = block(0, 2048, -30., "M-030");
        let stale_second = block(2048, FRAMES, -30., "M-030");
        let winners = case.prepare(&scene(
            single(
                "AO_1",
                1.,
                TrackSpec {
                    direct: vec![first.clone(), second.clone()],
                    ..TrackSpec::default()
                },
            ),
            FRAMES,
        ));
        // Reverse chronological order, with a stale label at both starts. An unlabeled winner
        // must also discard the overwritten label and keep whole-object controls.
        let all = case.prepare(&scene(
            single(
                "AO_1",
                1.,
                TrackSpec {
                    direct: vec![stale_second, stale_first, second, first],
                    ..TrackSpec::default()
                },
            ),
            FRAMES,
        ));
        assert_eq!(
            all.bindings[0].label_key,
            canonical_label(winning.as_bytes())
        );
        for label in ["M+030", "M-030", ""] {
            for mute in [false, true] {
                let o = [Override {
                    object_id: b"AO_1",
                    speaker_label: label.as_bytes(),
                    mute,
                    gain_db: -12.,
                    head_locked: None,
                }];
                let what = format!("winner '{winning}' override '{label}' mute={mute}");
                let chunks = [7, 333, 1024, 19];
                let expected = streamed(&winners, &file, outputs, FRAMES, 0, &chunks, &o);
                let actual = streamed(&all, &file, outputs, FRAMES, 0, &chunks, &o);
                let should_mute = mute && (label.is_empty() || label == winning);
                assert_eq!(expected.iter().any(|v| *v != 0.), !should_mute, "{what}");
                assert_eq!(bits(&actual), bits(&expected), "{what}");
            }
        }
    }
}

#[test]
fn blocks_clipped_by_the_object_end_prepare() {
    for direct in [false, true] {
        for end in [0, 48000, 144000] {
            let mut track = TrackSpec::default();
            for start in [0, 96000] {
                let block_end = if start == 0 { 96000 } else { 144000 };
                if direct {
                    track
                        .direct
                        .push((vec!["M+030"], direct_block(start, block_end)));
                } else {
                    track.blocks.push(object_block(start, block_end, 30.));
                }
            }
            let objects = vec![ObjectSpec {
                id: "AO_1001",
                gain: 1.,
                end,
                tracks: vec![track],
            }];
            let prepared = Case::new("0+2+0").prepare(&scene(objects, 144000));
            assert_eq!(prepared.plan.rows(), 1, "direct={direct} end={end}");
        }
    }
}

const FRAMES: u64 = 12317;
const FIRST: u64 = 4099;
const SECOND: u64 = 8203;

#[test]
fn matrix_routing_follows_the_blocks_and_lfe_keeps_its_routing() {
    let samples = sine(FRAMES as usize, 0.2);
    let file = input(&samples);
    let dynamic = [
        MatrixRoute {
            source_label: b"C",
            mute: false,
            targets: vec![(b"L", 0.5), (b"R", 0.866_025_4)],
        },
        MatrixRoute {
            source_label: b"U+000",
            mute: false,
            targets: vec![(b"M+000", 1.)],
        },
        MatrixRoute {
            source_label: b"M+110",
            mute: true,
            targets: vec![],
        },
    ];
    let mut split = direct_block(0, FIRST);
    split.gain = 0.5;
    let mut centre = direct_block(FIRST, SECOND);
    centre.gain = 0.25;
    let mut muted = direct_block(SECOND, FRAMES);
    muted.gain = 0.75;
    let track = TrackSpec {
        direct: vec![
            (vec!["M+000"], split),
            (vec!["U+000"], centre),
            (vec!["M+110"], muted),
        ],
        ..TrackSpec::default()
    };
    let case = Case {
        routing: Routing::Matrix,
        matrix: Some(&dynamic),
        ..Case::new("0+5+0")
    };
    let prepared = case.prepare(&scene(single("AO_matrix_dynamic", 0.8, track), FRAMES));
    let reference = offline(&prepared, &file, 6, FRAMES, 0);
    let mut max_error = 0f64;
    for (frame, x) in samples.iter().enumerate() {
        let mut expected = [0f32; 6];
        if (frame as u64) < FIRST {
            expected[0] = x * 0.8 * 0.5 * 0.5;
            expected[1] = x * 0.8 * 0.5 * 0.866_025_4;
        } else if (frame as u64) < SECOND {
            expected[2] = x * 0.8 * 0.25;
        }
        for (channel, e) in expected.iter().enumerate() {
            let actual = reference[frame * 6 + channel];
            max_error = max_error.max((f64::from(actual) - f64::from(*e)).abs());
            if *e == 0. {
                assert_eq!(actual, 0., "frame {frame} channel {channel} has no leakage");
            }
        }
    }
    assert!(
        max_error < 2e-6,
        "matrix coefficients and gains: {max_error}"
    );
    assert_ne!(reference[(FIRST as usize - 1) * 6], 0.);
    assert_ne!(reference[FIRST as usize * 6 + 2], 0.);

    let uniform = streamed(&prepared, &file, 6, FRAMES, 0, &[1024], &[]);
    assert_eq!(
        bits(&uniform),
        bits(&reference),
        "stream matches offline output"
    );
    assert_eq!(
        bits(&streamed(
            &prepared,
            &file,
            6,
            FRAMES,
            0,
            &[333, 1000, 7, 512],
            &[]
        )),
        bits(&uniform),
        "stream is independent of pull chunking"
    );
    let halved = streamed(
        &prepared,
        &file,
        6,
        FRAMES,
        0,
        &[257, 1024],
        &[Override {
            object_id: b"AO_matrix_dynamic",
            speaker_label: b"",
            mute: false,
            gain_db: -6.020_6,
            head_locked: None,
        }],
    );
    let worst = halved
        .iter()
        .zip(&reference)
        .map(|(a, b)| (f64::from(*a) - f64::from(*b) * 0.5).abs())
        .fold(0., f64::max);
    assert!(
        worst < 2e-6,
        "live gain multiplies the matrix result once: {worst}"
    );

    let lfe_matrix = [MatrixRoute {
        source_label: b"C",
        mute: true,
        targets: vec![],
    }];
    let mut lfe = direct_block(0, FRAMES);
    lfe.low_pass_present = 1;
    lfe.low_pass = 120.;
    lfe.gain = 0.6;
    let track = TrackSpec {
        direct: vec![(vec!["RC_LFE"], lfe)],
        ..TrackSpec::default()
    };
    let case = Case {
        routing: Routing::Matrix,
        matrix: Some(&lfe_matrix),
        split_power: true,
        ..Case::new("9+10+3")
    };
    let prepared = case.prepare(&scene(single("AO_matrix_lfe", 0.7, track), FRAMES));
    let rendered = offline(&prepared, &file, 24, FRAMES, 0);
    let gain = 0.7f32 * 0.6 * std::f32::consts::FRAC_1_SQRT_2;
    for (frame, x) in samples.iter().enumerate() {
        for channel in 0..24 {
            let target = if channel == 3 || channel == 9 {
                x * gain
            } else {
                0.
            };
            let actual = rendered[frame * 24 + channel];
            assert!(
                (f64::from(actual) - f64::from(target)).abs() < 2e-6,
                "LFE bypass keeps split-power routing in matrix mode"
            );
            if target == 0. {
                assert_eq!(actual, 0., "LFE bypass never enters non-LFE targets");
            }
        }
    }
}

#[test]
fn errors_and_warnings_keep_the_cpp_text() {
    let mut position = direct_block(0, 4800);
    position.has_position = 0;
    let objects = single(
        "AO_1",
        1.,
        TrackSpec {
            direct: vec![(vec!["NOT_A_LABEL"], position)],
            ..TrackSpec::default()
        },
    );
    let scene = scene(objects, 4800);
    let speakers = speakers("0+5+0");
    let file = input(&sine(4800, 0.25));
    let fallback = offline(&Case::new("0+5+0").prepare(&scene), &file, 6, 4800, 0);
    let energy = |channel: usize| {
        fallback
            .iter()
            .skip(channel)
            .step_by(6)
            .map(|v| v * v)
            .sum::<f32>()
    };
    assert!(
        energy(2) > 0. && (0..6).all(|c| c == 2 || energy(c) == 0.),
        "front-centre fallback"
    );
    let mut request = Request {
        scene: &scene,
        speakers: &speakers,
        layout_id: b"0+5+0",
        input_path: b"in.wav",
        spread: true,
        routing: Routing::Label,
        matrix: None,
        split_power: true,
    };
    let mut logs = Vec::new();
    vbap::prepare(&request, &mut |level, text| {
        logs.push((level, String::from_utf8(text).unwrap()))
    })
    .unwrap();
    let text: Vec<&str> = logs.iter().map(|(_, t)| t.as_str()).collect();
    assert_eq!(
        text,
        [
            "LFE routing mode 'split-power' only applies to 22.2 ('9+10+3'); using existing LFE routing",
            "DirectSpeakers block has no nominal position; using front-centre (azimuth 0, elevation 0)",
            "DirectSpeakers label 'NOT_A_LABEL' not in output layout — spatializing from nominal position",
        ]
    );
    request.routing = Routing::Matrix;
    let e = vbap::prepare(&request, &mut |_, _| {}).err().unwrap();
    assert_eq!(
        (e.code, e.message.as_slice()),
        (
            1,
            &b"DirectSpeakers matrix routing requires a parsed matrix"[..]
        )
    );
    let mono = [speakers[2]];
    request.speakers = &mono;
    request.routing = Routing::Position;
    let e = vbap::prepare(&request, &mut |_, _| {}).err().unwrap();
    assert_eq!(
        (e.code, e.message.as_slice(), e.context.as_slice()),
        (
            4,
            &b"Rust DSP: Invalid speaker array"[..],
            &b"track_uid=ATU_00000001"[..]
        )
    );
}
