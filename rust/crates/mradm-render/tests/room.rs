//! The room file renderer's sources and event table (PCM consistency 4.6 D): the scene rules
//! moved from the C++ `RoomRenderer::prepare` and `room_render_test` (same-start blocks, screen
//! references), with the checks, ramps, DirectSpeakers positions and event order.
mod common;

use common::{ObjectSpec, TrackSpec, direct_block, object_block};
use mradm_adm::records::{AdmScreen, Content, Programme, Span};
use mradm_dsp::room::control::State;
use mradm_dsp::room::loading;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_render::room::{self, Prepared, Request, Routing, Screens, Spread};
use mradm_render::routing::Level;
use mradm_scene::RenderScene;

const FRAMES: u64 = 9600;

fn request(scene: &RenderScene) -> Request<'_> {
    Request {
        scene,
        sample_rate: 48000,
        layout_id: b"binaural",
        spread: Spread::Automatic,
        routing: Routing::Automatic,
        has_matrix: false,
        default_interp_ms: 5,
        screens: Screens::default(),
    }
}

type Logs = Vec<(Level, String)>;

fn prepare(r: &Request) -> (Result<Prepared, (i32, String)>, Logs) {
    let mut logs = Vec::new();
    let result = room::prepare(r, &mut |level, text| {
        logs.push((level, String::from_utf8(text).unwrap()));
    })
    .map_err(|e| (e.code, String::from_utf8(e.message).unwrap()));
    (result, logs)
}

fn ok(r: &Request) -> (Prepared, Logs) {
    let (result, logs) = prepare(r);
    (result.unwrap(), logs)
}

/// The comparable content of a preparation.
type Row = (u64, u32, u32, u32, u64, State);
type Source = (u64, u32, [f32; 3]);
fn table(p: &Prepared) -> (Vec<Row>, Vec<Source>, Vec<Vec<u8>>) {
    (
        p.events
            .iter()
            .map(|e| {
                let c = &e.command;
                (
                    e.sample, e.priority, c.element, c.duration, c.changed, c.state,
                )
            })
            .collect(),
        p.descriptions
            .iter()
            .map(|d| (d.id, d.role, d.position))
            .collect(),
        p.labels.clone(),
    )
}

fn track(blocks: Vec<mradm_adm::records::ObjectBlock>) -> TrackSpec {
    TrackSpec {
        blocks,
        direct: Vec::new(),
    }
}

fn cartesian(start: u64, end: u64, [x, y, z]: [f32; 3]) -> mradm_adm::records::ObjectBlock {
    let mut b = object_block(start, end, 0.);
    b.position.cartesian = 1;
    (b.position.x, b.position.y, b.position.z) = (x, y, z);
    b
}

fn scene_of(tracks: Vec<TrackSpec>) -> RenderScene {
    common::scene(
        vec![ObjectSpec {
            id: "AO_1001",
            gain: 1.,
            end: u64::MAX,
            tracks,
        }],
        FRAMES,
    )
}

fn string(scene: &mut RenderScene, value: &str) -> u32 {
    let s = &mut scene.snapshot;
    s.strings.push(value.to_owned());
    (s.strings.len() - 1) as u32
}

fn ids(scene: &mut RenderScene, values: &[&str]) -> Span {
    let ids: Vec<u32> = values.iter().map(|v| string(scene, v)).collect();
    let s = &mut scene.snapshot;
    let offset = s.indices.len();
    s.indices.extend(ids);
    Span {
        offset,
        len: values.len(),
    }
}

/// A programme of one content holding `objects`, with a production screen of `width` if any.
fn programme(scene: &mut RenderScene, name: &str, objects: &[&str], width: Option<f64>) {
    let content_id = format!("ACO_{name}");
    let content = Content {
        id: string(scene, &content_id),
        object_ids: ids(scene, objects),
        ..Content::default()
    };
    scene.snapshot.contents.push(content);
    let p = Programme {
        id: string(scene, &format!("APR_{name}")),
        content_ids: ids(scene, &[&content_id]),
        reference_screen: u32::from(width.is_some()),
        screen_geometry: AdmScreen {
            width: width.unwrap_or(58.),
            ..AdmScreen::default()
        },
        ..Programme::default()
    };
    scene.snapshot.programmes.push(p);
}

