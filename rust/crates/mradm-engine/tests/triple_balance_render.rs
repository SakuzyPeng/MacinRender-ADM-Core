//! Triple Balance D mode through the render service (formerly the D-mode half of the C++
//! `triple_balance_stream_test`, R3 C2), on its scene: an RC 7.1.2 bed and three Cartesian objects
//! with events across control-block edges. D mode starts from silence, reports its effective model,
//! routes 22.2 LFE with equal power, crops a window exactly, repeats, ignores size with spread
//! none, and rejects other layouts and rates. The standard mode's stream, seeks, live edits and
//! their policy equivalence are `mradm-render`'s `triple_balance_stream.rs`; D mode's realtime
//! rejection is the `monitor-v1` goldens; its kernel is `mradm-dsp`'s `triple_balance_d.rs`.
mod common;
use common::render::*;
use mradm_engine::backend::SpeakerSpread;
use mradm_engine::render::Request;
use mradm_engine::request::Selection;
use mradm_testkit::wave::{AudioId, Wave};

const FRAMES: usize = 100_037;
const CHANNELS: usize = 13;

#[derive(Clone, Copy, Default)]
struct Variant {
    /// The objects go to the lower layer after frame 1024 (22.2).
    lower: bool,
    /// No Objects extent.
    zero: bool,
    rate: u32,
}

fn timecode(frame: u64, rate: u32) -> String {
    let ns = frame * 1_000_000_000 / u64::from(rate);
    format!("00:00:{:02}.{:09}", ns / 1_000_000_000, ns % 1_000_000_000)
}

