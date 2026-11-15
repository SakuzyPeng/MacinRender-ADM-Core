//! Whole VBAP renders through the render service (formerly the C++ `vbap_smoke_test`, R3 C1):
//! object positions, block timing and interpolation, object start / duration / gain / mute /
//! offset, DirectSpeakers label, alias, position and fallback routing per layout, channelLock,
//! divergence, MDAP spread, Apple geometry, the 2D-layout warning and the Triple Balance block-gain
//! and semantic-report rules. Backend-internal rules (same-start blocks, clipped blocks, live
//! controls, file streaming against the offline driver, the matrix and split-power LFE routing) are
//! `mradm-render`'s `vbap.rs`; the routing tables are its `routing-v1` goldens; the capability
//! table is `catalog.rs`.
mod common;
use common::render::*;
use mradm_engine::request::{DsRouting, Selection};
use mradm_testkit::adm::*;

const LFE1: usize = 3;
const LFE2: usize = 9;

fn vbap(dir: &Dir, input: &std::path::Path, layout: &str) -> mradm_engine::render::Request {
    let mut r = request(input, &dir.file("out.wav"), Selection::Saf, layout);
    r.peak_limit = false;
    r
}

fn front(cartesian_front: bool, rest: Vec<Param>) -> Single {
    let mut block = if cartesian_front {
        objects_block([
            cartesian_position([x(0.0f32), y(1.0f32), z(0.0f32)]),
            cartesian(true),
        ])
    } else {
        polar(0., 0., [])
    };
    for p in rest {
        block.set(p);
    }
    Single::objects(vec![block])
}

fn jump() -> Param {
    jump_position([Param::text(true)])
}

#[test]
fn a_front_object_is_centred_in_stereo() {
    for cartesian_front in [false, true] {
        let dir = scratch("vbap-front");
        let input = single_input(
            &dir,
            front(cartesian_front, vec![gain_linear(1.0)]),
            48000,
            1000,
        );
        let mut r = request(&input, &dir.file("out.wav"), Selection::Saf, "0+2+0");
        r.peak_limit = true;
        let (_, audio) = render(r);
        assert_eq!((audio.channels, audio.rate, audio.frames), (2, 48000, 1000));
        let s = audio.sums();
        assert!(s[0] > 0. && s[1] > 0.);
        assert!((0.95..1.05).contains(&(s[1] / s[0])), "{s:?}");
    }
}

#[test]
fn timed_blocks_switch_speakers_at_the_block_boundary() {
    let dir = scratch("vbap-timed");
    let block = |az, start| {
        polar(
            az,
            0.,
            [
                gain_linear(1.0),
                rtime(start * MS),
                duration(500 * MS),
                jump(),
            ],
        )
    };
    let input = single_input(
        &dir,
        Single::objects(vec![block(-30., 0), block(30., 500)]),
        1000,
        1000,
    );
    let (_, audio) = render(vbap(&dir, &input, "0+2+0"));
    assert_eq!((audio.channels, audio.rate, audio.frames), (2, 1000, 1000));
    let (first, second) = (audio.sums_in(0..500), audio.sums_in(500..1000));
    // az = -30 is M-030 (ch1), az = +30 is M+030 (ch0).
    assert!(first[1] > 0. && first[0] < 1e-3, "{first:?}");
    assert!(second[0] > 0. && second[1] < 1e-3, "{second:?}");
}

#[test]
fn an_interpolation_longer_than_the_block_is_clamped_to_it() {
    let dir = scratch("vbap-clamp");
    let blocks = vec![
        polar(
            -30.,
            0.,
            [gain_linear(1.0), rtime(0), duration(500 * MS), jump()],
        ),
        polar(
            30.,
            0.,
            [
                gain_linear(1.0),
                rtime(500 * MS),
                duration(100 * MS),
                jump_position([Param::text(false), interpolation_length(1.0)]),
            ],
        ),
        polar(
            -30.,
            0.,
            [
                gain_linear(1.0),
                rtime(600 * MS),
                duration(400 * MS),
                jump(),
            ],
        ),
    ];
    let input = single_input(&dir, Single::objects(blocks), 1000, 1000);
    let (_, audio) = render(vbap(&dir, &input, "0+2+0"));
    let middle = audio.sums_in(500..600);
    assert!(middle[0] > middle[1] * 0.8, "{middle:?}");
}