fn reproduction() -> Screens {
    Screens {
        reference: None,
        reproduction: Some(RoomScreenGeometry {
            width: 90.,
            ..RoomScreenGeometry::default()
        }),
    }
}

#[test]
fn reference_screens_follow_programmes_nested_objects_and_the_configuration() {
    let mut scene = scene_of(vec![track(vec![cartesian(0, FRAMES, [0., 1., 0.])])]);
    // AO_1001 is a child of AO_2000, which the programmes reference.
    let parent_id = string(&mut scene, "AO_2000");
    let children = ids(&mut scene, &["AO_1001"]);
    scene.snapshot.objects.push(mradm_adm::records::Object {
        id: parent_id,
        child_objects: children,
        ..Default::default()
    });
    programme(&mut scene, "1", &["AO_2000"], Some(60.));
    let screens = reproduction();
    let width = |scene: &RenderScene, screens: &Screens| {
        room::reference_screen(scene, b"AO_1001", screens).map(|g| g.width)
    };
    assert_eq!(
        width(&scene, &screens),
        Ok(60.),
        "inherited through the parent"
    );
    programme(&mut scene, "2", &["AO_2000"], Some(70.));
    let conflict = room::reference_screen(&scene, b"AO_1001", &screens).unwrap_err();
    assert_eq!(
        (conflict.code, String::from_utf8(conflict.message).unwrap()),
        (
            1,
            "对象属于制作屏幕不同的 Programme；请在屏幕配置中显式覆盖 reference".to_owned()
        )
    );
    let explicit = Screens {
        reference: Some(RoomScreenGeometry::default()),
        ..screens
    };
    assert_eq!(
        width(&scene, &explicit),
        Ok(58.),
        "an explicit reference wins"
    );
    assert_eq!(
        width(&scene, &Screens::default()),
        Ok(58.),
        "no reproduction screen keeps the default"
    );
    assert!(width(&scene, &screens).is_err());
    // A programme without a production screen counts as the default one.
    let mut other = scene_of(vec![track(vec![cartesian(0, FRAMES, [0., 1., 0.])])]);
    programme(&mut other, "1", &["AO_1001"], None);
    programme(&mut other, "2", &["AO_1001"], Some(58.));
    assert_eq!(width(&other, &screens), Ok(58.));
    // The mapping: enabled with a reproduction screen, which falls back to the reference.
    let reference = RoomScreenGeometry {
        width: 40.,
        ..RoomScreenGeometry::default()
    };
    let mapped = room::room_screens(&screens, &reference);
    assert_eq!(
        (
            mapped.enabled,
            mapped.reference.width,
            mapped.reproduction.width
        ),
        (1, 40., 90.)
    );
    let off = room::room_screens(&Screens::default(), &reference);
    assert_eq!((off.enabled, off.reproduction.width), (0, 40.));
}

#[test]
fn a_screen_reference_conflict_only_counts_blocks_in_effect() {
    let mut winners = vec![cartesian(0, 2400, [0.6, 1., 0.])];
    let mut peer = cartesian(0, 4800, [-1., 1., 0.]);
    peer.screen_ref = 1;
    peer.width = 0.4;
    winners.push(cartesian(4800, 7200, [-0.4, 1., 0.]));
    let overwritten = vec![
        peer,
        winners[0],
        cartesian(4800, 9600, [0.2, 1., 0.]),
        winners[1],
    ];
    let mut a = scene_of(vec![track(winners)]);
    let mut b = scene_of(vec![track(overwritten)]);
    for scene in [&mut a, &mut b] {
        programme(scene, "1", &["AO_1001"], Some(60.));
        programme(scene, "2", &["AO_1001"], Some(70.));
    }
    let mut ra = request(&a);
    ra.screens = reproduction();
    let mut rb = request(&b);
    rb.screens = reproduction();
    assert_eq!(table(&ok(&ra).0), table(&ok(&rb).0));
    // The same conflict in effect fails.
    let mut c = scene_of(vec![track(vec![peer])]);
    programme(&mut c, "1", &["AO_1001"], Some(60.));
    programme(&mut c, "2", &["AO_1001"], Some(70.));
    let mut rc = request(&c);
    rc.screens = reproduction();
    assert_eq!(prepare(&rc).0.err().map(|e| e.0), Some(1));
}