/// The C++ test's scene: bed AO_1001 on channels 1-10, objects AO_1010-AO_1012 on 11-13.
fn scene(v: Variant) -> (String, Vec<AudioId>) {
    let labels = [
        "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts",
        "RC_Rts",
    ];
    let (mut formats, mut uids, mut objects, mut top) =
        (String::new(), String::new(), String::new(), String::new());
    let mut chna = Vec::new();
    let mut track = |formats: &mut String, n: usize, ty: &str, blocks: &str, pack: &str| {
        let code = format!("{ty}{:04X}", 0x1000 + n);
        let (cf, tf, sf, uid) = (
            format!("AC_{code}"),
            format!("AT_{code}_01"),
            format!("AS_{code}"),
            format!("ATU_{n:08X}"),
        );
        *formats += &format!(
            r#"<audioChannelFormat audioChannelFormatID="{cf}" audioChannelFormatName="CF{n}" typeLabel="{ty}">{blocks}</audioChannelFormat><audioStreamFormat audioStreamFormatID="{sf}" audioStreamFormatName="SF{n}" formatDefinition="pcm" formatLabel="0001"><audioChannelFormatIDRef>{cf}</audioChannelFormatIDRef><audioTrackFormatIDRef>{tf}</audioTrackFormatIDRef></audioStreamFormat><audioTrackFormat audioTrackFormatID="{tf}" audioTrackFormatName="TF{n}" formatDefinition="pcm" formatLabel="0001"><audioStreamFormatIDRef>{sf}</audioStreamFormatIDRef></audioTrackFormat>"#
        );
        uids += &format!(
            r#"<audioTrackUID UID="{uid}"><audioTrackFormatIDRef>{tf}</audioTrackFormatIDRef><audioPackFormatIDRef>{pack}</audioPackFormatIDRef></audioTrackUID>"#
        );
        chna.push(AudioId::new(n as u16, &uid, &tf, pack));
        (cf, uid)
    };
    let (mut refs, mut bed) = (String::new(), String::new());
    for (i, label) in labels.iter().enumerate() {
        let ch = i + 1;
        let block = format!(
            r#"<audioBlockFormat audioBlockFormatID="AB_0001{}_00000001"><position coordinate="X">0</position><position coordinate="Y">1</position><position coordinate="Z">0</position><cartesian>1</cartesian><speakerLabel>{label}</speakerLabel></audioBlockFormat>"#,
            1000 + ch
        );
        let (cf, uid) = track(&mut formats, ch, "0001", &block, "AP_00011000");
        refs += &format!("<audioChannelFormatIDRef>{cf}</audioChannelFormatIDRef>");
        bed += &format!("<audioTrackUIDRef>{uid}</audioTrackUIDRef>");
    }
    formats += &format!(
        r#"<audioPackFormat audioPackFormatID="AP_00011000" audioPackFormatName="Bed" typeLabel="0001">{refs}</audioPackFormat>"#
    );
    objects += &format!(
        r#"<audioObject audioObjectID="AO_1001" audioObjectName="bed">{bed}</audioObject>"#
    );
    top += "<audioObjectIDRef>AO_1001</audioObjectIDRef>";
    for ch in 11..=13usize {
        let mut blocks = String::new();
        for (index, frame) in [0u64, 31, 511, 512, 1025, 1051, 1126, 16384, 16391, 66049]
            .iter()
            .enumerate()
        {
            let mut x: f32 = if *frame == 0 { -0.4 } else { 0.3 };
            if *frame != 0 && frame % 3 == 0 {
                x = -0.2;
            }
            let y: f32 = if *frame < 16384 { 0.2 } else { -0.3 };
            let z: f32 = if v.lower && *frame > 1024 { -0.5 } else { 0.5 };
            let mut size: f32 = if *frame == 16384 { 0.7 } else { 0.3 };
            if v.zero || ch == 13 {
                size = 0.;
            }
            let g = |value: f32| mradm_testkit::adm::g(f64::from(value), 9);
            blocks += &format!(
                r#"<audioBlockFormat audioBlockFormatID="AB_0003{}_{:08}" rtime="{}"><position coordinate="X">{}</position><position coordinate="Y">{}</position><position coordinate="Z">{}</position><cartesian>1</cartesian><width>{}</width><height>{}</height><depth>{}</depth></audioBlockFormat>"#,
                1000 + ch,
                index + 1,
                timecode(*frame, v.rate),
                g(x),
                g(y),
                g(z),
                g(size),
                g(size),
                g(size)
            );
        }
        let pack = format!("AP_0003{}", 1000 + ch);
        let (cf, uid) = track(&mut formats, ch, "0003", &blocks, &pack);
        formats += &format!(
            r#"<audioPackFormat audioPackFormatID="{pack}" audioPackFormatName="PF{ch}" typeLabel="0003"><audioChannelFormatIDRef>{cf}</audioChannelFormatIDRef></audioPackFormat>"#
        );
        let id = format!("AO_10{}", ch - 1);
        objects += &format!(
            r#"<audioObject audioObjectID="{id}" audioObjectName="object-{}"><audioTrackUIDRef>{uid}</audioTrackUIDRef></audioObject>"#,
            ch - 1
        );
        top += &format!("<audioObjectIDRef>{id}</audioObjectIDRef>");
    }
    let xml = format!(
        r#"<?xml version="1.0" encoding="UTF-8"?><ebuCoreMain><coreMetadata><format><audioFormatExtended>{formats}{uids}{objects}<audioContent audioContentID="ACO_1001" audioContentName="C">{top}</audioContent><audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="P"><audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme></audioFormatExtended></format></coreMetadata></ebuCoreMain>"#
    );
    (xml, chna)
}

fn input_of(dir: &Dir, v: Variant) -> std::path::PathBuf {
    let path = dir.file("in.wav");
    let (xml, chna) = scene(v);
    let mut wave = Wave::new(CHANNELS as u16, v.rate, 24);
    wave.chna = Some(chna);
    wave.axml = Some(xml);
    let samples: Vec<f32> = (0..FRAMES * CHANNELS)
        .map(|i| 0.05 * (i as f32 * 0.019).sin())
        .collect();
    wave.write(&path, &samples).unwrap();
    path
}

