//! The EAR backend on hand-built render scenes (PCM consistency 4.5), moved from the C++ tests
//! that edited `AdmScene` directly: same-start blocks leave no trace (S08), live controls bind to
//! the winning label, blocks clipped by the object end still prepare, DirectSpeakers matrix and
//! 22.2 split-power LFE routing through the compensation delay, HOA packs, wav71 ordering, the
//! frequency warnings, errors, and file streaming and windows equal the full offline render.
mod common;

use common::*;
use mradm_adm::records::{Hoa, HoaBlock, HoaChannel, Span};
use mradm_render::ear::{self, DELAY, Prepared, Request};
use mradm_render::offline::{EarPlan, Kernel, Window};
use mradm_render::routing::{Level, MatrixRoute, Override};
use mradm_scene::RenderScene;
use mradm_scene::layouts::Geometry;
use std::sync::Arc;

/// An error's code, message and context.
type Failure = (i32, String, String);

struct Case<'a> {
    layout: &'a str,
    geometry: Geometry,
    matrix: Option<&'a [MatrixRoute<'a>]>,
    split_power: bool,
}

impl Case<'_> {
    fn new(layout: &str) -> Case<'_> {
        Case {
            layout,
            geometry: Geometry::Standard,
            matrix: None,
            split_power: false,
        }
    }

    fn prepare_logged(
        &self,
        scene: &RenderScene,
    ) -> (Result<Prepared, Failure>, Vec<(Level, String)>) {
        let mut logs = Vec::new();
        let result = ear::prepare(
            &Request {
                scene,
                layout_id: self.layout.as_bytes(),
                geometry: self.geometry,
                matrix_routing: self.matrix.is_some(),
                matrix: self.matrix,
                input_path: b"input.wav",
                split_power: self.split_power,
            },
            &mut |level, text| logs.push((level, String::from_utf8(text).unwrap())),
        )
        .map_err(|e| {
            (
                e.code,
                String::from_utf8(e.message).unwrap(),
                String::from_utf8(e.context).unwrap(),
            )
        });
        (result, logs)
    }

    fn prepare(&self, scene: &RenderScene) -> Prepared {
        self.prepare_logged(scene)
            .0
            .unwrap_or_else(|e| panic!("{}: {e:?}", self.layout))
    }

    fn warnings(&self, scene: &RenderScene) -> Vec<String> {
        let (result, logs) = self.prepare_logged(scene);
        result.unwrap();
        logs.into_iter()
            .filter(|(level, _)| *level == Level::Warning)
            .map(|(_, text)| text)
            .collect()
    }

    fn error(&self, scene: &RenderScene) -> Failure {
        self.prepare_logged(scene).0.err().unwrap()
    }
}

fn kernel(p: &Prepared, layout: &str) -> Kernel {
    Kernel::Ear(Arc::new(EarPlan {
        mix: Arc::clone(&p.plan),
        filters: Arc::clone(&p.filters),
        wav71: layout == "wav71",
    }))
}

fn offline(p: &Prepared, layout: &str, input: &Temp, frames: u64, smoothing: u64) -> Vec<f32> {
    render_offline(kernel(p, layout), input, p.outputs, frames, smoothing, None).unwrap()
}

fn streamed(
    p: &Prepared,
    layout: &str,
    input: &Temp,
    frames: u64,
    smoothing: u64,
    chunks: &[usize],
    overrides: &[Override],
) -> Vec<f32> {
    let (pcm, error) = render_stream(
        &kernel(p, layout),
        &p.bindings,
        input,
        p.outputs,
        frames,
        smoothing,
        chunks,
        overrides,
    );
    assert!(error.is_none(), "{error:?}");
    pcm
}

fn object_track(blocks: Vec<mradm_adm::records::ObjectBlock>) -> TrackSpec {
    TrackSpec {
        blocks,
        ..TrackSpec::default()
    }
}

