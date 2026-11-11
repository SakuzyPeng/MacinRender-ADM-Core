//! The Apple AUSpatialMixer recipe (`apple::prepare`): buses, events, routing and logs.
mod common;

use common::{ObjectSpec, TrackSpec, direct_block, object_block, scene};
use mradm_render::apple::{self, Plan, Request, Routing, SourceMode};
use mradm_render::routing::{Level, LfeTarget, MatrixRoute};
use mradm_scene::RenderScene;

fn request<'a>(scene: &'a RenderScene, layout: &'a str) -> Request<'a> {
    Request {
        scene,
        layout_id: layout.as_bytes(),
        binaural: layout == "binaural",
        routing: Routing::Automatic,
        matrix: None,
        lfe_split_power: false,
        speaker_spread_none: false,
        binaural_spread_cloud: true,
        input_path: b"in.wav",
    }
}

fn run(
    r: &Request,
) -> (
    Result<Plan, mradm_render::routing::Error>,
    Vec<(Level, String)>,
) {
    let mut lines = Vec::new();
    let plan = apple::prepare(r, &mut |level, text| {
        lines.push((level, String::from_utf8(text).unwrap()));
    });
    (plan, lines)
}

fn with_channels(mut scene: RenderScene, channels: u16) -> RenderScene {
    scene.info.channels = channels;
    scene
}

fn object(tracks: Vec<TrackSpec>) -> ObjectSpec {
    ObjectSpec {
        id: "AO_1001",
        gain: 0.5,
        end: 1000,
        tracks,
    }
}

#[test]
fn a_point_object_is_one_point_source_bus_in_au_convention() {
    let mut block = object_block(0, 2000, 30.);
    block.position.elevation = 95.;
    block.position.distance = 0.;
    block.head_locked = 1;
    let s = scene(
        vec![object(vec![TrackSpec {
            blocks: vec![block],
            ..TrackSpec::default()
        }])],
        1000,
    );
    let (plan, lines) = run(&request(&s, "0+5+0"));
    let plan = plan.unwrap();
    assert_eq!(plan.routing, Routing::Label);
    assert_eq!(
        lines[0],
        (Level::Info, "DirectSpeakers routing: label".into())
    );
    assert_eq!(plan.spatial.len(), 1);
    let bus = &plan.spatial[0];
    assert_eq!(
        (bus.mode, bus.channel, bus.is_direct),
        (SourceMode::PointSource, 0, false)
    );
    assert_eq!(bus.object_id, b"AO_1001");
    let e = &bus.events[0];
    // A point extent keeps one source; the end is clipped to the object's.
    assert_eq!(bus.events.len(), 1);
    assert_eq!((e.start, e.end), (0, 1000));
    assert_eq!((e.azimuth, e.elevation, e.distance), (-30., 90., 1.0e-3));
    assert_eq!(e.gain, 0.5);
    assert!(e.head_locked);
}

#[test]
fn extent_spreads_into_the_cloud_unless_spreading_is_off() {
    let mut block = object_block(0, 1000, 0.);
    block.width = 90.;
    block.height = 30.;
    let s = scene(
        vec![object(vec![TrackSpec {
            blocks: vec![block],
            ..TrackSpec::default()
        }])],
        1000,
    );
    // Each cloud point is a coherent source on its own bus.
    let buses = run(&request(&s, "4+7+0")).0.unwrap().spatial;
    assert!(buses.len() > 1, "a wide object is a cloud of point sources");
    assert!(buses.iter().all(|b| b.events.len() == 1));
    let azimuths: Vec<f32> = buses.iter().map(|b| b.events[0].azimuth).collect();
    assert!(azimuths.iter().any(|&a| a > 1.) && azimuths.iter().any(|&a| a < -1.));

    let mut r = request(&s, "4+7+0");
    r.speaker_spread_none = true;
    assert_eq!(run(&r).0.unwrap().spatial.len(), 1);
    let mut r = request(&s, "binaural");
    r.binaural_spread_cloud = false;
    assert_eq!(run(&r).0.unwrap().spatial.len(), 1);
}

