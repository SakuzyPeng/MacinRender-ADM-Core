//! Triple Balance file streaming (PCM consistency 4.6 B): the stream against the offline driver,
//! its seeks and checkpoint LRU, and the live edit rules (the eviction checks moved from the C++
//! `triple_balance_stream_test`, which keeps its public-interface checks).
mod common;

use common::{RATE, Temp, TrackSpec, input, render_offline};
use mradm_render::offline::Kernel;
use mradm_render::routing::Override;
use mradm_render::triple_balance::{self, Mode, Prepared, Request, Routing, Spread};
use mradm_render::triple_balance_stream::{
    DEFAULT_CHECKPOINT_BUDGET, LiveOverride, Source, Stream,
};
use mradm_scene::{RenderScene, policy};

const FRAMES: u64 = 100_037;
const LAYOUT: &str = "9.1.6";
const OUTPUTS: usize = 16;
const CHUNKS: [usize; 7] = [1, 7, 333, 512, 1000, 1024, 4097];

/// A 0.25-size Cartesian object over three events (diffuse when asked).
fn sized_object(diffuse: f32) -> RenderScene {
    let blocks = [
        (0u64, [0f32, 1., 0.]),
        (30_000, [0.5, 0.5, 0.25]),
        (70_000, [-0.5, 0.25, 0.]),
    ]
    .iter()
    .map(|&(start, [x, y, z])| {
        let mut b = common::object_block(start, u64::MAX, 0.);
        b.position.cartesian = 1;
        (b.position.x, b.position.y, b.position.z) = (x, y, z);
        (b.width, b.height, b.depth) = (0.25, 0.25, 0.25);
        b.diffuse = diffuse;
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
        FRAMES,
    )
}

fn prepare(scene: &RenderScene, spread: Spread) -> Prepared {
    let request = Request {
        scene,
        layout_id: LAYOUT.as_bytes(),
        mode: Mode::Standard,
        spread,
        split_power: false,
        standard_geometry: true,
        smoothing_frames: 0,
        routing: Routing::Automatic,
        has_matrix: false,
        input_path: b"input.wav",
    };
    triple_balance::prepare(&request, &mut |_, _| {})
        .result
        .unwrap()
}

fn open(prepared: &Prepared, spread: Spread, source: &Temp, budget: usize) -> Stream {
    let (mix, session) = Source::of(&prepared.plan).unwrap();
    let source_plan = Source {
        mix,
        session,
        bindings: prepared.bindings.clone().into(),
        size_tracks: prepared.size_tracks.clone().into(),
        spread_none: spread == Spread::None,
        inputs: 1,
        outputs: prepared.outputs,
        frames: FRAMES,
        sample_rate: RATE,
    };
    Stream::open(source.path(), source_plan, 5, budget).unwrap()
}

fn offline(scene: &RenderScene, source: &Temp) -> Vec<f32> {
    let prepared = prepare(scene, Spread::Automatic);
    render_offline(
        Kernel::TripleBalance(prepared.plan),
        source,
        OUTPUTS,
        FRAMES,
        0,
        None,
    )
    .unwrap()
}

fn pull(stream: &mut Stream, mut frames: usize, pattern: &[usize]) -> Vec<f32> {
    let mut out = Vec::new();
    let mut index = 0;
    while frames != 0 {
        let want = frames.min(pattern[index % pattern.len()]);
        index += 1;
        let mut block = vec![0.; want * OUTPUTS];
        let got = stream.process(&mut block, want).unwrap();
        if got == 0 {
            break;
        }
        out.extend_from_slice(&block[..got * OUTPUTS]);
        frames -= got;
    }
    out
}

fn seek(stream: &mut Stream, frame: u64) {
    let mut pending = stream.seek_begin(frame).unwrap();
    while pending {
        pending = stream.seek_step().unwrap();
    }
    stream.seek_finish().unwrap();
}

fn slice(pcm: &[f32], start: u64, frames: u64) -> &[f32] {
    let end = (start + frames).min(FRAMES) as usize;
    &pcm[start as usize * OUTPUTS..end * OUTPUTS]
}

fn first_difference(a: &[f32], b: &[f32]) -> Option<usize> {
    a.iter()
        .zip(b)
        .position(|(x, y)| x.to_bits() != y.to_bits())
        .or((a.len() != b.len()).then_some(a.len().min(b.len())))
}

fn object(scale: f32) -> LiveOverride<'static> {
    LiveOverride {
        base: Override {
            object_id: b"AO_1001",
            speaker_label: b"",
            mute: false,
            gain_db: 0.,
            head_locked: None,
        },
        extent_scale: scale,
        extent_width_scale: 1.,
        extent_height_scale: 1.,
        extent_depth_scale: 1.,
        diffuse_scale: 1.,
        divergence_scale: 1.,
    }
}