#[test]
fn nested_object_starts_accumulate() {
    let dir = scratch("vbap-nested");
    let mut single = Single::bare(
        Type::Objects,
        vec![polar(
            0.,
            0.,
            [gain_linear(1.0), rtime(0), duration(500 * MS), jump()],
        )],
    );
    let child = single.object;
    single.doc.set(child, start(250 * MS));
    let parent = single.doc.object([object_name("Parent"), start(250 * MS)]);
    single.doc.reference(parent, child);
    single.doc.add(parent);
    single.wrap(parent);
    let input = single_input(&dir, single, 1000, 1000);
    let (_, audio) = render(vbap(&dir, &input, "0+2+0"));
    assert!(audio.energy_in(0..500) < 1e-6);
    assert!(audio.energy_in(500..1000) > 0.);
}

fn speaker_render(
    label: &str,
    position: Option<(f32, f32)>,
    layout: &str,
    edit: impl FnOnce(&mut mradm_engine::render::Request),
) -> (Run, Vec<f64>) {
    let dir = scratch("vbap-ds");
    let input = single_input(&dir, Single::speaker(label, position), 48000, 1000);
    let mut r = request(&input, &dir.file("out.wav"), Selection::Saf, layout);
    edit(&mut r);
    let (run, audio) = render(r);
    (run, audio.sums())
}

fn active(sums: &[f64], skip: &[usize]) -> usize {
    sums.iter()
        .enumerate()
        .filter(|(c, s)| !skip.contains(c) && **s > 1e-5)
        .count()
}

#[test]
fn direct_speakers_labels_route_one_hot() {
    let (_, s) = speaker_render("M+030", Some((30., 0.)), "0+2+0", |_| {});
    assert!(s[0] > 0. && s[1] < 1e-6, "{s:?}");

    // WAVE_7_1 order is L R C LFE Lrs Rrs Ls Rs.
    let (_, rear) = speaker_render("M+135", Some((135., 0.)), "wav71", |_| {});
    let (_, side) = speaker_render("M+090", Some((90., 0.)), "wav71", |_| {});
    assert!(rear[4] > 0. && rear[6] < 1e-6, "{rear:?}");
    assert!(side[6] > 0. && side[4] < 1e-6, "{side:?}");

    let (_, lfe) = speaker_render("RC_LFE", Some((0., 0.)), "0+5+0", |_| {});
    assert!(lfe[LFE1] > 0.);
    assert!(
        lfe.iter().enumerate().all(|(c, s)| c == LFE1 || *s < 1e-6),
        "{lfe:?}"
    );

    for (layout, label, az, el, channels, target) in [
        ("2+5+0", "U+030", 30., 30., 8, 6),
        ("4+5+4", "U+150", 150., 45., 14, 12),
        ("9.1.6", "M+000", 0., 0., 16, 2),
    ] {
        let (_, s) = speaker_render(label, Some((az, el)), layout, |_| {});
        assert_eq!(s.len(), channels);
        assert!(s[target] > 0. && s[LFE1] < 1e-9, "{layout}: {s:?}");
    }

    // 9.1.6 top side left (Ltm) is ch12, by label and by an exact position fallback.
    let (_, s) = speaker_render("U+110", Some((110., 45.)), "9.1.6", |_| {});
    assert!(s[12] > 0. && s[LFE1] < 1e-9);
    assert!(
        s.iter()
            .enumerate()
            .all(|(c, v)| c == 12 || c == LFE1 || *v < 1e-6),
        "{s:?}"
    );
    let (run, s) = speaker_render("NOT_A_BS2051_LABEL", Some((110., 45.)), "9.1.6", |_| {});
    assert!(s[12] > 0.);
    assert!(run.warnings("saf-vbap") > 0);
}

