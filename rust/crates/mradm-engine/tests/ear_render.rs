//! Whole EAR renders through the render service (formerly the C++ `ear_render_fixture_test` and
//! `ear_render_smoke_test`, R3 C1): front objects, DirectSpeakers beds and mixed scenes in stereo,
//! 9.1.6 objects and top-side labels, the RC_LFE alias, Cartesian objects and speaker positions,
//! the position offset, the diffuse bus and the 255-sample compensation delay, short inputs, the
//! P2 semantics warnings, channelLock, divergence and two jump blocks in one window. Live gains on
//! the stream and stream / window equality are `mradm-render`'s `ear.rs`; the capability table is
//! `catalog.rs`; the C++ `resolve_live_channel_gain` / `LiveGainRamp` island helpers are tested by
//! the C++ `speaker_layouts_test`.
mod common;
use common::render::*;
use mradm_engine::request::Selection;
use mradm_testkit::adm::*;

const LFE: usize = 3;

fn ear(dir: &Dir, input: &std::path::Path, layout: &str) -> mradm_engine::render::Request {
    let mut r = request(input, &dir.file("out.wav"), Selection::Ear, layout);
    r.peak_limit = false;
    r
}

fn jump() -> Param {
    jump_position([Param::text(true)])
}

fn one(single: Single, layout: &str) -> Audio {
    let dir = scratch("ear");
    let input = single_input(&dir, single, 48000, 1000);
    render(ear(&dir, &input, layout)).1
}

#[test]
fn front_objects_beds_and_mixed_scenes_render_in_stereo() {
    let audio = one(
        Single::objects(vec![polar(0., 0., [gain_linear(1.0)])]),
        "0+2+0",
    );
    assert_eq!((audio.channels, audio.rate, audio.frames), (2, 48000, 1000));
    let s = audio.sums();
    assert!(s[0] > 0. && (0.95..1.05).contains(&(s[1] / s[0])), "{s:?}");

    let dir = scratch("ear-bed");
    let (axml, uids) = tracks(
        vec![
            (Type::DirectSpeakers, vec![speaker_block("M+030", 30., 0.)]),
            (Type::DirectSpeakers, vec![speaker_block("M-030", -30., 0.)]),
        ],
        true,
    );
    let input = multi_input(&dir, &axml, &uids, &[0.5, 0.25], 48000, 1000);
    let mut r = ear(&dir, &input, "0+2+0");
    r.peak_limit = true;
    let (_, audio) = render(r);
    assert_eq!((audio.channels, audio.frames), (2, 1000));
    let s = audio.sums();
    assert!(s[0] > 0. && s[1] > 0.);

    let dir = scratch("ear-mixed");
    let (axml, uids) = tracks(
        vec![
            (Type::Objects, vec![polar(0., 0., [])]),
            (Type::DirectSpeakers, vec![speaker_block("M+030", 30., 0.)]),
        ],
        false,
    );
    let input = multi_input(&dir, &axml, &uids, &[0.5, 0.4], 48000, 1000);
    let (_, audio) = render(ear(&dir, &input, "0+2+0"));
    let s = audio.sums();
    assert!(audio.frames == 1000 && s[0] > 0. && s[1] > 0.);
}

#[test]
fn custom_layouts_and_aliases_route() {
    let s = one(
        Single::objects(vec![polar(0., 0., [gain_linear(1.0)])]),
        "9.1.6",
    )
    .sums();
    assert_eq!(s.len(), 16);
    assert!(s[2] > 0. && s[LFE] < 1e-9, "{s:?}");
    let s = one(Single::speaker("U+110", Some((110., 45.))), "9.1.6").sums();
    assert!(s[12] > 0. && s[LFE] < 1e-9, "U+110 is Ltm: {s:?}");
    let s = one(Single::speaker("RC_LFE", Some((45., -35.264_39))), "0+5+0").sums();
    assert!(s[LFE] > 0.);
    assert!(
        s.iter().enumerate().all(|(c, v)| c == LFE || *v < 1e-9),
        "{s:?}"
    );
}

