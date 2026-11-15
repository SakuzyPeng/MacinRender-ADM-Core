//! The deterministic ADM BWF fixtures of the consistency matrix (formerly
//! `tests/tools/make_fixture.cpp`). The inputs are bit-exact by construction:
//!
//! - samples come from a 32-bit LCG; the integer is mapped to `f32` by subtracting 2^23 and
//!   dividing by 2^23, both exact in binary32, so no libm or rounding choice is involved;
//! - positions, extents and gains are exact decimals that round-trip through the XML text.
//!
//! Kinds (see [`KINDS`]):
//!
//! - `objects-point`: one Objects channel without extent (the clean gain path).
//! - `objects-extent`: one Objects channel with width / height / diffuse (decorrelator and, with
//!   `--binaural-spread-mode saf-spreader`, the OM spreader).
//! - `objects-extent-multi`: three Objects tracks (several OLA sources and spreader groups).
//! - `objects-cartesian`: one Cartesian Objects channel with extent; its vector length is exact
//!   (0.25² + 0.75² + 0.375² = 0.875²), a decidable probe of the length rounding.
//! - `directspeakers`: a 5.1 bed (DirectSpeakers gains and LFE routing).
//! - `directspeakers-lfe`: the 5.1 bed with an ADM lowPass on LFE1 (PCM consistency 4.2).
//! - `hoa`: HOA1 (4 channels); `hoa3`: HOA3 (16 channels, PCM consistency 3.3).
//! - `objects-room`: one Cartesian Objects channel with equal-size extent moving over four blocks
//!   (the Triple Balance size path, PCM consistency 3.4); `objects-room-96k` the same at 96 kHz
//!   for one second (the room's default ramp, 5.7).
//! - `params`: 40 same-start Objects blocks and a Cartesian DirectSpeakers channel, both with dB
//!   gains (block order and import projections, PCM consistency 3.5).
//! - `policy-objects`: four named objects in two contents with importance, dialogue,
//!   channelLock, divergence, jumpPosition, headLocked and a Cartesian screenRef object (the
//!   semantic policy matrix and screen configuration, PCM consistency 4.2).
use crate::adm::*;
use crate::wave::{AudioId, Wave};
use std::io;
use std::path::Path;

pub const SAMPLE_RATE: u32 = 48000;
/// One second, keeping CI artifacts small.
pub const FRAMES: u32 = 48000;
pub const HIGH_RATE: u32 = 96000;
pub const BIT_DEPTH: u16 = 24;

/// Every fixture kind, in the order of the generator's usage text.
pub const KINDS: [&str; 12] = [
    "objects-point",
    "objects-extent",
    "objects-extent-multi",
    "objects-cartesian",
    "objects-room",
    "objects-room-96k",
    "params",
    "directspeakers",
    "directspeakers-lfe",
    "hoa",
    "hoa3",
    "policy-objects",
];

/// Numerical Recipes LCG: trivially reproducible, broadband and identical everywhere.
pub fn lcg_next(state: &mut u32) -> u32 {
    *state = state.wrapping_mul(1664525).wrapping_add(1013904223);
    *state
}

/// Exact in binary32: `r >> 8` is in [0, 2^24), the difference converts exactly and the division
/// by 2^23 only changes the exponent.
pub fn exact_sample(r: u32) -> f32 {
    ((r >> 8) as i32 - 8_388_608) as f32 / 8_388_608.0
}

/// `channels * frames` interleaved samples from `seed`.
pub fn signal(channels: usize, frames: usize, seed: u32) -> Vec<f32> {
    let mut state = seed;
    (0..channels * frames)
        .map(|_| exact_sample(lcg_next(&mut state)))
        .collect()
}

/// A built document and its track UIDs in track order.
pub struct Built {
    pub doc: Document,
    pub uids: Vec<Id>,
}

impl Built {
    /// The UID strings in track order.
    pub fn uid_strings(&self) -> Vec<String> {
        self.uids
            .iter()
            .map(|&u| self.doc.id(u).to_owned())
            .collect()
    }