#[test]
fn direct_speakers_routing_modes_on_22_2() {
    let apple = |mode: DsRouting| {
        move |r: &mut mradm_engine::render::Request| {
            r.apple_geometry = true;
            r.routing = mode as u32;
            r.peak_limit = false;
        }
    };
    // U+045 names ch12 while its nominal position lies between speakers.
    let (_, automatic) = speaker_render(
        "U+045",
        Some((45., 15.)),
        "9+10+3",
        apple(DsRouting::Automatic),
    );
    let (_, label) = speaker_render("U+045", Some((45., 15.)), "9+10+3", apple(DsRouting::Label));
    let (_, position) = speaker_render(
        "U+045",
        Some((45., 15.)),
        "9+10+3",
        apple(DsRouting::Position),
    );
    assert_eq!(label.len(), 24);
    assert!(automatic[12] > 0. && label[12] > 0.);
    assert!(
        label.iter().enumerate().all(|(c, s)| c == 12 || *s < 1e-6),
        "{label:?}"
    );
    assert!(active(&position, &[LFE1, LFE2]) >= 2, "{position:?}");
    assert!(position[LFE1] < 1e-9 && position[LFE2] < 1e-9);

    let (run, unknown) = speaker_render(
        "UNKNOWN",
        Some((45., 15.)),
        "9+10+3",
        apple(DsRouting::Label),
    );
    assert!(active(&unknown, &[LFE1, LFE2]) >= 2, "{unknown:?}");
    assert!(run.warned("saf-vbap", "spatializing from nominal position"));

    let (_, alias) = speaker_render("L", Some((0., 0.)), "9+10+3", apple(DsRouting::Label));
    assert!(alias[6] > 0., "L is M+030, ch6 of 22.2");

    let (_, lfe) = speaker_render("LFE", Some((0., 0.)), "9+10+3", apple(DsRouting::Position));
    assert!(lfe[LFE1] > 0. && lfe[LFE2] < 1e-9, "{lfe:?}");

    // An unmatched label at -179 wraps to the rear and spreads with continuous VBAP.
    let (run, s) = speaker_render("NOT_A_BS2051_LABEL", Some((-179., 0.)), "9+10+3", |_| {});
    let rear = 8;
    let others: Vec<f64> = s
        .iter()
        .enumerate()
        .filter(|(c, _)| ![rear, LFE1, LFE2].contains(c))
        .map(|(_, v)| *v)
        .collect();
    assert!(s[rear] > 0.);
    assert!(others.iter().sum::<f64>() > 1e-6);
    assert!(s[rear] > others.iter().copied().fold(0., f64::max));
    assert!(s[LFE1] < 1e-9 && s[LFE2] < 1e-9);
    assert!(run.warnings("saf-vbap") > 0);
}

#[test]
fn explicit_direct_speakers_routing_is_rejected_by_other_backends() {
    let dir = scratch("vbap-reject");
    let input = single_input(&dir, Single::speaker("M+000", Some((0., 0.))), 48000, 1000);
    for (renderer, layout) in [
        (Selection::Ear, "0+5+0"),
        (Selection::SafBinaural, "binaural"),
        (Selection::Hoa, "hoa3"),
    ] {
        for mode in [DsRouting::Label, DsRouting::Position] {
            let mut r = request(&input, &dir.file("out.wav"), renderer, layout);
            r.routing = mode as u32;
            assert_eq!(run(r).code(), 2, "{renderer:?} {mode:?}");
        }
    }
}

#[test]
fn mdap_spread_reaches_the_elevated_speakers() {
    let elevated = |width_value: f32| {
        let dir = scratch("vbap-mdap");
        let input = single_input(
            &dir,
            front(false, vec![gain_linear(1.0), width(width_value)]),
            48000,
            1000,
        );
        let r = request(&input, &dir.file("out.wav"), Selection::Saf, "4+5+0");
        let (_, audio) = render(r);
        audio.sums()[6..].iter().sum::<f64>()
    };
    assert!(elevated(0.8) > elevated(0.));
}

fn simple(edit: impl FnOnce(&mut Single), rate: u32) -> Audio {
    let dir = scratch("vbap-simple");
    let mut single = Single::objects(vec![polar(0., 0., [gain_linear(1.0), jump()])]);
    edit(&mut single);
    let input = single_input(&dir, single, rate, 1000);
    render(vbap(&dir, &input, "0+2+0")).1
}

#[test]
fn object_gain_mute_and_duration_apply() {
    let full = simple(|_| {}, 48000).energy();
    let half = simple(|s| s.doc.set(s.object, gain_linear(0.5)), 48000).energy();
    assert!(
        full > 0. && (0.48..0.52).contains(&(half / full)),
        "{full} {half}"
    );
    assert_eq!(
        simple(|s| s.doc.set(s.object, mute(true)), 48000).energy(),
        0.
    );
    let gated = simple(|s| s.doc.set(s.object, duration(250 * MS)), 1000);
    assert!(gated.energy_in(0..250) > 0.);
    assert_eq!(gated.energy_in(250..1000), 0.);
}