#[test]
fn divergence_adds_buses_and_pads_shorter_blocks_silent() {
    let mut diverging = object_block(500, 1000, 0.);
    diverging.divergence = 0.5;
    let s = scene(
        vec![object(vec![TrackSpec {
            blocks: vec![diverging, object_block(0, 500, 10.)],
            ..TrackSpec::default()
        }])],
        1000,
    );
    let mut r = request(&s, "0+5+0");
    r.speaker_spread_none = true;
    let plan = run(&r).0.unwrap();
    assert_eq!(plan.spatial.len(), 3);
    for (k, bus) in plan.spatial.iter().enumerate() {
        // Events follow the block starts; the first block has one source, so slots 1 and 2 are
        // silent there.
        assert_eq!(bus.events.len(), 2);
        assert_eq!((bus.events[0].start, bus.events[1].start), (0, 500));
        if k == 0 {
            assert_eq!(bus.events[0].azimuth, -10.);
        } else {
            assert_eq!(bus.events[0].gain, 0.);
            assert_eq!(bus.events[0].distance, 1.);
        }
        assert!(bus.events[1].gain > 0.);
    }
}

#[test]
fn direct_speakers_route_by_label_position_and_lfe() {
    let mut positioned = direct_block(0, 1000);
    positioned.azimuth = 60.;
    let s = with_channels(
        scene(
            vec![object(vec![
                TrackSpec {
                    direct: vec![(vec!["M+030"], direct_block(0, 1000))],
                    ..TrackSpec::default()
                },
                TrackSpec {
                    direct: vec![(vec!["X+060"], positioned)],
                    ..TrackSpec::default()
                },
                TrackSpec {
                    direct: vec![(vec!["LFE1"], direct_block(0, 1000))],
                    ..TrackSpec::default()
                },
            ])],
            1000,
        ),
        3,
    );
    let (plan, lines) = run(&request(&s, "0+5+0"));
    let plan = plan.unwrap();
    assert_eq!(plan.direct.len(), 1);
    assert_eq!(plan.direct[0].events[0].outputs, vec![(0, 1.)]);
    assert_eq!(plan.direct[0].label_key, b"M+030");
    // An unknown label without a label direction spatializes from its nominal position, which
    // this block has only in the records: the overlay decides.
    assert_eq!(plan.spatial.len(), 2);
    let bed = &plan.spatial[0];
    assert_eq!(bed.mode, SourceMode::AmbienceBed);
    let lfe = &plan.spatial[1];
    assert_eq!((lfe.mode, lfe.is_lfe), (SourceMode::Bypass, true));
    assert_eq!(lfe.events[0].lfe, LfeTarget::Lfe1);
    assert!(lines.iter().any(|(level, text)| *level == Level::Warning
        && text == "DirectSpeakers label 'X+060' not in output layout — spatializing from nominal position"));
    assert!(
        lines
            .last()
            .unwrap()
            .1
            .starts_with("routing 2 spatial buses, 1 direct side buses")
    );

    let mut r = request(&s, "0+5+0");
    r.routing = Routing::Position;
    let plan = run(&r).0.unwrap();
    assert_eq!(plan.direct.len(), 0);
    assert_eq!(plan.spatial[0].events[0].azimuth, 0.);
}

#[test]
fn lfe_side_buses_apply_on_22_2() {
    let s = with_channels(
        scene(
            vec![object(vec![
                TrackSpec {
                    direct: vec![(vec!["LFE1"], direct_block(0, 1000))],
                    ..TrackSpec::default()
                },
                TrackSpec {
                    direct: vec![(vec!["LFE2"], direct_block(0, 1000))],
                    ..TrackSpec::default()
                },
            ])],
            1000,
        ),
        2,
    );
    let plan = run(&request(&s, "9+10+3")).0.unwrap();
    assert!(plan.lfe_routing.applies_to_22_2);
    assert_eq!(plan.lfe.len(), 2);
    assert_eq!(plan.lfe[1].events[0].lfe, LfeTarget::Lfe2);
    assert!(plan.spatial.is_empty());
}