/// Six blocks per start, the winner written last; winners end halfway through their peers.
fn same_start(kind: u8, winning_label: &'static str) -> (TrackSpec, TrackSpec) {
    let mut winners = TrackSpec::default();
    let mut overwritten = TrackSpec::default();
    for i in 0..6 {
        for start in [4800u64, 0] {
            let winner = i == 5;
            let end = start + if winner { 2400 } else { 4800 };
            if kind == 0 {
                let mut b = cartesian(
                    start,
                    end,
                    [
                        if winner {
                            if start == 0 { 0.6 } else { -0.4 }
                        } else {
                            -1. + 0.3 * i as f32
                        },
                        1.,
                        0.,
                    ],
                );
                b.gain = if winner { 0.5 } else { 0.9 };
                b.width = if winner { 0. } else { 0.4 };
                b.screen_ref = u32::from(!winner);
                overwritten.blocks.push(b);
                if winner {
                    winners.blocks.push(b);
                }
                continue;
            }
            let mut b = direct_block(start, end);
            b.has_position = 1;
            b.azimuth = if winner {
                if start == 0 { 30. } else { -110. }
            } else {
                -30.
            };
            b.gain = if winner { 0.5 } else { 0.9 };
            let labels = if winner && !winning_label.is_empty() {
                vec![winning_label]
            } else if !winner && kind == 1 {
                b.low_pass_present = 1;
                b.low_pass = 120.;
                vec!["LFE1"]
            } else if !winner {
                vec!["M-030"]
            } else {
                Vec::new()
            };
            overwritten.direct.push((labels.clone(), b));
            if winner {
                winners.direct.push((labels, b));
            }
        }
    }
    winners.blocks.sort_by_key(|b| b.start);
    winners.direct.sort_by_key(|(_, b)| b.start);
    (winners, overwritten)
}

#[test]
fn only_the_last_block_at_each_start_takes_effect() {
    for (kind, label) in [(0, ""), (1, ""), (2, "M+030"), (2, "")] {
        let (winners, overwritten) = same_start(kind, label);
        let a = scene_of(vec![winners]);
        let b = scene_of(vec![overwritten]);
        let mut ra = request(&a);
        ra.screens = reproduction();
        let mut rb = request(&b);
        rb.screens = reproduction();
        let (pa, _) = ok(&ra);
        let (pb, _) = ok(&rb);
        assert_eq!(table(&pa), table(&pb), "kind {kind} label {label:?}");
        let expected: &[u8] = if label.is_empty() { b"" } else { b"M+030" };
        assert_eq!(
            pb.labels,
            vec![expected.to_vec()],
            "the live label is the winner's"
        );
        if kind == 1 {
            assert_eq!(pb.descriptions[0].role, 1, "LFE peers leave no LFE role");
        }
    }
}

fn states(p: &Prepared, priority: u32) -> Vec<(u64, State, u32)> {
    p.events
        .iter()
        .filter(|e| e.priority == priority)
        .map(|e| (e.sample, e.command.state, e.command.duration))
        .collect()
}