#[test]
fn direct_speakers_block_time_windows_gate_the_signal() {
    let dir = scratch("vbap-ds-window");
    let mut block = direct_speakers_block([
        spherical([azimuth(30.0f32), elevation(0.0f32), distance(1.0f32)]),
        rtime(250 * MS),
        duration(250 * MS),
    ]);
    block.add(speaker_label("M+030"));
    let input = single_input(
        &dir,
        Single::new(Type::DirectSpeakers, vec![block]),
        1000,
        1000,
    );
    let (_, audio) = render(vbap(&dir, &input, "0+2+0"));
    assert_eq!(audio.energy_in(0..250), 0.);
    assert!(audio.energy_in(250..500) > 0.);
    assert_eq!(audio.energy_in(500..1000), 0.);
}

#[test]
fn the_object_position_offset_applies() {
    let dir = scratch("vbap-offset");
    let mut single = Single::objects(vec![polar(30., 0., [gain_linear(1.0), jump()])]);
    single
        .doc
        .set(single.object, spherical_offset([azimuth(-30.0f32)]));
    let input = single_input(&dir, single, 48000, 1000);
    let s = render(vbap(&dir, &input, "0+2+0")).1.sums();
    assert!(
        s[0] > 0. && s[1] > 0. && (0.9..1.1).contains(&(s[1] / s[0])),
        "{s:?}"
    );
}

fn semantics(az: f32, lock: Option<Option<f32>>, divergence: Option<(f32, f32)>) -> Vec<f64> {
    let dir = scratch("vbap-semantics");
    let mut block = polar(az, 0., [jump()]);
    if let Some(max) = lock {
        let mut values = vec![Param::text(true)];
        values.extend(max.map(max_distance));
        block.set(channel_lock(values));
    }
    if let Some((value, range)) = divergence {
        block.set(object_divergence([
            Param::text(value),
            azimuth_range(range),
        ]));
    }
    let input = single_input(&dir, Single::objects(vec![block]), 48000, 1000);
    render(vbap(&dir, &input, "0+5+0")).1.sums()
}

#[test]
fn channel_lock_and_divergence_apply() {
    let locked = semantics(20., Some(None), None);
    assert!(
        locked[0] > 100. && locked[1] < 1e-6 && locked[2] < 1e-6 && locked[LFE1] < 1e-9,
        "{locked:?}"
    );
    let rejected = semantics(20., Some(Some(0.01)), None);
    assert!(
        rejected[2] > 1.,
        "a lock beyond maxDistance falls back to VBAP: {rejected:?}"
    );
    let diverged = semantics(0., None, Some((1., 30.)));
    assert!(
        diverged[0] > 1. && diverged[1] > 1. && diverged[2] < 1e-6,
        "{diverged:?}"
    );
    assert!((0.95..1.05).contains(&(diverged[1] / diverged[0])));
}

#[test]
fn the_default_interpolation_controls_the_block_ramp() {
    let right_after = |interp_ms: u32| {
        let dir = scratch("vbap-interp");
        let blocks = vec![
            polar(-30., 0., [rtime(0), duration(500 * MS)]),
            polar(30., 0., [rtime(500 * MS), duration(500 * MS)]),
        ];
        let input = single_input(&dir, Single::objects(blocks), 1000, 1000);
        let mut r = vbap(&dir, &input, "0+2+0");
        r.default_interp_ms = interp_ms;
        render(r).1.sums_in(510..540)[1]
    };
    assert!(right_after(0) < 1e-6);
    assert!(right_after(200) > 1e-3);
}

#[test]
fn apple_geometry_changes_the_gains() {
    let sums = |apple: bool| {
        let dir = scratch("vbap-geometry");
        let input = single_input(
            &dir,
            Single::objects(vec![polar(90., 45., [jump()])]),
            48000,
            1000,
        );
        let mut r = vbap(&dir, &input, "2+5+0");
        r.apple_geometry = apple;
        render(r).1.sums()
    };
    let (standard, apple) = (sums(false), sums(true));
    let total: f64 = apple.iter().sum();
    let difference: f64 = apple
        .iter()
        .zip(&standard)
        .map(|(a, s)| (a - s).abs())
        .sum();
    // CoreAudio's 5.1.2 top-middle-left is +90 / +45.
    assert!(total > 0. && apple[6] > total * 0.99, "{apple:?}");
    assert!(difference > total * 0.1);
}