#[test]
fn cartesian_positions_and_offsets_apply() {
    let front = objects_block([
        cartesian_position([x(0.0f32), y(1.0f32), z(0.0f32)]),
        cartesian(true),
        gain_linear(1.0),
        jump(),
    ]);
    let s = one(Single::objects(vec![front]), "0+2+0").sums();
    assert!(s[0] > 0. && (0.9..1.1).contains(&(s[1] / s[0])), "{s:?}");

    let mut offset = Single::objects(vec![polar(30., 0., [gain_linear(1.0), jump()])]);
    offset
        .doc
        .set(offset.object, spherical_offset([azimuth(-30.0f32)]));
    let s = one(offset, "0+2+0").sums();
    assert!(s[0] > 0. && (0.9..1.1).contains(&(s[1] / s[0])), "{s:?}");

    // A Cartesian speaker at about +30 degrees drives the left channel.
    let speaker = direct_speakers_block([cartesian_position([x(-0.5f32), y(0.866f32), z(0.0f32)])]);
    let dir = scratch("ear-ds-cartesian");
    let input = single_input(
        &dir,
        Single::new(Type::DirectSpeakers, vec![speaker]),
        48000,
        1000,
    );
    let mut r = ear(&dir, &input, "0+2+0");
    r.peak_limit = true;
    let audio = render(r).1;
    let power = |c: usize| -> f64 {
        (0..audio.frames)
            .map(|f| f64::from(audio.samples[f * 2 + c]).powi(2))
            .sum()
    };
    assert!(power(0) > 100. && power(0) > power(1) * 5.);
}

#[test]
fn the_diffuse_bus_and_the_compensation_delay() {
    for value in [0.0f32, 0.5, 1.0] {
        let single = Single::objects(vec![polar(
            0.,
            0.,
            [gain_linear(1.0), diffuse(value), jump()],
        )]);
        let audio = one(single, "0+2+0");
        assert!(audio.energy() > 0., "diffuse {value}");
        if value == 0. {
            assert_eq!(audio.energy_in(0..255), 0.);
            assert!(audio.energy_in(255..1000) > 0.);
        }
    }
}

#[test]
fn a_short_input_keeps_its_length() {
    let dir = scratch("ear-short");
    let input = single_input(
        &dir,
        Single::objects(vec![polar(0., 0., [gain_linear(1.0)])]),
        48000,
        200,
    );
    assert_eq!(render(ear(&dir, &input, "0+2+0")).1.frames, 200);
}

#[test]
fn p2_semantics_warn_only_for_screen_references() {
    let block = polar(
        0.,
        0.,
        [
            jump(),
            channel_lock([Param::text(true)]),
            object_divergence([Param::text(0.5f32)]),
            screen_ref(true),
        ],
    );
    let dir = scratch("ear-p2");
    let input = single_input(&dir, Single::objects(vec![block]), 48000, 1000);
    let (run, audio) = render(ear(&dir, &input, "0+2+0"));
    let warned = |needle: &str| {
        run.logs
            .iter()
            .any(|(l, _, t)| *l == mradm_engine::request::Level::Warning && t.contains(needle))
    };
    assert!(!warned("channelLock") && !warned("objectDivergence"));
    assert!(warned("screenRef"));
    assert!(audio.energy() > 0.);
}

fn semantics(az: f32, lock: bool, divergence: Option<(f32, f32)>) -> Vec<f64> {
    let mut block = polar(az, 0., [jump()]);
    if lock {
        block.set(channel_lock([Param::text(true)]));
    }
    if let Some((value, range)) = divergence {
        block.set(object_divergence([
            Param::text(value),
            azimuth_range(range),
        ]));
    }
    one(Single::objects(vec![block]), "0+5+0").sums()
}

#[test]
fn channel_lock_and_divergence_apply() {
    let s = semantics(20., true, None);
    assert!(s[0] > 100. && s[0] > s[2] * 10. && s[LFE] < 1e-9, "{s:?}");
    let s = semantics(0., false, Some((1., 30.)));
    assert!(
        s[0] > 1. && s[1] > 1. && s[0] > s[2] && s[1] > s[2],
        "{s:?}"
    );
}

#[test]
fn two_jump_blocks_render_in_one_window() {
    let dir = scratch("ear-blocks");
    let block = |az, start| polar(az, 0., [rtime(start * MS), duration(500 * MS), jump()]);
    let input = single_input(
        &dir,
        Single::objects(vec![block(-30., 0), block(30., 500)]),
        1000,
        1000,
    );
    let audio = render(ear(&dir, &input, "0+2+0")).1;
    assert_eq!(audio.frames, 1000);
    let (early, late) = (audio.sums_in(320..470), audio.sums_in(820..970));
    assert!(early[1] > 1. && early[1] > early[0] * 5., "{early:?}");
    assert!(late[0] > 1. && late[0] > late[1] * 5., "{late:?}");
}

#[test]
fn a_missing_input_is_an_io_error_with_a_message() {
    let dir = scratch("ear-missing");
    let r = request(
        &dir.file("missing.wav"),
        &dir.file("out.wav"),
        Selection::Ear,
        "0+5+0",
    );
    let run = run(r);
    assert_eq!(run.code(), 3);
    assert!(!run.outcome.error.unwrap().message.is_empty());
}