#[test]
fn the_stream_equals_the_offline_render_for_any_pull_pattern_and_seek() {
    let source = input(&common::sine(FRAMES as usize, 0.5));
    let scene = sized_object(0.);
    let reference = offline(&scene, &source);
    let prepared = prepare(&scene, Spread::Automatic);
    let mut stream = open(
        &prepared,
        Spread::Automatic,
        &source,
        DEFAULT_CHECKPOINT_BUDGET,
    );
    assert!(stream.requires_preroll());
    assert_eq!(stream.outputs(), OUTPUTS);
    for pattern in [&CHUNKS[..], &[1024], &[FRAMES as usize]] {
        seek(&mut stream, 0);
        let pcm = pull(&mut stream, FRAMES as usize + 100, pattern);
        assert_eq!(first_difference(&pcm, &reference), None, "{pattern:?}");
    }
    for frame in [
        0u64, 511, 512, 1023, 1024, 90_001, 16_385, 48_128, 99_999, 100_037,
    ] {
        seek(&mut stream, frame);
        let pcm = pull(&mut stream, 2050, &CHUNKS);
        assert_eq!(
            first_difference(&pcm, slice(&reference, frame, 2050)),
            None,
            "seek {frame}"
        );
    }
    assert_eq!(stream.process(&mut [], 0).unwrap(), 0, "empty process");
    let short = stream.process(&mut [0.; OUTPUTS - 1], 1).unwrap_err();
    assert_eq!(
        (short.code, short.message.as_str()),
        (1, "triple-balance: output buffer is too small")
    );
}

#[test]
fn eviction_and_a_zero_budget_keep_exact_seeks() {
    let source = input(&common::sine(FRAMES as usize, 0.5));
    let scene = sized_object(0.);
    let reference = offline(&scene, &source);
    let prepared = prepare(&scene, Spread::Automatic);
    let per = open(&prepared, Spread::Automatic, &source, 0)
        .checkpoint_bytes()
        .unwrap();
    // Zero, one and two checkpoints: the forced anchor survives the eviction of an older one.
    for budget in [0, per * 3 / 2, per * 2] {
        let mut stream = open(&prepared, Spread::Automatic, &source, budget);
        pull(&mut stream, FRAMES as usize, &CHUNKS);
        for frame in [66_050u64, 1025, 90_001, 66_050, 97_000, 50_000] {
            seek(&mut stream, frame);
            let pcm = pull(&mut stream, 2048, &CHUNKS);
            assert_eq!(
                first_difference(&pcm, slice(&reference, frame, 2048)),
                None,
                "budget {budget}, seek {frame}"
            );
        }
    }
}

#[test]
fn the_seek_anchor_survives_a_full_cache() {
    let source = input(&common::sine(FRAMES as usize, 0.5));
    let prepared = prepare(&sized_object(0.), Spread::Automatic);
    let per = open(&prepared, Spread::Automatic, &source, 0)
        .checkpoint_bytes()
        .unwrap();
    let mut one = open(&prepared, Spread::Automatic, &source, per * 3 / 2);
    seek(&mut one, 66_050);
    // Rendering on past the 96,256 checkpoint would evict the only entry, the anchor.
    pull(&mut one, FRAMES as usize, &CHUNKS);
    assert!(!one.seek_begin(66_050).unwrap(), "restored at the anchor");
    one.seek_finish().unwrap();
    assert!(
        one.seek_begin(97_000).unwrap(),
        "the later block was never kept"
    );
    let mut none = open(&prepared, Spread::Automatic, &source, 0);
    seek(&mut none, 66_050);
    assert!(
        none.seek_begin(66_050).unwrap(),
        "no cache renders from the start"
    );
    let mut two = open(&prepared, Spread::Automatic, &source, per * 2);
    seek(&mut two, 66_050);
    pull(&mut two, FRAMES as usize, &CHUNKS);
    assert!(!two.seek_begin(66_050).unwrap());
    two.seek_finish().unwrap();
    assert!(
        !two.seek_begin(97_000).unwrap(),
        "the 96,256 checkpoint joined the anchor"
    );
}