#[test]
fn elevated_sources_on_a_2d_layout_warn() {
    let warned = |layout: &str, el: f32| {
        let dir = scratch("vbap-2d");
        let input = single_input(
            &dir,
            Single::objects(vec![polar(0., el, [jump()])]),
            48000,
            1000,
        );
        let (run, _) = render(vbap(&dir, &input, layout));
        run
    };
    let run = warned("0+2+0", 30.);
    assert!(run.warned("saf-vbap", "2D"));
    assert!(run.warned("saf-vbap", "elevation") || run.warned("saf-vbap", "horizontal"));
    assert!(!warned("4+5+0", 30.).warned("saf-vbap", "2D"));
    assert!(!warned("0+2+0", 0.).warned("saf-vbap", "2D"));
}

#[test]
fn triple_balance_ignores_the_block_gain_vbap_applies_it() {
    let total = |renderer: Selection, gain: f32| {
        let dir = scratch("vbap-tb-gain");
        let input = single_input(
            &dir,
            front(true, vec![gain_linear(f64::from(gain))]),
            48000,
            1000,
        );
        let mut r = request(&input, &dir.file("out.wav"), renderer, "4+7+0");
        r.peak_limit = false;
        render(r).1.energy()
    };
    let (tb_one, tb_half) = (
        total(Selection::TripleBalance, 1.),
        total(Selection::TripleBalance, 0.5),
    );
    let (vbap_one, vbap_half) = (total(Selection::Saf, 1.), total(Selection::Saf, 0.5));
    assert!(tb_one > 0. && vbap_one > 0.);
    assert!((tb_half / tb_one - 1.).abs() < 1e-5);
    assert!((vbap_half / vbap_one - 0.5).abs() < 1e-5);
}

#[test]
fn triple_balance_semantic_reports_match_in_file_and_memory() {
    let dir = scratch("vbap-tb-report");
    let single = front(
        true,
        vec![
            gain_linear(0.25),
            width(0.25f32),
            height(0.25f32),
            depth(0.25f32),
        ],
    );
    let input = single_input(&dir, single, 48000, 10037);
    let report = dir.file("report.json");
    let mut r = request(
        &input,
        &dir.file("out.wav"),
        Selection::TripleBalance,
        "9.1.6",
    );
    r.peak_limit = false;
    r.capture_semantic_report = true;
    r.semantic_report_path = Some(text(&report));
    r.semantic_policy_json = Some(
        br#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":0.5,"mute":true}}}"#
            .to_vec(),
    );
    let same = |run: &Run| {
        let file = std::fs::read_to_string(&report).unwrap();
        run.outcome
            .semantic_report
            .as_ref()
            .map(|m| format!("{m}\n"))
            == Some(file)
    };
    let first = run(r.clone());
    assert!(first.ok() && same(&first));
    let text = first.outcome.semantic_report.clone().unwrap();
    for needle in [
        "\"renderer_effective\"",
        "\"output_gain\": 0.0",
        "\"source_rtime_sample\"",
        "\"gain\": 0.25",
    ] {
        assert!(text.contains(needle), "{needle}");
    }
    r.speaker_spread = mradm_engine::backend::SpeakerSpread::Mdap as u32;
    let rejected = run(r);
    assert_eq!(rejected.code(), 2);
    assert!(same(&rejected));
    assert!(
        rejected
            .outcome
            .semantic_report
            .unwrap()
            .contains(r#""status": "unsupported""#)
    );
}

#[test]
fn a_missing_input_fails_at_import() {
    let dir = scratch("vbap-missing");
    let r = request(
        &dir.file("missing.wav"),
        &dir.file("out.wav"),
        Selection::Saf,
        "0+5+0",
    );
    assert_eq!(run(r).code(), 3, "io_error, not unsupported");
}

#[test]
fn a_rear_label_without_coordinates_stays_behind() {
    let (run, s) = speaker_render("M+135", None, "0+5+0", |r| {
        r.apple_geometry = true;
        r.routing = DsRouting::Label as u32;
    });
    assert!(s[4] > s[0] + s[1] + s[2], "{s:?}");
    assert!(run.warned("saf-vbap", "spatializing from label direction"));
    assert!(!run.warned("saf-vbap", "no nominal position"));
}