    /// Writes the WAVE file: one CHNA entry per track, the AXML, and `frames` samples per channel
    /// of the LCG signal from `seed`.
    pub fn write(&self, path: &Path, seed: u32, rate: u32, frames: u32) -> io::Result<()> {
        let uids = self.uid_strings();
        let mut wave = Wave::new(uids.len() as u16, rate, BIT_DEPTH);
        wave.chna = Some(
            uids.iter()
                .enumerate()
                .map(|(i, uid)| AudioId::new(i as u16 + 1, uid, "", ""))
                .collect(),
        );
        wave.axml = Some(self.doc.xml());
        wave.write(path, &signal(uids.len(), frames as usize, seed))
    }
}

/// Wires one channel into the stream / track / UID chain and returns the UID.
pub fn wire_channel(doc: &mut Document, cf: Id, pf: Id, suffix: &str) -> Id {
    let sf = doc.stream([stream_name(format!("SF_{suffix}"))]);
    doc.reference(sf, cf);
    doc.add(sf);
    let tf = doc.track([track_name(format!("TF_{suffix}"))]);
    doc.reference(tf, sf);
    doc.reference(sf, tf);
    doc.add(tf);
    let uid = doc.track_uid();
    doc.reference(uid, tf);
    doc.reference(uid, pf);
    doc.add(uid);
    uid
}

/// One object over `uids` in one content of one programme, then the identifiers.
pub fn finish(doc: &mut Document, uids: &[Id], name: &str) {
    let obj = doc.object([object_name(format!("{name}Object"))]);
    for &u in uids {
        doc.reference(obj, u);
    }
    doc.add(obj);
    let content = doc.content([content_name(format!("{name}Content"))]);
    doc.reference(content, obj);
    doc.add(content);
    let prog = doc.programme([programme_name(format!("{name}Programme"))]);
    doc.reference(prog, content);
    doc.add(prog);
    doc.reassign_ids();
}

const MS: i64 = 1_000_000;
const S: i64 = 1_000_000_000;

pub fn objects(with_extent: bool, track_count: usize) -> Built {
    let mut doc = Document::new();
    let mut uids = Vec::new();
    for i in 0..track_count {
        let suffix = if track_count == 1 {
            String::new()
        } else {
            i.to_string()
        };
        let cf = doc.channel(Type::Objects, [channel_name(format!("ObjCF{suffix}"))]);
        let mut block = objects_block([spherical([
            azimuth(30.0f32 - (i as f32 * 60.0)),
            elevation(0.0f32),
        ])]);
        if with_extent {
            // Width exceeds the spreader gate; diffuse keeps the OLA bus active as well.
            block.set(width(30.0f32));
            block.set(height(10.0f32));
            block.set(diffuse(0.5f32));
        }
        doc.block(cf, BlockKind::Objects, block);
        doc.add(cf);
        let pf = doc.pack(Type::Objects, [pack_name(format!("ObjPF{suffix}"))]);
        doc.reference(pf, cf);
        doc.add(pf);
        uids.push(wire_channel(&mut doc, cf, pf, &format!("Obj{suffix}")));
    }
    let name = if with_extent {
        "ObjectsExtent"
    } else {
        "ObjectsPoint"
    };
    let multi = if track_count == 1 { "" } else { "Multi" };
    finish(&mut doc, &uids, &format!("{name}{multi}"));
    Built { doc, uids }
}

/// The Cartesian counterpart of `objects(true, 1)`, with normalised extent (what BS.2076 specifies
/// for Cartesian objects, and what keeps the VBAP spread path measurable).
pub fn objects_cartesian() -> Built {
    let mut doc = Document::new();
    let cf = doc.channel(Type::Objects, [channel_name("ObjCartCF")]);
    let block = objects_block([
        cartesian_position([x(0.25f32), y(0.75f32), z(0.375f32)]),
        cartesian(true),
        width(0.5f32),
        height(0.25f32),
        depth(0.125f32),
        diffuse(0.5f32),
    ]);
    doc.block(cf, BlockKind::Objects, block);
    doc.add(cf);
    let pf = doc.pack(Type::Objects, [pack_name("ObjCartPF")]);
    doc.reference(pf, cf);
    doc.add(pf);
    let uids = vec![wire_channel(&mut doc, cf, pf, "ObjCart")];
    finish(&mut doc, &uids, "ObjectsCartesian");
    Built { doc, uids }
}

