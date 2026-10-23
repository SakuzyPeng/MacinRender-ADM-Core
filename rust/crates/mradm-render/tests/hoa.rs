//! The HOA encoder backend on hand-built render scenes (PCM consistency 4.5), moved from the C++
//! tests that edited `AdmScene` directly: same-start blocks leave no trace (S08), live controls
//! bind to the winning label, a later track rebinds its channel, the DirectSpeakers label
//! fallback, errors and warnings, premature end of input, the windowed pre-roll, and file
//! streaming equals the offline driver.
mod common;

use common::*;
use mradm_dsp::hoa::CHANNELS;
use mradm_render::hoa::{self, Prepared, Request, label_direction};
use mradm_render::offline::{HOA_SHORT_READ, Kernel, Location, Window};
use mradm_render::routing::{Level, Override};
use mradm_render::vbap::Binding;
use mradm_scene::RenderScene;
use std::sync::Arc;

/// An error's code, message and context.
type Failure = (i32, String, String);

fn prepare_logged(scene: &RenderScene) -> (Result<Prepared, Failure>, Vec<String>) {
    let mut logs = Vec::new();
    let result = hoa::prepare(
        &Request {
            scene,
            layout_id: b"hoa3",
            split_power: false,
        },
        &mut |level, text| {
            assert_eq!(level, Level::Warning);
            logs.push(String::from_utf8(text).unwrap());
        },
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

fn prepare(scene: &RenderScene) -> Prepared {
    let (result, _) = prepare_logged(scene);
    result.unwrap_or_else(|e| panic!("{e:?}"))
}

fn kernel(p: &Prepared) -> Kernel {
    Kernel::Hoa(Arc::clone(&p.plan))
}

fn offline(p: &Prepared, input: &Temp, frames: u64, smoothing: u64) -> Vec<f32> {
    render_offline(kernel(p), input, CHANNELS, frames, smoothing, None).unwrap()
}

fn streamed(
    p: &Prepared,
    input: &Temp,
    frames: u64,
    smoothing: u64,
    chunks: &[usize],
    overrides: &[Override],
) -> Vec<f32> {
    let (pcm, error) = render_stream(
        &kernel(p),
        &p.bindings,
        input,
        CHANNELS,
        frames,
        smoothing,
        chunks,
        overrides,
    );
    assert!(error.is_none(), "{error:?}");
    pcm
}

/// Deterministic noise, so the diffuse decorrelation history matters.
fn noise(frames: usize) -> Vec<f32> {
    let mut state = 0x1234_5678u32;
    (0..frames)
        .map(|_| {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            ((state >> 8) as f32 / (1 << 24) as f32 - 0.5) * 0.5
        })
        .collect()
}

/// Two blocks crossing a 1024-frame block edge, diffuse and extended.
fn diffuse_track(frames: u64) -> TrackSpec {
    let mut first = object_block(0, 1500, 30.);
    first.diffuse = 0.6;
    first.width = 40.;
    let mut second = object_block(1500, frames, -60.);
    second.diffuse = 0.25;
    second.interpolation_present = 1;
    second.interpolation = 480;
    TrackSpec {
        blocks: vec![first, second],
        ..TrackSpec::default()
    }
}

fn rms(pcm: &[f32], channel: usize) -> f64 {
    let frames = pcm.len() / CHANNELS;
    let sum: f64 = (0..frames)
        .map(|f| f64::from(pcm[f * CHANNELS + channel]).powi(2))
        .sum();
    (sum / frames as f64).sqrt()
}

#[test]
fn speaker_labels_name_directions() {
    assert_eq!(label_direction(b"M+030"), Some([30., 0.]));
    assert_eq!(label_direction(b"U-045"), Some([-45., 30.]));
    assert_eq!(label_direction(b"T+000"), Some([0., 90.]));
    assert_eq!(label_direction(b"B+110"), Some([110., -30.]));
    for rejected in [&b"M+30"[..], b"M+030foo", b"X+030", b"M*030", b"m+030", b""] {
        assert_eq!(label_direction(rejected), None, "{rejected:?}");
    }
}

#[test]
fn same_start_blocks_leave_no_trace() {
    const FRAMES: u64 = 4096;
    let file = input(&sine(FRAMES as usize, 0.25));
    for direct in [false, true] {
        for smoothing in [0, 1024] {
            let label = format!("direct={direct} smoothing={smoothing}");
            let winners = prepare(&scene(
                single("AO_1", 1., same_start_track(direct, false)),
                FRAMES,
            ));
            let all = prepare(&scene(
                single("AO_1", 1., same_start_track(direct, true)),
                FRAMES,
            ));
            let expected = offline(&winners, &file, FRAMES, smoothing);
            assert!(
                expected.iter().any(|v| *v != 0.),
                "{label}: winners produce output"
            );
            assert_eq!(
                bits(&offline(&all, &file, FRAMES, smoothing)),
                bits(&expected),
                "{label}: offline PCM"
            );
            let chunks = [7, 333, 1024, 19];
            assert_eq!(
                bits(&streamed(&all, &file, FRAMES, smoothing, &chunks, &[])),
                bits(&streamed(&winners, &file, FRAMES, smoothing, &chunks, &[])),
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
        direct: vec![block("M+030", 30.), block("M-030", -30.)],
        ..TrackSpec::default()
    };
    let p = prepare(&scene(single("AO_1", 1., track), FRAMES));
    assert_eq!(p.bindings[0].label_key, b"M-030");
    let mute = |label: &'static str| Override {
        object_id: b"AO_1",
        speaker_label: label.as_bytes(),
        mute: true,
        gain_db: 0.,
        head_locked: None,
    };
    let chunks = [1024];
    let unmodified = streamed(&p, &file, FRAMES, 0, &chunks, &[]);
    assert!(unmodified.iter().any(|v| *v != 0.));
    assert_eq!(
        bits(&streamed(&p, &file, FRAMES, 0, &chunks, &[mute("M+030")])),
        bits(&unmodified),
        "the overwritten label is not bound"
    );
    // The 20 ms de-zipper ramps down from unity; after it the channel is silent.
    let muted = streamed(&p, &file, FRAMES, 0, &chunks, &[mute("m_-030")]);
    assert!(muted[1024 * CHANNELS..].iter().all(|v| *v == 0.));
}

#[test]
fn a_later_track_rebinds_its_channel() {
    const FRAMES: u64 = 2048;
    let file = input(&sine(FRAMES as usize, 0.25));
    let mut labelled = direct_block(0, FRAMES);
    labelled.has_position = 1;
    let objects = vec![
        ObjectSpec {
            id: "AO_1",
            gain: 1.,
            end: u64::MAX,
            tracks: vec![TrackSpec {
                direct: vec![(vec!["M+000"], labelled)],
                ..TrackSpec::default()
            }],
        },
        ObjectSpec {
            id: "AO_2",
            gain: 0.5,
            end: u64::MAX,
            tracks: vec![TrackSpec {
                blocks: vec![object_block(0, FRAMES, 90.)],
                ..TrackSpec::default()
            }],
        },
    ];
    let p = prepare(&scene(objects, FRAMES));
    // The binding follows the last track on the channel and drops the earlier speaker label,
    // while both tracks' blocks join the channel's timeline.
    assert_eq!(
        p.bindings,
        vec![Binding {
            input_channel: 0,
            object_id: b"AO_2".to_vec(),
            label_key: Vec::new(),
        }]
    );
    let pcm = offline(&p, &file, FRAMES, 0);
    assert!(rms(&pcm, 0) > 0.05, "W");
}

#[test]
fn direct_speakers_without_a_position_fall_back_to_their_label() {
    const FRAMES: u64 = 2048;
    let file = input(&sine(FRAMES as usize, 0.25));
    let positioned = |azimuth, elevation, has_position| {
        let mut b = direct_block(0, FRAMES);
        b.azimuth = azimuth;
        b.elevation = elevation;
        b.has_position = has_position;
        b
    };
    let with = |labels: Vec<&'static str>, block| {
        scene(
            single(
                "AO_1",
                1.,
                TrackSpec {
                    direct: vec![(labels, block)],
                    ..TrackSpec::default()
                },
            ),
            FRAMES,
        )
    };
    let expected = offline(
        &prepare(&with(vec!["U-045"], positioned(-45., 30., 1))),
        &file,
        FRAMES,
        0,
    );
    let fallback = offline(
        &prepare(&with(vec!["X", "U-045"], positioned(7., 7., 0))),
        &file,
        FRAMES,
        0,
    );
    assert_eq!(bits(&fallback), bits(&expected));

    let (result, logs) = prepare_logged(&with(vec!["Left"], positioned(0., 0., 0)));
    let skipped = result.unwrap();
    assert_eq!(
        logs,
        ["DirectSpeakers channel 0 has no position and no parseable label; skipping"]
    );
    assert!(offline(&skipped, &file, FRAMES, 0).iter().all(|v| *v == 0.));
}

#[test]
fn errors_and_warnings_keep_the_cpp_text() {
    const FRAMES: u64 = 2048;
    let track = || TrackSpec {
        blocks: vec![object_block(0, FRAMES, 0.)],
        ..TrackSpec::default()
    };
    let base = scene(single("AO_1", 1., track()), FRAMES);
    let mut log = |_, _| {};
    let unsupported = hoa::prepare(
        &Request {
            scene: &base,
            layout_id: b"5.1",
            split_power: false,
        },
        &mut log,
    )
    .err()
    .unwrap();
    assert_eq!(
        (unsupported.code, unsupported.message.as_slice()),
        (
            2,
            &b"unsupported HOA output layout '5.1'; supported: hoa3"[..]
        )
    );

    let mut outside = scene(single("AO_1", 1., track()), FRAMES);
    outside.snapshot.tracks[0].channel = 1;
    assert_eq!(
        prepare_logged(&outside).0.err().unwrap(),
        (
            4,
            "track channel index 1 is outside input channel count 1".to_owned(),
            String::new()
        )
    );

    let mut silent = scene(single("AO_1", 1., track()), FRAMES);
    silent.info.sample_rate = 0;
    assert_eq!(
        prepare_logged(&silent).0.err().unwrap(),
        (
            1,
            "HOA requires a nonzero sample rate".to_owned(),
            String::new()
        )
    );

    let mut muted = scene(single("AO_1", 1., track()), FRAMES);
    muted.overlay.objects[0].mute = true;
    let (result, logs) = prepare_logged(&muted);
    assert!(result.unwrap().bindings.is_empty());
    assert_eq!(
        logs,
        ["no renderable tracks found (all muted?), writing silence"]
    );

    let mut nonfinite = scene(single("AO_1", f32::INFINITY, track()), FRAMES);
    nonfinite.overlay.objects[0].mute = false;
    let (code, _, context) = prepare_logged(&nonfinite).0.err().unwrap();
    assert_eq!((code, context.as_str()), (1, "HOA DSP"));
}

#[test]
fn a_premature_end_of_input_is_an_io_error() {
    const FILE_FRAMES: usize = 4096;
    let file = input(&noise(FILE_FRAMES));
    let p = prepare(&scene(single("AO_1", 1., diffuse_track(8192)), 8192));
    let offline = render_offline(kernel(&p), &file, CHANNELS, 8192, 0, None)
        .err()
        .unwrap();
    assert_eq!(
        (offline.code, offline.message.as_str(), offline.location),
        (3, HOA_SHORT_READ, Location::Input)
    );
    let (pcm, error) = render_stream(
        &kernel(&p),
        &p.bindings,
        &file,
        CHANNELS,
        8192,
        0,
        &[1024],
        &[],
    );
    let error = error.unwrap();
    assert_eq!(
        (error.code, error.message.as_str(), error.location),
        (3, HOA_SHORT_READ, Location::None)
    );
    assert_eq!(pcm.len(), FILE_FRAMES * CHANNELS, "the complete prefix");
}

#[test]
fn file_streaming_and_windows_equal_the_full_offline_render() {
    const FRAMES: u64 = 4103;
    let file = input(&noise(FRAMES as usize));
    for smoothing in [0, 1024, 1500] {
        let p = prepare(&scene(single("AO_1", 0.75, diffuse_track(FRAMES)), FRAMES));
        let full = offline(&p, &file, FRAMES, smoothing);
        assert_eq!(full.len(), FRAMES as usize * CHANNELS);
        for chunks in [&[1024][..], &[333, 1000, 512, 7]] {
            assert_eq!(
                bits(&streamed(&p, &file, FRAMES, smoothing, chunks, &[])),
                bits(&full),
                "smoothing={smoothing} chunks={chunks:?}"
            );
        }
        // The diffuse delay line is refilled by one aligned pre-roll block.
        for (start, count) in [(0, 100), (1023, u64::MAX), (2500, 1000), (3100, 7)] {
            let window = render_offline(
                kernel(&p),
                &file,
                CHANNELS,
                FRAMES,
                smoothing,
                Some(Window { start, count }),
            )
            .unwrap();
            let end = start.saturating_add(count).min(FRAMES) as usize;
            assert_eq!(
                bits(&window),
                bits(&full[start as usize * CHANNELS..end * CHANNELS]),
                "smoothing={smoothing} window={start}+{count}"
            );
        }
    }
}