#[test]
fn an_abandoned_stepped_seek_leaves_the_next_seek_exact() {
    let source = input(&common::sine(FRAMES as usize, 0.5));
    let scene = sized_object(0.);
    let reference = offline(&scene, &source);
    let prepared = prepare(&scene, Spread::Automatic);
    let mut stream = open(
        &prepared,
        Spread::Automatic,
        &source,
        DEFAULT_CHECKPOINT_BUDGET,
    );
    assert!(
        stream.seek_begin(90_001).unwrap(),
        "a size track renders from the start"
    );
    for _ in 0..7 {
        assert!(stream.seek_step().unwrap());
    }
    // The caller cancelled; the next seek starts over.
    seek(&mut stream, 50_000);
    let pcm = pull(&mut stream, 4096, &CHUNKS);
    assert_eq!(
        first_difference(&pcm, slice(&reference, 50_000, 4096)),
        None
    );
    assert!(stream.seek_step().is_err(), "no seek in progress");
    assert!(stream.seek_finish().is_err(), "no seek in progress");
}

#[test]
fn static_edits_equal_the_policy_applied_offline_render() {
    let source = input(&common::sine(FRAMES as usize, 0.5));
    let base = sized_object(0.);
    let prepared = prepare(&base, Spread::Automatic);
    let mut stream = open(
        &prepared,
        Spread::Automatic,
        &source,
        DEFAULT_CHECKPOINT_BUDGET,
    );
    for scale in [0f32, 0.5, 1., 4.] {
        let mut edit = object(scale);
        edit.base.gain_db = -6.;
        stream.validate_overrides(&[edit]).unwrap();
        stream.set_overrides(&[edit]).unwrap();
        seek(&mut stream, 0);
        let text = format!(
            r#"{{"schema":"mradm.semantic-policy.v1","objects":[{{"id":"AO_1001","gain":{{"gain_db":-6}},"extent":{{"scale":{scale}}}}}]}}"#
        );
        let mut scene = sized_object(0.);
        policy::apply(&mut scene, &policy::parse(text.as_bytes()).unwrap());
        let effective = offline(&scene, &source);
        let pcm = pull(&mut stream, FRAMES as usize, &CHUNKS);
        assert_eq!(first_difference(&pcm, &effective), None, "scale {scale}");
        seek(&mut stream, 66_050);
        let pcm = pull(&mut stream, 3000, &CHUNKS);
        assert_eq!(
            first_difference(&pcm, slice(&effective, 66_050, 3000)),
            None,
            "scale {scale} checkpointed seek"
        );
        if scale != 0. {
            let mut invalid = edit;
            invalid.extent_width_scale = 2.;
            stream.set_overrides(&[invalid]).unwrap();
            seek(&mut stream, 66_050);
            let pcm = pull(&mut stream, 3000, &CHUNKS);
            assert_eq!(
                first_difference(&pcm, slice(&effective, 66_050, 3000)),
                None,
                "a rejected edit keeps the accepted target"
            );
        }
    }
    stream.set_overrides(&[]).unwrap();
    seek(&mut stream, 0);
    let pcm = pull(&mut stream, FRAMES as usize, &CHUNKS);
    assert_eq!(
        first_difference(&pcm, &offline(&base, &source)),
        None,
        "cleared"
    );
}

#[test]
fn live_mute_and_size_transitions_are_independent_of_pull_sizes() {
    let source = input(&common::sine(FRAMES as usize, 0.5));
    let scene = sized_object(0.);
    let prepared = prepare(&scene, Spread::Automatic);
    let reference = offline(&scene, &source);
    let mut stream = open(
        &prepared,
        Spread::Automatic,
        &source,
        DEFAULT_CHECKPOINT_BUDGET,
    );
    pull(&mut stream, 4096, &CHUNKS);
    let mut mute = object(1.);
    mute.base.mute = true;
    stream.set_overrides(&[mute]).unwrap();
    let muted = pull(&mut stream, 2048, &CHUNKS);
    assert!(
        muted[1024 * OUTPUTS..].iter().all(|&v| v == 0.),
        "a live mute reaches exact silence"
    );
    stream.set_overrides(&[]).unwrap();
    let resumed = pull(&mut stream, 2048, &CHUNKS);
    assert_eq!(
        first_difference(&resumed[1024 * OUTPUTS..], slice(&reference, 7168, 1024)),
        None,
        "mute and unmute never reset the size state"
    );
    let automate = |pattern: &[usize]| {
        let mut s = open(
            &prepared,
            Spread::Automatic,
            &source,
            DEFAULT_CHECKPOINT_BUDGET,
        );
        let mut pcm = Vec::new();
        for scale in [1f32, 0., 0.5, 4., 1.] {
            s.set_overrides(&[object(scale)]).unwrap();
            let block = pull(&mut s, 4096, pattern);
            assert!(block.iter().all(|v| v.is_finite()));
            pcm.extend(block);
        }
        pcm
    };
    let aligned = automate(&[1024]);
    assert_eq!(first_difference(&aligned, &automate(&CHUNKS)), None);
    assert_ne!(
        first_difference(&aligned, slice(&reference, 0, 5 * 4096)),
        None,
        "the edits reach the output"
    );
}