#[test]
fn object_events_ramp_truncate_and_order_like_the_cpp_renderer() {
    let mut second = cartesian(3000, u64::MAX, [-0.8, 1.3, 0.]);
    second.interpolation_present = 1;
    second.interpolation = 3317;
    let mut third = cartesian(5000, 6000, [0.5, 0.5, 0.5]);
    third.jump = 1;
    let fourth = {
        let mut b = object_block(7000, u64::MAX, 30.);
        b.position.elevation = 10.;
        b.position.distance = 2.;
        b
    };
    let mut scene = common::scene(
        vec![ObjectSpec {
            id: "AO_1001",
            gain: 0.5,
            end: 9000,
            tracks: vec![track(vec![
                cartesian(0, 2000, [0., 1., 0.]),
                second,
                third,
                fourth,
            ])],
        }],
        FRAMES,
    );
    scene.snapshot.object_blocks[3].gain = 0.25;
    let (p, logs) = ok(&request(&scene));
    assert_eq!(logs, vec![(Level::Info, room::PREPARED_INFO.to_owned())]);
    let ramps: Vec<(u64, u32)> = states(&p, 2).iter().map(|s| (s.0, s.2)).collect();
    assert_eq!(
        ramps,
        vec![(0, 0), (3000, 2000), (5000, 0), (7000, 5 * 48)],
        "first and jump blocks cut; the authored ramp is capped by the block; the default is ms × 48"
    );
    let ends: Vec<u64> = states(&p, 0).iter().skip(1).map(|s| s.0).collect();
    assert_eq!(
        ends,
        vec![2000, 5000, 6000, 9000],
        "next start, block end, object end"
    );
    let polar = loading::polar(30., 10., 2.).unwrap();
    let last = states(&p, 2)[3].1;
    assert_eq!(last.position, polar);
    assert_eq!(last.gain, 0.5 * 0.25);
    // Commands sharing a sample: deactivation, activation, state.
    let at: Vec<u32> = p
        .events
        .iter()
        .filter(|e| e.sample == 5000)
        .map(|e| e.priority)
        .collect();
    assert_eq!(at, vec![0, 1, 2]);
    assert_eq!(
        p.events[0].command.state.active, 0,
        "every source starts inactive"
    );
    scene.overlay.objects[0].mute = true;
    let (muted, _) = ok(&request(&scene));
    assert!(states(&muted, 2).iter().all(|s| s.1.gain == 0.));
    let mut faster = request(&scene);
    faster.default_interp_ms = 20;
    assert_eq!(
        states(&ok(&faster).0, 2)[3].2,
        20 * 48,
        "the 96 kHz known issue stays"
    );
}