fn direct_track(labels: Vec<&'static str>, block: mradm_adm::records::DirectBlock) -> TrackSpec {
    TrackSpec {
        direct: vec![(labels, block)],
        ..TrackSpec::default()
    }
}

/// Adds an HOA pack of one W channel on input channel 0.
fn add_hoa(scene: &mut RenderScene, normalization: &str, pack: Hoa, block_gain: f32, frames: u64) {
    let s = &mut scene.snapshot;
    let string = |s: &mut mradm_adm::Snapshot, v: &str| {
        s.strings.push(v.to_owned());
        (s.strings.len() - 1) as u32
    };
    let normalization = string(s, normalization);
    let id = string(s, "AO_hoa");
    let uid = string(s, "ATU_hoa");
    s.hoa_blocks.push(HoaBlock {
        gain: block_gain,
        start: 0,
        end: frames,
        ..HoaBlock::default()
    });
    s.hoa_channels.push(HoaChannel {
        uid,
        channel: 0,
        order: 0,
        degree: 0,
        blocks: Span {
            offset: s.hoa_blocks.len() - 1,
            len: 1,
        },
    });
    s.hoa.push(Hoa {
        object_id: id,
        pack_id: id,
        normalization,
        channels: Span {
            offset: s.hoa_channels.len() - 1,
            len: 1,
        },
        end: frames,
        ..pack
    });
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
            let expected = offline(&winners, layout, &file, FRAMES, smoothing);
            assert!(
                expected.iter().any(|v| *v != 0.),
                "{label}: winners produce output"
            );
            assert_eq!(
                bits(&offline(&all, layout, &file, FRAMES, smoothing)),
                bits(&expected),
                "{label}: offline PCM"
            );
            let chunks = [7, 333, 1024, 19];
            assert_eq!(
                bits(&streamed(
                    &all,
                    layout,
                    &file,
                    FRAMES,
                    smoothing,
                    &chunks,
                    &[]
                )),
                bits(&streamed(
                    &winners,
                    layout,
                    &file,
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
    let block = |label: &'static str, azimuth| {
        let mut b = direct_block(0, FRAMES);
        b.has_position = 1;
        b.azimuth = azimuth;
        (vec![label], b)
    };
    let track = TrackSpec {
        direct: vec![block("M-030", -30.), block("M+030", 30.)],
        ..TrackSpec::default()
    };
    let p = Case::new("0+5+0").prepare(&scene(single("AO_1", 1., track), FRAMES));
    assert_eq!(p.bindings[0].label_key, b"M+030");
    let mute = |label: &'static str| Override {
        object_id: b"AO_1",
        speaker_label: label.as_bytes(),
        mute: true,
        gain_db: 0.,
        head_locked: None,
    };
    let chunks = [1024];
    let unmodified = streamed(&p, "0+5+0", &file, FRAMES, 0, &chunks, &[]);
    assert!(unmodified.iter().any(|v| *v != 0.));
    assert_eq!(
        bits(&streamed(
            &p,
            "0+5+0",
            &file,
            FRAMES,
            0,
            &chunks,
            &[mute("M-030")]
        )),
        bits(&unmodified),
        "the overwritten label is not bound"
    );
    // After the 20 ms de-zipper and the compensation delay the channel is silent.
    let muted = streamed(&p, "0+5+0", &file, FRAMES, 0, &chunks, &[mute("m_+030")]);
    assert!(muted[2048 * p.outputs..].iter().all(|v| *v == 0.));
}

#[test]
fn blocks_clipped_by_the_object_end_prepare() {
    for direct in [false, true] {
        for end in [0, 48_000, 144_000] {
            let mut track = TrackSpec::default();
            for start in [0, 96_000] {
                let block_end = if start == 0 { 96_000 } else { 144_000 };
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
            Case::new("0+2+0").prepare(&scene(objects, 144_000));
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
        matrix: Some(&dynamic),
        ..Case::new("0+5+0")
    };
    let (prepared, logs) =
        case.prepare_logged(&scene(single("AO_matrix_dynamic", 0.8, track), FRAMES));
    let prepared = prepared.unwrap();
    assert!(logs.contains(&(Level::Info, "DirectSpeakers routing: matrix".to_owned())));
    let reference = offline(&prepared, "0+5+0", &file, FRAMES, 0);
    let delay = DELAY as u64;
    let mut max_error = 0f64;
    for frame in 0..FRAMES {
        let mut expected = [0f32; 6];
        if frame >= delay {
            let source = (frame - delay) as usize;
            let x = samples[source];
            if (source as u64) < FIRST {
                expected[0] = x * 0.8 * 0.5 * 0.5;
                expected[1] = x * 0.8 * 0.5 * 0.866_025_4;
            } else if (source as u64) < SECOND {
                expected[2] = x * 0.8 * 0.25;
            }
        }
        for (channel, e) in expected.iter().enumerate() {
            let actual = reference[frame as usize * 6 + channel];
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
    let boundary = (FIRST + delay) as usize;
    assert_ne!(reference[(boundary - 1) * 6], 0.);
    assert_ne!(reference[boundary * 6 + 2], 0.);
    let uniform = streamed(&prepared, "0+5+0", &file, FRAMES, 0, &[1024], &[]);
    assert_eq!(
        bits(&uniform),
        bits(&reference),
        "stream matches offline output"
    );
    assert_eq!(
        bits(&streamed(
            &prepared,
            "0+5+0",
            &file,
            FRAMES,
            0,
            &[333, 1000, 7, 512],
            &[]
        )),
        bits(&uniform),
        "stream is independent of pull chunking"
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
    let case = Case {
        matrix: Some(&lfe_matrix),
        split_power: true,
        ..Case::new("9+10+3")
    };
    let prepared = case.prepare(&scene(
        single("AO_matrix_lfe", 0.7, direct_track(vec!["RC_LFE"], lfe)),
        FRAMES,
    ));
    let rendered = offline(&prepared, "9+10+3", &file, FRAMES, 0);
    let gain = 0.7f32 * 0.6 * std::f32::consts::FRAC_1_SQRT_2;
    for frame in 0..FRAMES as usize {
        let x = if frame >= DELAY {
            samples[frame - DELAY]
        } else {
            0.
        };
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
fn hoa_packs_decode_with_their_gains_warnings_and_errors() {
    const FRAMES: u64 = 4096;
    let file = input(&sine(FRAMES as usize, 0.25));
    let empty = || scene(Vec::new(), FRAMES);
    let case = Case::new("0+5+0");
    let render = |pack: Hoa, block_gain| {
        let mut s = empty();
        add_hoa(&mut s, "SN3D", pack, block_gain, FRAMES);
        let p = case.prepare(&s);
        assert_eq!(p.bindings.len(), 1);
        assert!(
            p.bindings[0].object_id.is_empty(),
            "HOA channels bind no object"
        );
        offline(&p, "0+5+0", &file, FRAMES, 0)
    };
    let unity = render(
        Hoa {
            gain: 1.,
            ..Hoa::default()
        },
        1.,
    );
    assert!(unity.iter().any(|v| *v != 0.));
    let halved: Vec<f32> = unity.iter().map(|v| v * 0.5).collect();
    assert_eq!(
        bits(&render(
            Hoa {
                gain: 0.5,
                ..Hoa::default()
            },
            1.
        )),
        bits(&halved)
    );
    assert_eq!(
        bits(&render(
            Hoa {
                gain: 1.,
                ..Hoa::default()
            },
            0.5
        )),
        bits(&halved)
    );

    let mut muted = empty();
    add_hoa(
        &mut muted,
        "SN3D",
        Hoa {
            gain: 1.,
            mute: 1,
            ..Hoa::default()
        },
        1.,
        FRAMES,
    );
    assert_eq!(
        case.warnings(&muted),
        ["no renderable tracks found (all muted?), writing silence"]
    );

    let mut flagged = empty();
    add_hoa(
        &mut flagged,
        "N3D",
        Hoa {
            gain: 1.,
            screen_ref: 1,
            nfc_ref_dist: 2.,
            ..Hoa::default()
        },
        1.,
        FRAMES,
    );
    assert_eq!(
        case.warnings(&flagged),
        [
            "screenRef for HOA is not implemented; ignoring",
            "nfcRefDist is not implemented; ignoring"
        ]
    );

    let mut unknown = empty();
    add_hoa(
        &mut unknown,
        "Ambix",
        Hoa {
            gain: 1.,
            ..Hoa::default()
        },
        1.,
        FRAMES,
    );
    assert_eq!(
        case.error(&unknown),
        (
            2,
            "Unknown HOA normalization".to_owned(),
            "layout=0+5+0".to_owned()
        )
    );
}

#[test]
fn wav71_is_0_7_0_in_wave_order() {
    const FRAMES: u64 = 4096;
    let file = input(&sine(FRAMES as usize, 0.25));
    let mut block = object_block(0, FRAMES, 100.);
    block.diffuse = 0.3;
    let s = scene(single("AO_1", 1., object_track(vec![block])), FRAMES);
    let ear = offline(&Case::new("0+7+0").prepare(&s), "0+7+0", &file, FRAMES, 0);
    let wave = offline(&Case::new("wav71").prepare(&s), "wav71", &file, FRAMES, 0);
    let mut expected = ear.clone();
    for frame in expected.as_chunks_mut::<8>().0 {
        frame.swap(4, 6);
        frame.swap(5, 7);
    }
    assert_ne!(
        bits(&expected),
        bits(&ear),
        "the side and rear pairs carry signal"
    );
    assert_eq!(bits(&wave), bits(&expected));
}

#[test]
fn frequency_indications_keep_the_cpp_warnings() {
    const FRAMES: u64 = 2048;
    let mut high = direct_block(0, FRAMES);
    high.has_position = 1;
    high.low_pass_present = 1;
    high.low_pass = 1000.;
    assert_eq!(
        Case::new("0+5+0").warnings(&scene(
            single("AO_1", 1., direct_track(vec!["M+000"], high)),
            FRAMES
        )),
        [
            "frequency indication present but does not indicate an LFE channel",
            "LFE indication from frequency element does not match speakerLabel"
        ]
    );
    let lfe = direct_block(0, FRAMES);
    assert!(
        Case::new("0+5+0")
            .warnings(&scene(
                single("AO_1", 1., direct_track(vec!["LFE1"], lfe)),
                FRAMES
            ))
            .is_empty()
    );
}

#[test]
fn lfe_blocks_reach_their_lfe_channel() {
    const FRAMES: u64 = 2048;
    let file = input(&sine(FRAMES as usize, 0.25));
    // Each LFE label names its 22.2 LFE; the other layouts have a single LFE.
    for (labels, channel) in [
        (vec!["LFE2"], 9),
        (vec!["LFER"], 9),
        (vec!["Sub", "lfe_2"], 9),
        (vec!["LFE"], 3),
        (vec!["LFEL"], 3),
    ] {
        let mut block = direct_block(0, FRAMES);
        block.has_position = 1;
        let label = format!("{labels:?}");
        let s = scene(single("AO_1", 1., direct_track(labels, block)), FRAMES);
        let p = Case::new("9+10+3").prepare(&s);
        let pcm = offline(&p, "9+10+3", &file, FRAMES, 0);
        for c in 0..24 {
            let active = pcm.iter().skip(c).step_by(24).any(|v| *v != 0.);
            assert_eq!(active, c == channel, "{label}: channel {c}");
        }
    }
}

#[test]
fn errors_keep_the_cpp_text() {
    const FRAMES: u64 = 2048;
    let s = scene(
        single("AO_1", 1., object_track(vec![object_block(0, FRAMES, 0.)])),
        FRAMES,
    );
    assert_eq!(
        Case::new("7.7.7").error(&s),
        (
            2,
            "Unknown EAR layout".to_owned(),
            "layout=7.7.7".to_owned()
        )
    );
    assert_eq!(
        Case {
            geometry: Geometry::Apple,
            ..Case::new("7.7.7")
        }
        .error(&s),
        (
            2,
            "unsupported output layout '7.7.7': Apple speaker geometry is unavailable for layout '7.7.7'"
                .to_owned(),
            "layout=7.7.7".to_owned()
        )
    );
    let mut logs = Vec::new();
    let missing = ear::prepare(
        &Request {
            scene: &s,
            layout_id: b"0+5+0",
            geometry: Geometry::Standard,
            matrix_routing: true,
            matrix: None,
            input_path: b"input.wav",
            split_power: false,
        },
        &mut |level, text| logs.push((level, text)),
    )
    .err()
    .unwrap();
    assert_eq!(
        (
            missing.code,
            missing.message.as_slice(),
            missing.context.as_slice()
        ),
        (
            1,
            &b"DirectSpeakers matrix routing requires a parsed matrix"[..],
            &b""[..]
        )
    );
    assert_eq!(
        logs,
        [(Level::Info, b"speaker geometry: standard".to_vec())]
    );

    let mut outside = scene(
        single("AO_1", 1., object_track(vec![object_block(0, FRAMES, 0.)])),
        FRAMES,
    );
    outside.snapshot.tracks[0].channel = 1;
    assert_eq!(
        Case::new("0+5+0").error(&outside),
        (
            4,
            "track channel index 1 is outside input channel count 1".to_owned(),
            "input=input.wav".to_owned()
        )
    );
}

/// Deterministic noise, so the decorrelation history matters.
fn noise(frames: usize) -> Vec<f32> {
    let mut state = 0x1234_5678u32;
    (0..frames)
        .map(|_| {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            ((state >> 8) as f32 / (1 << 24) as f32 - 0.5) * 0.5
        })
        .collect()
}

#[test]
fn file_streaming_and_windows_equal_the_full_offline_render() {
    const FRAMES: u64 = 4103;
    let file = input(&noise(FRAMES as usize));
    let mut first = object_block(0, 1500, 30.);
    first.diffuse = 0.6;
    first.width = 40.;
    let mut second = object_block(1500, FRAMES, -60.);
    second.diffuse = 0.25;
    second.interpolation_present = 1;
    second.interpolation = 480;
    for layout in ["0+5+0", "wav71"] {
        let p = Case::new(layout).prepare(&scene(
            single("AO_1", 0.75, object_track(vec![first, second])),
            FRAMES,
        ));
        for smoothing in [0, 1024, 1500] {
            let full = offline(&p, layout, &file, FRAMES, smoothing);
            assert_eq!(full.len(), FRAMES as usize * p.outputs);
            for chunks in [&[1024][..], &[333, 1000, 512, 7]] {
                assert_eq!(
                    bits(&streamed(&p, layout, &file, FRAMES, smoothing, chunks, &[])),
                    bits(&full),
                    "{layout} smoothing={smoothing} chunks={chunks:?}"
                );
            }
            // The decorrelation overlap and the compensation delay are refilled by one aligned
            // pre-roll block.
            for (start, count) in [(0, 100), (1023, u64::MAX), (2500, 1000), (3100, 7)] {
                let window = render_offline(
                    kernel(&p, layout),
                    &file,
                    p.outputs,
                    FRAMES,
                    smoothing,
                    Some(Window { start, count }),
                )
                .unwrap();
                let end = start.saturating_add(count).min(FRAMES) as usize;
                assert_eq!(
                    bits(&window),
                    bits(&full[start as usize * p.outputs..end * p.outputs]),
                    "{layout} smoothing={smoothing} window={start}+{count}"
                );
            }
        }
    }
}