/// The semantic policy matrix: four objects with distinct names, importance and dialogue in two
/// contents, each moving over two 500 ms blocks with one semantic dimension.
pub fn policy_objects() -> Built {
    struct Spec {
        name: &'static str,
        importance: i32,
        dialogue: i32,
        cartesian: bool,
        positions: [[f32; 3]; 2],
    }
    let specs = [
        Spec {
            name: "Dialogue Lead",
            importance: 9,
            dialogue: 1,
            cartesian: false,
            positions: [[20.0, 0.0, 1.0], [-35.0, 15.0, 1.0]],
        },
        Spec {
            name: "Dialogue Alt",
            importance: 6,
            dialogue: 1,
            cartesian: false,
            positions: [[-40.0, 10.0, 1.0], [50.0, 5.0, 1.0]],
        },
        Spec {
            name: "Ambience Wide",
            importance: 3,
            dialogue: 0,
            cartesian: false,
            positions: [[110.0, 0.0, 1.0], [150.0, 30.0, 1.0]],
        },
        Spec {
            name: "FX Cartesian",
            importance: 5,
            dialogue: 0,
            cartesian: true,
            positions: [[0.5, 0.5, 0.25], [-0.25, 0.75, 0.5]],
        },
    ];
    let mut doc = Document::new();
    let mut uids = Vec::new();
    let mut objects = Vec::new();
    for (i, spec) in specs.iter().enumerate() {
        let suffix = format!("Pol{i}");
        let cf = doc.channel(Type::Objects, [channel_name(format!("CF_{suffix}"))]);
        for (b, p) in spec.positions.iter().enumerate() {
            let mut block = objects_block([]);
            if spec.cartesian {
                block.set(cartesian_position([x(p[0]), y(p[1]), z(p[2])]));
                block.set(cartesian(true));
                block.set(screen_ref(true));
            } else {
                block.set(spherical([azimuth(p[0]), elevation(p[1]), distance(p[2])]));
            }
            block.set(rtime(500 * MS * b as i64));
            block.set(duration(500 * MS));
            match i {
                0 => {
                    block.set(channel_lock([Param::text(true), max_distance(0.5f32)]));
                    block.set(jump_position([
                        Param::text(true),
                        interpolation_length(50.0 / 1000.0),
                    ]));
                }
                1 => block.set(object_divergence([
                    Param::text(0.5f32),
                    azimuth_range(30.0f32),
                ])),
                2 => {
                    block.set(width(40.0f32));
                    block.set(height(15.0f32));
                    block.set(diffuse(0.25f32));
                    block.set(head_locked(b == 1));
                }
                _ => {
                    block.set(width(0.25f32));
                    block.set(height(0.25f32));
                    block.set(depth(0.25f32));
                }
            }
            doc.block(cf, BlockKind::Objects, block);
        }
        doc.add(cf);
        let pf = doc.pack(Type::Objects, [pack_name(format!("PF_{suffix}"))]);
        doc.reference(pf, cf);
        doc.add(pf);
        let uid = wire_channel(&mut doc, cf, pf, &suffix);
        uids.push(uid);
        let obj = doc.object([object_name(spec.name), importance(spec.importance)]);
        if spec.dialogue != 0 {
            doc.set(obj, dialogue_id(spec.dialogue));
        }
        doc.reference(obj, uid);
        doc.add(obj);
        objects.push(obj);
    }
    let dialogue = doc.content([content_name("Dialogue")]);
    doc.reference(dialogue, objects[0]);
    doc.reference(dialogue, objects[1]);
    doc.add(dialogue);
    let ambience = doc.content([content_name("Ambience")]);
    doc.reference(ambience, objects[2]);
    doc.reference(ambience, objects[3]);
    doc.add(ambience);
    let prog = doc.programme([programme_name("Main Mix")]);
    doc.reference(prog, dialogue);
    doc.reference(prog, ambience);
    doc.add(prog);
    doc.reassign_ids();
    Built { doc, uids }
}