#[test]
fn override_validation_keeps_the_cpp_order_and_messages() {
    let source = input(&common::sine(FRAMES as usize, 0.5));
    let scene = sized_object(1.);
    let prepared = prepare(&scene, Spread::Automatic);
    let stream = open(
        &prepared,
        Spread::Automatic,
        &source,
        DEFAULT_CHECKPOINT_BUDGET,
    );
    let check = |edit: LiveOverride, code: i32, message: &str| {
        assert_eq!(
            stream.validate_overrides(&[object(1.), edit]),
            Err((code, message.to_owned(), b"AO_1001".to_vec())),
            "{edit:?}"
        );
    };
    const FINITE: &str = "Triple Balance requires finite nonnegative size scales";
    const ONLY: &str =
        "Triple Balance live editing supports gain, mute and linked isotropic size only";
    let mut e = object(1.);
    e.base.gain_db = f32::NAN;
    check(e, 1, FINITE);
    e.base.gain_db = 1000.;
    check(e, 1, FINITE);
    e = object(-1.);
    check(e, 1, FINITE);
    e = object(f32::MAX);
    e.extent_width_scale = 2.;
    check(e, 1, FINITE);
    e = object(1.);
    e.extent_depth_scale = -0.;
    e.extent_height_scale = -1.;
    check(e, 1, FINITE);
    e = object(1.);
    e.extent_height_scale = 2.;
    check(e, 2, ONLY);
    e = object(1.);
    e.diffuse_scale = 0.5;
    check(e, 2, ONLY);
    e = object(1.);
    e.divergence_scale = 0.;
    check(e, 2, ONLY);
    e = object(1.);
    e.base.head_locked = Some(true);
    check(e, 2, ONLY);
    e = object(2.);
    e.base.speaker_label = b"M+030";
    check(e, 2, "Triple Balance size editing applies to Objects only");
    e = object(0.);
    check(
        e,
        2,
        "Triple Balance does not support a pure diffuse point source",
    );
    e = object(1.);
    e.base.head_locked = Some(false);
    e.base.gain_db = -96.;
    assert_eq!(stream.validate_overrides(&[e, object(0.5)]), Ok(()));

    let none = prepare(&sized_object(0.), Spread::None);
    let stream = open(&none, Spread::None, &source, DEFAULT_CHECKPOINT_BUDGET);
    assert_eq!(
        stream.validate_overrides(&[object(2.)]),
        Err((
            2,
            "Triple Balance size editing requires spread=auto".to_owned(),
            b"AO_1001".to_vec()
        ))
    );
    assert_eq!(stream.validate_overrides(&[object(1.)]), Ok(()));
}

#[test]
fn a_live_edit_to_point_settles_on_the_static_point_render() {
    let source = input(&common::sine(FRAMES as usize, 0.5));
    let prepared = prepare(&sized_object(0.), Spread::Automatic);
    let mut fixed = open(
        &prepared,
        Spread::Automatic,
        &source,
        DEFAULT_CHECKPOINT_BUDGET,
    );
    fixed.set_overrides(&[object(0.)]).unwrap();
    let still = pull(&mut fixed, FRAMES as usize, &[1024]);
    for pattern in [&CHUNKS[..], &[1024]] {
        let mut live = open(
            &prepared,
            Spread::Automatic,
            &source,
            DEFAULT_CHECKPOINT_BUDGET,
        );
        let mut pcm = pull(&mut live, 8192, pattern);
        live.set_overrides(&[object(0.)]).unwrap();
        pcm.extend(pull(&mut live, FRAMES as usize, pattern));
        // The size tail joins the point curve within one block after the edit's block.
        let settled = 8192 + 2 * 1024;
        assert_eq!(
            first_difference(&pcm[settled * OUTPUTS..], &still[settled * OUTPUTS..]),
            None,
            "{pattern:?}"
        );
        assert_ne!(
            pcm[8192 * OUTPUTS..],
            still[8192 * OUTPUTS..],
            "the transition is audible"
        );
    }
}