fn ds_scene(blocks: Vec<(Vec<&'static str>, mradm_adm::records::DirectBlock)>) -> RenderScene {
    scene_of(vec![TrackSpec {
        blocks: Vec::new(),
        direct: blocks,
    }])
}

#[test]
fn direct_speakers_positions_follow_the_cpp_fallbacks() {
    let front =
        "DirectSpeakers block has no nominal position; using front-centre (azimuth 0, elevation 0)";
    let position = |scene: &RenderScene| states(&ok(&request(scene)).0, 2)[0].1.position;
    // Without a position: the warning, then the label direction at distance 1.
    let labelled = ds_scene(vec![(vec!["M+110"], direct_block(0, FRAMES))]);
    let (p, logs) = ok(&request(&labelled));
    assert_eq!(logs[0], (Level::Warning, front.to_owned()));
    assert_eq!(
        states(&p, 2)[0].1.position,
        loading::polar(110., 0., 1.).unwrap()
    );
    // Neither position nor label: the front.
    let bare = ds_scene(vec![(vec!["X"], direct_block(0, FRAMES))]);
    assert_eq!(position(&bare), loading::polar(0., 0., 1.).unwrap());
    // A nominal position keeps its distance.
    let mut b = direct_block(0, FRAMES);
    b.has_position = 1;
    b.azimuth = -30.;
    b.elevation = 15.;
    b.distance = 2.;
    let mut placed = ds_scene(vec![(vec!["M+110"], b)]);
    assert_eq!(position(&placed), loading::polar(-30., 15., 2.).unwrap());
    // An ADM Cartesian source position wins unless the user moved the speaker.
    placed.snapshot.direct_blocks[0].position.cartesian = 1;
    (
        placed.snapshot.direct_blocks[0].position.x,
        placed.snapshot.direct_blocks[0].position.y,
    ) = (0.25, 0.5);
    assert_eq!(position(&placed), [0.25, 0.5, 0.]);
    placed.overlay.direct_blocks[0].position_override = true;
    assert_eq!(position(&placed), loading::polar(-30., 15., 2.).unwrap());
    // An effective position from the overlay without the record's ADM source.
    placed.overlay.direct_blocks[0].position_override = false;
    placed.snapshot.direct_blocks[0].has_position = 0;
    assert_eq!(position(&placed), loading::polar(-30., 15., 2.).unwrap());
}

#[test]
fn lfe_sources_keep_their_role_and_reject_a_switch() {
    let mut lfe = direct_block(0, 4800);
    lfe.low_pass_present = 1;
    lfe.low_pass = 120.;
    let scene = ds_scene(vec![(vec!["LFE1"], lfe)]);
    let (p, logs) = ok(&request(&scene));
    assert_eq!(p.descriptions[0].role, 2);
    assert_eq!(
        states(&p, 2)[0].1.position,
        [0., 1., 0.],
        "an LFE source is not placed"
    );
    assert_eq!(logs.len(), 1, "no position warning for LFE");
    let switch = ds_scene(vec![
        (vec!["LFE1"], lfe),
        (vec!["M+030"], direct_block(4800, FRAMES)),
    ]);
    assert_eq!(
        prepare(&request(&switch)).0.err(),
        Some((2, "同一房间声源不能在全频与 LFE 角色间切换".to_owned()))
    );
}

#[test]
fn requests_and_tracks_keep_the_cpp_checks() {
    let scene = scene_of(vec![track(vec![cartesian(0, FRAMES, [0., 1., 0.])])]);
    let format = Some((
        2,
        "room-binaural 需要同采样率双耳 Objects/DirectSpeakers".to_owned(),
    ));
    let mode = Some((
        2,
        "room-binaural 使用连续位置渲染，不支持此 spreader/扬声器路由模式".to_owned(),
    ));
    let fails = |edit: &dyn Fn(&mut Request)| {
        let mut r = request(&scene);
        edit(&mut r);
        prepare(&r).0.err()
    };
    assert_eq!(fails(&|r| r.sample_rate = 96000), format);
    assert_eq!(fails(&|r| r.layout_id = b"stereo"), format);
    assert_eq!(fails(&|r| r.spread = Spread::SafSpreader), mode);
    assert_eq!(fails(&|r| r.has_matrix = true), mode);
    assert_eq!(fails(&|r| r.routing = Routing::Label), mode);
    assert_eq!(fails(&|r| r.routing = Routing::Matrix), mode);
    assert_eq!(fails(&|r| r.routing = Routing::Position), None);
    let mut hoa = scene_of(vec![track(vec![cartesian(0, FRAMES, [0., 1., 0.])])]);
    hoa.snapshot.hoa.push(Default::default());
    assert_eq!(prepare(&request(&hoa)).0.err(), format);
    let mut r = request(&scene);
    r.spread = Spread::None;
    assert!(!ok(&r).0.spread);

    let invalid = Some((2, "room-binaural 声源轨道无效或超过 512 路".to_owned()));
    let mut unbound = scene_of(vec![track(vec![cartesian(0, FRAMES, [0., 1., 0.])])]);
    unbound.snapshot.tracks[0].channel = 1;
    assert_eq!(
        prepare(&request(&unbound)).0.err(),
        invalid,
        "beyond the input"
    );
    let mut both = scene_of(vec![TrackSpec {
        blocks: vec![cartesian(0, FRAMES, [0., 1., 0.])],
        direct: vec![(vec!["M+030"], direct_block(0, FRAMES))],
    }]);
    assert_eq!(
        prepare(&request(&both)).0.err(),
        invalid,
        "Objects and DirectSpeakers"
    );
    both.snapshot.tracks[0].ds_blocks.len = 0;
    assert!(prepare(&request(&both)).0.is_ok());
    let many: Vec<TrackSpec> = (0..513)
        .map(|_| track(vec![cartesian(0, FRAMES, [0., 1., 0.])]))
        .collect();
    let mut wide = scene_of(many);
    for t in &mut wide.snapshot.tracks {
        t.channel = 0;
    }
    assert_eq!(prepare(&request(&wide)).0.err(), invalid, "513 sources");
    wide.snapshot.tracks[512].blocks.len = 0;
    let (p, _) = ok(&request(&wide));
    assert_eq!(
        p.descriptions.len(),
        512,
        "a track without blocks is no source"
    );
}

#[test]
fn an_unmapped_screen_reference_warns_once() {
    let mut a = cartesian(0, 4800, [0., 1., 0.]);
    a.screen_ref = 1;
    let mut b = cartesian(4800, FRAMES, [0.5, 1., 0.]);
    b.screen_ref = 1;
    let mut scene = scene_of(vec![track(vec![a, b]), track(vec![a])]);
    scene.snapshot.tracks[1].channel = 0;
    let (_, logs) = ok(&request(&scene));
    let warning = "未配置重放屏幕，screenRef 保留原方向";
    assert_eq!(logs.iter().filter(|l| l.1 == warning).count(), 1);
    let mut mapped = request(&scene);
    mapped.screens = reproduction();
    let (p, logs) = ok(&mapped);
    assert!(logs.iter().all(|l| l.1 != warning));
    assert!(p.descriptions.iter().all(|d| d.screens.enabled == 1));
}
