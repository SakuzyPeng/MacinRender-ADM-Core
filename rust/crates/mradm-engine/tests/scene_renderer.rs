//! The Scene stream renderer creation (PCM consistency 5.4 A): the selection, each renderer's
//! checks, their order, texts and context, the rate it renders at and the checkpoint suffix, as
//! the C++ `create_live_renderer` and `create_live_*_renderer` shells had them.
mod common;
use common::backend::{TestHost, repo};
use mradm_dsp::room::loading;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, RoomRequest, Screens, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::renderer::{Config, Created, create};
use mradm_render::routing::Error;
use std::sync::Arc;

fn config(selection: Selection, layout: &'static [u8]) -> Config<'static> {
    Config {
        selection,
        layout_id: layout,
        apple_geometry: false,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread: BinauralSpreadMode::Automatic,
        split_power: false,
        smoothing_frames: 0,
        sample_rate: 48000,
        sofa: None,
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry {
                cartesian: 0,
                reserved: 0,
                aspect_ratio: 1.78,
                centre: [0., 0., 1.],
                width: 58.,
            },
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        room: RoomRequest {
            package: None,
            hrir: b"",
            metres: 0.,
            quality: 0,
        },
        sink: true,
    }
}

fn room_config(layout: &'static [u8]) -> Config<'static> {
    let manifest = repo().join("tests/fixtures/room/phase2/room.json");
    let mut c = config(Selection::RoomBinaural, layout);
    c.room.package = Some(Arc::new(
        loading::load_package(manifest.to_str().unwrap().as_bytes()).unwrap(),
    ));
    c
}

fn created(c: &Config) -> Created {
    match create(c, &TestHost) {
        Ok(created) => created,
        Err(e) => panic!("{}", String::from_utf8_lossy(&e.message)),
    }
}

fn rejected(c: &Config) -> (i32, String, String) {
    let Err(Error {
        code,
        message,
        context,
    }) = create(c, &TestHost)
    else {
        panic!("{:?} created", c.selection);
    };
    (
        code,
        String::from_utf8(message).unwrap(),
        String::from_utf8(context).unwrap(),
    )
}

#[test]
fn only_the_four_scene_renderers_are_created() {
    for selection in [
        Selection::Automatic,
        Selection::Ear,
        Selection::Hoa,
        Selection::Apple,
        Selection::Binaural,
    ] {
        assert_eq!(
            rejected(&config(selection, b"0+5+0")),
            (
                2,
                "Scene streams support SAF VBAP, SAF binaural, room-binaural and Triple Balance"
                    .into(),
                String::new()
            ),
            "{selection:?}"
        );
    }
}

#[test]
fn speaker_and_binaural_renderers_report_their_suffix_rate_and_channels() {
    let vbap = created(&config(Selection::Saf, b"4+7+0"));
    assert_eq!(vbap.renderer.output_channels(), 12);
    assert_eq!(vbap.renderer.trace_suffix(), ".30-vbap12");
    assert_eq!(vbap.sample_rate, 48000);
    assert!(vbap.renderer.control().is_none());

    let stereo = created(&config(Selection::Saf, b"0+2+0"));
    assert_eq!(stereo.renderer.trace_suffix(), ".30-vbap2");

    let tb = created(&config(Selection::TripleBalance, b"4+7+0"));
    assert_eq!(tb.renderer.trace_suffix(), ".30-triple");
    assert_eq!(tb.renderer.names().0, "Live Triple Balance");

    let mut b = config(Selection::SafBinaural, b"binaural");
    b.sample_rate = 44100;
    let binaural = created(&b);
    assert_eq!(binaural.renderer.trace_suffix(), ".30-binaural");
    assert_eq!(binaural.renderer.output_channels(), 2);
    assert_eq!(binaural.sample_rate, 44100);
    assert!(binaural.renderer.tail() > 0);
}

#[test]
fn speaker_renderer_rejections_carry_no_context() {
    let (code, message, context) = rejected(&config(Selection::Saf, b"7.1.4x"));
    assert_eq!(code, 2);
    assert_eq!(message, "unsupported live VBAP output layout '7.1.4x'");
    assert_eq!(context, "");

    // Triple Balance: speaker_spread none / automatic are accepted, MDAP and a SOFA path are not.
    let fixed =
        "Live Triple Balance requires fixed room geometry, no MDAP/SOFA and 48 kHz for 22.2";
    let mut tb = config(Selection::TripleBalance, b"4+7+0");
    for spread in [SpeakerSpread::Automatic, SpeakerSpread::None] {
        tb.speaker_spread = spread;
        created(&tb);
    }
    tb.speaker_spread = SpeakerSpread::Mdap;
    assert_eq!(rejected(&tb), (2, fixed.into(), String::new()));
    tb.speaker_spread = SpeakerSpread::Automatic;
    tb.sofa = Some(mradm_engine::backend::Sofa {
        path: b"/nonexistent.sofa",
        file_name: b"nonexistent.sofa",
    });
    assert_eq!(rejected(&tb), (2, fixed.into(), String::new()));
    tb.sofa = None;
    tb.apple_geometry = true;
    assert_eq!(rejected(&tb), (2, fixed.into(), String::new()));
    let (code, message, _) = rejected(&config(Selection::TripleBalance, b"0+5+0"));
    assert_eq!(
        (code, message.as_str()),
        (2, "Unsupported Triple Balance layout")
    );
}

#[test]
fn room_checks_screens_then_the_package_then_output_and_spread() {
    let spreader = "room-binaural 不支持 SAF covariance spreader";
    let output = "room-binaural 需要与房间包采样率一致的双耳输出";

    let mut bad_screen = config(Selection::RoomBinaural, b"0+2+0");
    bad_screen.screens.reference.width = -10.;
    bad_screen.binaural_spread = BinauralSpreadMode::SafSpreader;
    let (code, message, context) = rejected(&bad_screen);
    assert_eq!(
        (code, message.as_str(), context.as_str()),
        (1, "Invalid screen geometry or edge ordering", "screen")
    );

    let mut no_package = config(Selection::RoomBinaural, b"0+2+0");
    no_package.binaural_spread = BinauralSpreadMode::SafSpreader;
    assert_eq!(
        rejected(&no_package),
        (1, "room-binaural 需要房间包".into(), String::new())
    );

    // The output check (layout compared as given, the rate against the package's) precedes the
    // spreader check.
    for (layout, rate) in [
        (&b"0+2+0"[..], 48000),
        (b"Binaural", 48000),
        (b"binaural", 44100),
    ] {
        let mut c = room_config(b"binaural");
        c.layout_id = layout;
        c.sample_rate = rate;
        c.binaural_spread = BinauralSpreadMode::SafSpreader;
        assert_eq!(rejected(&c), (2, output.into(), String::new()));
    }
    let mut c = room_config(b"binaural");
    c.binaural_spread = BinauralSpreadMode::SafSpreader;
    assert_eq!(rejected(&c), (2, spreader.into(), String::new()));

    let mut quality = room_config(b"binaural");
    quality.room.quality = 9;
    let (code, _, context) = rejected(&quality);
    assert_eq!((code, context.as_str()), (1, "room-binaural"));
}

#[test]
fn room_renderer_renders_at_the_package_rate_with_its_control() {
    for spread in [
        BinauralSpreadMode::Automatic,
        BinauralSpreadMode::None,
        BinauralSpreadMode::Cloud,
    ] {
        let mut c = room_config(b"binaural");
        c.binaural_spread = spread;
        let room = created(&c);
        assert_eq!(room.sample_rate, 48000);
        assert_eq!(room.renderer.trace_suffix(), "");
        assert_eq!(room.renderer.output_channels(), 2);
        assert_eq!(room.renderer.names().0, "room-binaural");
        assert!(room.renderer.control().is_some());
    }
}
