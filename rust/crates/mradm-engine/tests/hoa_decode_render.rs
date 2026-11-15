//! EAR decodes of an HOA1 pack through the render service (formerly the C++
//! `hoa_decode_fixture_test`, R3 C1): W reaches every non-LFE speaker of 5.1 and 9.1.6, object mute
//! and gain apply, a later gain-0 block gates the signal after the compensation delay, and a UID
//! the object references without a track or pack contributes nothing. HOA pack normalisation,
//! gains and the decode errors are `mradm-render`'s `ear.rs`.
mod common;
use common::render::*;
use mradm_engine::request::Selection;
use mradm_testkit::adm::*;
use mradm_testkit::fixtures::wire_channel;
use mradm_testkit::wave::{AudioId, Wave};

const FRAMES: usize = 2048;
const ORDERS: [(i32, i32); 4] = [(0, 0), (1, -1), (1, 0), (1, 1)];

/// One HOA1 pack in one object; the channels' blocks from `blocks(order, degree)`. With `phantom`
/// the object also references a UID with no track or pack.
fn hoa1(
    blocks: impl Fn(i32, i32) -> Vec<Properties>,
    object: Vec<Param>,
    phantom: bool,
) -> (String, Vec<String>) {
    let mut doc = Document::new();
    let pack = doc.pack(Type::Hoa, [pack_name("HOA1PF")]);
    let obj = doc.object([object_name("HOA1Object")]);
    for p in object {
        doc.set(obj, p);
    }
    let mut uids = Vec::new();
    for (i, (o, d)) in ORDERS.iter().enumerate() {
        let channel = doc.channel(Type::Hoa, [channel_name(format!("HOA1CF_{i}"))]);
        for block in blocks(*o, *d) {
            doc.block(channel, BlockKind::Hoa, block);
        }
        doc.add(channel);
        doc.reference(pack, channel);
        uids.push(wire_channel(&mut doc, channel, pack, &i.to_string()));
    }
    doc.add(pack);
    for &u in &uids {
        doc.reference(obj, u);
    }
    if phantom {
        let u = doc.track_uid();
        doc.add(u);
        doc.reference(obj, u);
        uids.push(u);
    }
    doc.add(obj);
    let content = doc.content([content_name("HOA1Content")]);
    doc.reference(content, obj);
    doc.add(content);
    let programme = doc.programme([programme_name("HOA1Programme")]);
    doc.reference(programme, content);
    doc.add(programme);
    doc.reassign_ids();
    let ids = uids.iter().map(|&u| doc.id(u).to_owned()).collect();
    (doc.xml(), ids)
}

fn plain(o: i32, d: i32) -> Vec<Properties> {
    vec![hoa_block([order(o), degree(d)])]
}

/// Renders with signal 0.5 on input channel `signal` only.
fn decode(scene: (String, Vec<String>), signal: usize, layout: &str) -> Audio {
    let dir = scratch("hoa-decode");
    let path = dir.file("in.wav");
    let (axml, uids) = scene;
    let mut wave = Wave::new(uids.len() as u16, 48000, 24);
    wave.chna = Some(
        uids.iter()
            .enumerate()
            .map(|(i, u)| AudioId::new(i as u16 + 1, u, "", ""))
            .collect(),
    );
    wave.axml = Some(axml);
    let channels = uids.len();
    let samples: Vec<f32> = (0..FRAMES * channels)
        .map(|i| if i % channels == signal { 0.5 } else { 0. })
        .collect();
    wave.write(&path, &samples).unwrap();
    let mut r = request(&path, &dir.file("out.wav"), Selection::Ear, layout);
    r.peak_limit = false;
    render(r).1
}

fn rms_in(audio: &Audio, channel: usize, range: std::ops::Range<usize>) -> f64 {
    let n = range.len() as f64;
    (range
        .map(|f| f64::from(audio.samples[f * audio.channels + channel]).powi(2))
        .sum::<f64>()
        / n)
        .sqrt()
}

fn rms(audio: &Audio) -> Vec<f64> {
    (0..audio.channels)
        .map(|c| rms_in(audio, c, 0..audio.frames))
        .collect()
}

#[test]
fn w_reaches_every_speaker() {
    let r = rms(&decode(hoa1(plain, vec![], false), 0, "0+5+0"));
    assert_eq!(r.len(), 6);
    assert!(
        [0, 1, 2, 4, 5].iter().all(|&c| r[c] > 0.01) && r[3] < 1e-5,
        "{r:?}"
    );
    assert!((r[0] - r[1]).abs() < r[0] * 0.1);
    let r = rms(&decode(hoa1(plain, vec![], false), 0, "9.1.6"));
    assert_eq!(r.len(), 16);
    assert!(r[0] > 0.01 && r[1] > 0.01 && r[3] < 1e-5, "{r:?}");
}

#[test]
fn object_mute_and_gain_apply() {
    let r = rms(&decode(hoa1(plain, vec![mute(true)], false), 0, "0+5+0"));
    assert!(r.iter().all(|v| *v < 1e-6));
    let full = rms(&decode(hoa1(plain, vec![], false), 0, "0+5+0"));
    let half = rms(&decode(
        hoa1(plain, vec![gain_linear(0.5)], false),
        0,
        "0+5+0",
    ));
    assert!((0.45..0.55).contains(&(half[0] / full[0])));
}

#[test]
fn a_gain_zero_block_gates_after_the_compensation_delay() {
    let split = 21_333_334;
    let blocks = |o, d| {
        vec![
            hoa_block([order(o), degree(d), duration(split)]),
            hoa_block([order(o), degree(d), rtime(split), gain_linear(0.0)]),
        ]
    };
    let audio = decode(hoa1(blocks, vec![], false), 0, "0+5+0");
    let half = FRAMES / 2;
    assert!(rms_in(&audio, 0, 0..half) > 0.01);
    assert!(rms_in(&audio, 0, half + 256..FRAMES) < 1e-5);
}

#[test]
fn a_uid_without_a_track_contributes_nothing() {
    let r = rms(&decode(hoa1(plain, vec![], true), 4, "0+5+0"));
    assert!(r.iter().all(|v| *v < 1e-5), "{r:?}");
}