/// A BS.2051 System B (0+5+0) bed; `lfe_low_pass` marks LFE1 with an ADM lowPass frequency, which
/// the semantic policy's DirectSpeakers `lfe` filter tests.
pub fn direct_speakers(lfe_low_pass: bool) -> Built {
    let speakers: [(&str, f32, f32); 6] = [
        ("M+030", 30.0, 0.0),
        ("M-030", -30.0, 0.0),
        ("M+000", 0.0, 0.0),
        ("LFE1", 0.0, -30.0),
        ("M+110", 110.0, 0.0),
        ("M-110", -110.0, 0.0),
    ];
    let mut doc = Document::new();
    let pf = doc.pack(Type::DirectSpeakers, [pack_name("DsPF")]);
    doc.add(pf);
    let mut uids = Vec::new();
    for (i, (label, az, el)) in speakers.into_iter().enumerate() {
        let suffix = format!("Ds{i}");
        let cf = doc.channel(Type::DirectSpeakers, [channel_name(format!("CF_{suffix}"))]);
        let mut block =
            direct_speakers_block([spherical([azimuth(az), elevation(el), distance(1.0f32)])]);
        block.add(speaker_label(label));
        if lfe_low_pass && label == "LFE1" {
            // BS.2076 carries the lowPass frequency on the audioChannelFormat.
            doc.set(cf, low_pass(120.0));
        }
        doc.block(cf, BlockKind::DirectSpeakers, block);
        doc.add(cf);
        doc.reference(pf, cf);
        uids.push(wire_channel(&mut doc, cf, pf, &suffix));
    }
    let name = if lfe_low_pass {
        "DirectSpeakersLfe"
    } else {
        "DirectSpeakers"
    };
    finish(&mut doc, &uids, name);
    Built { doc, uids }
}

/// A Cartesian object for the Triple Balance room renderer (equal-size extent, no diffuse) over
/// four 250 ms blocks.
pub fn objects_room() -> Built {
    let steps: [[f32; 4]; 4] = [
        [-0.5, 0.75, 0.0, 0.0],
        [0.25, 0.5, 0.25, 0.25],
        [0.75, -0.25, 0.5, 0.625],
        [-0.25, -0.75, 0.875, 0.125],
    ];
    let mut doc = Document::new();
    let cf = doc.channel(Type::Objects, [channel_name("ObjRoomCF")]);
    for (i, [px, py, pz, size]) in steps.into_iter().enumerate() {
        let block = objects_block([
            cartesian_position([x(px), y(py), z(pz)]),
            cartesian(true),
            rtime(250 * MS * i as i64),
            duration(250 * MS),
            width(size),
            height(size),
            depth(size),
        ]);
        doc.block(cf, BlockKind::Objects, block);
    }
    doc.add(cf);
    let pf = doc.pack(Type::Objects, [pack_name("ObjRoomPF")]);
    doc.reference(pf, cf);
    doc.add(pf);
    let uids = vec![wire_channel(&mut doc, cf, pf, "ObjRoom")];
    finish(&mut doc, &uids, "ObjectsRoom");
    Built { doc, uids }
}