#[test]
fn binaural_checks_the_routing_first() {
    let s = scene(vec![object(vec![TrackSpec::default()])], 1000);
    for (routing, text) in [
        (
            Routing::Label,
            "Apple binaural output does not support DirectSpeakers label routing; use position",
        ),
        (
            Routing::Matrix,
            "Apple binaural output does not support DirectSpeakers matrix routing",
        ),
    ] {
        let mut r = request(&s, "binaural");
        r.routing = routing;
        let (plan, lines) = run(&r);
        let e = plan.unwrap_err();
        assert_eq!(
            (e.code, e.message.as_slice(), e.context.as_slice()),
            (2, text.as_bytes(), &b"layout=binaural"[..])
        );
        assert!(lines.is_empty());
    }
    let (plan, lines) = run(&request(&s, "binaural"));
    assert_eq!(plan.unwrap().routing, Routing::Position);
    assert_eq!(lines[0].1, "DirectSpeakers routing: position");
    assert_eq!(
        lines.last().unwrap().1,
        "no renderable tracks found (all muted?), writing silence"
    );
}

#[test]
fn matrix_routing_needs_a_matrix_and_resolves_it() {
    let s = scene(
        vec![object(vec![TrackSpec {
            direct: vec![(vec!["M+000"], direct_block(0, 1000))],
            ..TrackSpec::default()
        }])],
        1000,
    );
    let mut r = request(&s, "0+5+0");
    r.routing = Routing::Matrix;
    let e = run(&r).0.unwrap_err();
    assert_eq!(
        (e.code, e.message.as_slice()),
        (
            1,
            &b"DirectSpeakers matrix routing requires a parsed matrix"[..]
        )
    );
    let routes = [MatrixRoute {
        source_label: b"M+000",
        mute: false,
        targets: vec![(b"M+030", 0.5), (b"M-030", 0.5)],
    }];
    r.matrix = Some(&routes);
    let plan = run(&r).0.unwrap();
    assert_eq!(plan.direct[0].events[0].outputs, vec![(0, 0.5), (1, 0.5)]);
}

#[test]
fn a_channel_outside_the_input_fails_and_muted_objects_are_skipped() {
    let s = scene(
        vec![object(vec![
            TrackSpec {
                blocks: vec![object_block(0, 1000, 0.)],
                ..TrackSpec::default()
            },
            TrackSpec {
                blocks: vec![object_block(0, 1000, 0.)],
                ..TrackSpec::default()
            },
        ])],
        1000,
    );
    let e = run(&request(&s, "0+5+0")).0.unwrap_err();
    assert_eq!(e.code, 4);
    assert_eq!(
        e.message,
        b"track channel index 1 is outside input channel count 1"
    );
    assert_eq!(e.context, b"input=in.wav");
    let mut muted = s.clone();
    muted.overlay.objects[0].mute = true;
    let (plan, lines) = run(&request(&muted, "0+5+0"));
    assert!(plan.unwrap().spatial.is_empty());
    assert_eq!(
        lines.last().unwrap().1,
        "no renderable tracks found (all muted?), writing silence"
    );
}

#[test]
fn the_effective_position_comes_from_the_overlay() {
    let mut block = direct_block(0, 1000);
    block.azimuth = 60.;
    block.distance = 2.;
    let mut s = scene(
        vec![object(vec![TrackSpec {
            direct: vec![(vec!["X+060"], block)],
            ..TrackSpec::default()
        }])],
        1000,
    );
    let (plan, lines) = run(&request(&s, "0+5+0"));
    let e = &plan.unwrap().spatial[0].events[0];
    assert_eq!((e.azimuth, e.distance), (0., 1.));
    assert!(
        lines
            .iter()
            .any(|(_, t)| t.starts_with("DirectSpeakers block has no nominal position"))
    );
    // A policy position override: the record keeps the authored flag, the overlay is effective.
    s.overlay.direct_blocks[0].has_position = true;
    let (plan, lines) = run(&request(&s, "0+5+0"));
    let e = &plan.unwrap().spatial[0].events[0];
    assert_eq!((e.azimuth, e.distance), (-60., 2.));
    assert!(
        !lines
            .iter()
            .any(|(_, t)| t.starts_with("DirectSpeakers block has no nominal position"))
    );
}