/// A D-mode render of the variant and its PCM; `edit` adjusts the request.
fn d_mode(layout: &str, v: Variant, edit: impl FnOnce(&mut Request)) -> (Run, Option<Audio>) {
    let dir = scratch("triple-d");
    let input = input_of(&dir, v);
    let mut r = request(
        &input,
        &dir.file("out.wav"),
        Selection::TripleBalance,
        layout,
    );
    r.peak_limit = false;
    r.triple_balance_mode = 1;
    r.capture_semantic_report = true;
    r.split_power = layout == "9+10+3";
    edit(&mut r);
    let output = std::path::PathBuf::from(&r.output.as_ref().unwrap().utf8);
    let run = run(r);
    let audio = run.ok().then(|| Audio::read(&output));
    (run, audio)
}

fn pcm(result: (Run, Option<Audio>)) -> Audio {
    let (run, audio) = result;
    audio.unwrap_or_else(|| panic!("{:?}", run.outcome.error))
}

fn variant(lower: bool) -> Variant {
    Variant {
        lower,
        rate: 48000,
        ..Variant::default()
    }
}

#[test]
fn d_mode_renders_report_crop_and_repeat() {
    for layout in ["9.1.6", "9+10+3"] {
        let extended = layout == "9+10+3";
        let channels = if extended { 24 } else { 16 };
        let (run, full) = d_mode(layout, variant(extended), |_| {});
        let full = full.unwrap_or_else(|| panic!("{layout}: {:?}", run.outcome.error));
        assert_eq!((full.channels, full.frames), (channels, FRAMES));
        assert!(
            full.samples[..channels].iter().all(|v| *v == 0.),
            "the startup ramp starts silent"
        );
        let report = run.outcome.semantic_report.unwrap();
        assert!(
            report.contains(r#""mode": "d""#) && report.contains(r#""gain_control_frames": 1536"#),
            "{report}"
        );
        if extended {
            assert!(report.contains("triple-balance-d-222-v1"));
            assert!(report.contains(r#""lfe_routing": "split-power""#));
            assert!(report.contains(r#""size_decorrelation": false"#));
            assert!(!report.contains("target_mix_spread_gains"));
            let direct = pcm(d_mode(layout, variant(true), |r| r.split_power = false));
            for (f, d) in full
                .samples
                .as_chunks::<24>()
                .0
                .iter()
                .zip(direct.samples.as_chunks::<24>().0.iter())
            {
                assert_eq!(d[9], 0.);
                assert_eq!(f[3], f[9], "both 22.2 LFE channels carry one signal");
                assert!(
                    (f[3] - d[3] * 0.5f32.sqrt()).abs() < 1e-7,
                    "equal power, startup included"
                );
                for c in (0..24).filter(|c| *c != 3 && *c != 9) {
                    assert_eq!(f[c], d[c]);
                }
            }
        }

        let (start, count) = (1537usize, 2051usize);
        let cropped = pcm(d_mode(layout, variant(extended), |r| {
            r.render_start_sec = start as f64 / 48000.;
            r.render_end_sec = Some((start + count) as f64 / 48000.);
        }));
        assert_eq!(
            cropped.samples,
            full.samples[start * channels..(start + count) * channels]
        );

        let again = pcm(d_mode(layout, variant(extended), |_| {}));
        assert_eq!(
            again.samples, full.samples,
            "a repeated render is identical"
        );

        let ignored = pcm(d_mode(layout, variant(extended), |r| {
            r.speaker_spread = SpeakerSpread::None as u32
        }));
        let zero = Variant {
            zero: true,
            ..variant(extended)
        };
        assert_eq!(
            ignored.samples,
            pcm(d_mode(layout, zero, |_| {})).samples,
            "spread none ignores size"
        );
    }
}

#[test]
fn d_mode_rejects_other_layouts_and_rates() {
    for layout in ["4+7+0", "0+5+0"] {
        assert_eq!(
            d_mode(layout, variant(false), |r| r.split_power = false)
                .0
                .code(),
            2,
            "{layout}"
        );
    }
    let other = Variant {
        rate: 44100,
        ..variant(false)
    };
    assert!(!d_mode("9.1.6", other, |_| {}).0.ok());
}