/// Parameter entry points: 40 same-start Objects blocks (the last in document order is in effect)
/// and a label-free Cartesian DirectSpeakers channel, both with dB gains.
pub fn params() -> Built {
    let mut doc = Document::new();
    let objects = doc.channel(Type::Objects, [channel_name("ParamsObjCF")]);
    for i in 0..40 {
        let block = objects_block([
            spherical([
                azimuth(-97.5f32 + 5.0 * i as f32),
                elevation(if i % 2 == 0 { 0.0f32 } else { 15.0 }),
            ]),
            rtime(0),
            duration(S),
            gain_db(-6.25),
        ]);
        doc.block(objects, BlockKind::Objects, block);
    }
    doc.add(objects);
    let objects_pack = doc.pack(Type::Objects, [pack_name("ParamsObjPF")]);
    doc.reference(objects_pack, objects);
    doc.add(objects_pack);

    let direct = doc.channel(Type::DirectSpeakers, [channel_name("ParamsDsCF")]);
    let block = direct_speakers_block([
        cartesian_position([x(0.5f32), y(0.75f32), z(0.25f32)]),
        gain_db(-3.5),
    ]);
    doc.block(direct, BlockKind::DirectSpeakers, block);
    doc.add(direct);
    let direct_pack = doc.pack(Type::DirectSpeakers, [pack_name("ParamsDsPF")]);
    doc.reference(direct_pack, direct);
    doc.add(direct_pack);

    let uids = vec![
        wire_channel(&mut doc, objects, objects_pack, "ParamsObj"),
        wire_channel(&mut doc, direct, direct_pack, "ParamsDs"),
    ];
    finish(&mut doc, &uids, "Params");
    Built { doc, uids }
}

/// HOA up to `max_order` in ACN order.
pub fn hoa(max_order: i32) -> Built {
    let mut doc = Document::new();
    let pf = doc.pack(Type::Hoa, [pack_name("HoaPF")]);
    doc.add(pf);
    let mut uids = Vec::new();
    let mut i = 0;
    for o in 0..=max_order {
        for d in -o..=o {
            let suffix = format!("Hoa{i}");
            let cf = doc.channel(Type::Hoa, [channel_name(format!("CF_{suffix}"))]);
            doc.block(cf, BlockKind::Hoa, hoa_block([order(o), degree(d)]));
            doc.add(cf);
            doc.reference(pf, cf);
            uids.push(wire_channel(&mut doc, cf, pf, &suffix));
            i += 1;
        }
    }
    finish(&mut doc, &uids, &format!("Hoa{max_order}"));
    Built { doc, uids }
}

/// A fixture kind: the document, its seed (distinct per kind so a mix-up is obvious), rate and
/// frames.
pub struct Fixture {
    pub built: Built,
    pub seed: u32,
    pub rate: u32,
    pub frames: u32,
}

/// The fixture of `kind`, or `None` for an unknown kind.
pub fn fixture(kind: &str) -> Option<Fixture> {
    let (built, seed) = match kind {
        "objects-point" => (objects(false, 1), 0x1111_1111),
        "objects-extent" => (objects(true, 1), 0x2222_2222),
        "objects-extent-multi" => (objects(true, 3), 0x5555_5555),
        "objects-cartesian" => (objects_cartesian(), 0x6666_6666),
        "directspeakers" => (direct_speakers(false), 0x3333_3333),
        "hoa" => (hoa(1), 0x4444_4444),
        "objects-room" | "objects-room-96k" => (objects_room(), 0x8888_8888),
        "params" => (params(), 0x9999_9999),
        "hoa3" => (hoa(3), 0x7777_7777),
        "policy-objects" => (policy_objects(), 0xAAAA_AAAA),
        "directspeakers-lfe" => (direct_speakers(true), 0xBBBB_BBBB),
        _ => return None,
    };
    // The 96 kHz kind keeps the one-second duration, so its block times are those of objects-room.
    let (rate, frames) = if kind == "objects-room-96k" {
        (HIGH_RATE, HIGH_RATE)
    } else {
        (SAMPLE_RATE, FRAMES)
    };
    Some(Fixture {
        built,
        seed,
        rate,
        frames,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn signal_is_exact() {
        let mut state = 0;
        assert_eq!(lcg_next(&mut state), 1013904223);
        assert_eq!(exact_sample(0), -1.0);
        assert_eq!(exact_sample(u32::MAX), 8_388_607.0 / 8_388_608.0);
    }

    #[test]
    fn every_kind_builds() {
        for kind in KINDS {
            let f = fixture(kind).unwrap();
            assert!(!f.built.uids.is_empty(), "{kind}");
            assert!(f.built.doc.xml().starts_with("<?xml"));
        }
        assert!(fixture("bogus").is_none());
        assert_eq!(fixture("hoa3").unwrap().built.uids.len(), 16);
    }
}
