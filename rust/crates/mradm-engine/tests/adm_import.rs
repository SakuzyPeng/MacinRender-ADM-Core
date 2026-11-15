//! ADM import semantics on authored fixtures (formerly the C++ `adm_io_fixture_test`, R3 C3),
//! read through the scene the backends receive: its inspect JSON (`scene_json`, the view the C++
//! test read as `AdmScene`) and, for the authored provenance, the snapshot. Covers the minimal
//! graph and CHNA mapping, object and block provenance, block positions, gains and defaults,
//! fractional block boundaries, channelLock / divergence, DirectSpeakers blocks, head-lock
//! precedence for every type, HOA pack attributes, mixed objects, track-scoped pack channels
//! with upper-case CHNA UIDs, the Binaural warning, the LFE low-pass, programme and content loudness,
//! languages, labels, dialogue, the reference screen, programme times, importance and dialogue IDs.
mod common;
use common::render::*;
use mradm_engine::scene_json;
use mradm_json::Value;
use mradm_scene::RenderScene;
use mradm_testkit::adm::*;
use mradm_testkit::fixtures::wire_channel;

fn import(axml: &str, uids: &[&str], frames: usize) -> (RenderScene, Value) {
    let dir = scratch("import");
    let path = dir.file("in.wav");
    input(&path, axml, uids, 48000, frames, 0.);
    let scene = mradm_scene::import(path.to_str().unwrap().as_bytes(), Default::default())
        .unwrap_or_else(|e| panic!("{}", e.message));
    let json = mradm_json::parse(scene_json::inspect_json(&scene, "<input>").as_bytes()).unwrap();
    (scene, json)
}

fn single(single: Single) -> (RenderScene, Value) {
    let (axml, uid) = single.built();
    import(&axml, &[&uid], 1)
}

/// `value[key]` through a path of keys and indices ("objects.0.tracks.0").
fn at<'a>(value: &'a Value, path: &str) -> &'a Value {
    path.split('.')
        .fold(value, |v, key| match key.parse::<usize>() {
            Ok(i) => &v.as_array().unwrap()[i],
            Err(_) => v
                .as_object()
                .unwrap()
                .get(key)
                .unwrap_or_else(|| panic!("no {key} in {path}")),
        })
}

fn num(value: &Value, path: &str) -> f64 {
    at(value, path).as_f64().unwrap()
}

fn text<'a>(value: &'a Value, path: &str) -> &'a str {
    at(value, path).as_str().unwrap()
}

fn len(value: &Value, path: &str) -> usize {
    at(value, path).as_array().unwrap().len()
}

fn yes(value: &Value, path: &str) -> bool {
    matches!(at(value, path), Value::Bool(true))
}

/// Whether `value` has `key` (an unset optional is omitted).
fn has(value: &Value, key: &str) -> bool {
    value.as_object().unwrap().get(key).is_some()
}

fn near(a: f64, b: f64) -> bool {
    (a - b).abs() < 0.01
}

#[test]
fn the_minimal_graph_maps_its_chna_track() {
    let s = Single::objects(vec![polar(0., 0., [])]);
    let (axml, uid) = s.built();
    let (_, j) = import(&axml, &[&uid], 0);
    assert_eq!(
        (
            num(&j, "file.sample_rate"),
            num(&j, "file.num_channels"),
            num(&j, "file.num_frames")
        ),
        (48000., 1., 0.)
    );
    assert_eq!(
        (
            len(&j, "programmes"),
            len(&j, "contents"),
            len(&j, "objects")
        ),
        (1, 1, 1)
    );
    assert_eq!(text(&j, "programmes.0.name"), "TestProgramme");
    assert_eq!(len(&j, "programmes.0.content_ids"), 1);
    assert_eq!(text(&j, "contents.0.name"), "TestContent");
    assert_eq!(len(&j, "contents.0.object_ids"), 1);
    assert_eq!(text(&j, "objects.0.name"), "TestObject");
    assert_eq!(len(&j, "objects.0.tracks"), 1);
    assert_eq!(text(&j, "objects.0.tracks.0.track_uid"), uid);
    assert_eq!(num(&j, "objects.0.tracks.0.channel_index"), 0.);
}

#[test]
fn object_and_block_provenance_survive_canonical_timing() {
    let mut s = Single::objects(vec![polar(
        30.,
        10.,
        [
            gain_db(-12.041199826559248),
            rtime(10 * MS),
            duration(20 * MS),
        ],
    )]);
    s.doc.set(s.object, gain_db(-6.020599913279624));
    s.doc.set(s.object, mute(true));
    s.doc.set(s.object, start(1_000 * MS));
    s.doc.set(s.object, duration(2_000 * MS));
    let (scene, _) = single(s);
    let snap = &scene.snapshot;
    let object = &snap.objects[0];
    assert!(object.source_gain.present != 0 && object.source_gain.decibels != 0);
    assert!((object.gain - 0.5).abs() < 1e-6, "dB normalised once");
    assert!(object.mute_present != 0 && object.mute != 0);
    assert_eq!((object.start, object.end), (48000, 144000));
    let block = &snap.object_blocks[snap.tracks[object.tracks.offset].blocks.offset];
    assert_eq!((block.start, block.end), (48480, 49440));
    assert_eq!(
        (block.source.rtime_samples, block.source.duration_samples),
        (480, 960)
    );
    assert!(block.source.gain.decibels != 0 && (block.gain - 0.25).abs() < 1e-6);
}

#[test]
fn object_blocks_keep_position_gain_and_defaults() {
    let (_, j) = single(Single::objects(vec![polar(30., 10., [gain_linear(0.8)])]));
    let b = at(&j, "objects.0.tracks.0.object_blocks.0");
    assert!(!yes(b, "position.cartesian"));
    assert!(near(num(b, "position.azimuth"), 30.) && near(num(b, "position.elevation"), 10.));
    assert!(near(num(b, "position.distance"), 1.) && near(num(b, "gain"), 0.8));
    assert!(!yes(b, "channel_lock"));
    assert!(!has(b, "channel_lock_max_distance"));
    assert!(near(num(b, "divergence"), 0.) && near(num(b, "divergence_azimuth_range"), 45.));
    assert!(near(num(b, "divergence_position_range"), 0.));
}

#[test]
fn fractional_block_boundaries_are_contiguous() {
    let block = |az, start_ns: i64, duration_ns: i64| {
        objects_block([
            spherical([azimuth(az), elevation(0.0f32)]),
            rtime(start_ns),
            duration(duration_ns),
        ])
    };
    let s = Single::objects(vec![
        block(-30.0f32, 0, 5_408_630_000),
        block(30.0f32, 5_408_630_000, 1_040_000),
        block(-30.0f32, 5_409_670_000, 1_040_000),
    ]);
    let (scene, _) = single(s);
    let b = &scene.snapshot.object_blocks;
    assert_eq!(b.len(), 3);
    assert_eq!((b[0].end, b[1].end), (b[1].start, b[2].start));
    assert_eq!(
        (b[1].start, b[1].end),
        (259_614, 259_664),
        "rounded to the nearest sample"
    );
}

#[test]
fn channel_lock_and_divergence_import_with_their_defaults() {
    let lock = |explicit: bool| {
        let mut l = vec![Param::text(true)];
        let mut d = vec![Param::text(0.5f32)];
        if explicit {
            l.push(max_distance(1.0f32));
            d.push(azimuth_range(60.0f32));
            d.push(position_range(0.25f32));
        }
        single(Single::objects(vec![polar(
            20.,
            0.,
            [channel_lock(l), object_divergence(d)],
        )]))
        .1
    };
    let j = lock(true);
    let b = at(&j, "objects.0.tracks.0.object_blocks.0");
    assert!(yes(b, "channel_lock") && near(num(b, "channel_lock_max_distance"), 1.));
    assert!(near(num(b, "divergence"), 0.5) && near(num(b, "divergence_azimuth_range"), 60.));
    assert!(near(num(b, "divergence_position_range"), 0.25));
    let j = lock(false);
    let b = at(&j, "objects.0.tracks.0.object_blocks.0");
    assert!(yes(b, "channel_lock") && !has(b, "channel_lock_max_distance"));
    assert!(near(num(b, "divergence"), 0.5) && near(num(b, "divergence_azimuth_range"), 45.));
    assert!(near(num(b, "divergence_position_range"), 0.));
}

#[test]
fn direct_speakers_blocks_keep_labels_positions_and_gains() {
    let mut block = speaker_block("M+030", 30., 0.);
    block.set(gain_linear(0.7));
    let (_, j) = single(Single::new(Type::DirectSpeakers, vec![block]));
    assert_eq!(len(&j, "objects.0.tracks.0.object_blocks"), 0);
    let b = at(&j, "objects.0.tracks.0.ds_blocks.0");
    assert_eq!(text(b, "speaker_labels.0"), "M+030");
    assert!(!text(b, "pack_format_id").is_empty() && yes(b, "has_position"));
    assert!(
        near(num(b, "azimuth"), 30.)
            && near(num(b, "elevation"), 0.)
            && near(num(b, "distance"), 1.)
    );
    assert!(near(num(b, "gain"), 0.7));
}

#[test]
fn head_lock_precedence_holds_for_every_type() {
    let cases = [
        (None, None, false),
        (Some(true), None, true),
        (Some(true), Some(false), false),
        (Some(false), Some(true), true),
    ];
    for ty in [Type::Objects, Type::DirectSpeakers, Type::Hoa] {
        for (object_value, block_value, expected) in cases {
            let mut block = match ty {
                Type::Objects => polar(0., 0., []),
                Type::DirectSpeakers => speaker_block("M+000", 0., 0.),
                _ => hoa_block([order(0), degree(0)]),
            };
            if let Some(v) = block_value {
                block.set(head_locked(v));
            }
            let mut s = Single::new(ty, vec![block]);
            if let Some(v) = object_value {
                s.doc.set(s.object, head_locked(v));
            }
            let (_, j) = single(s);
            let what = format!("{ty:?} object={object_value:?} block={block_value:?}");
            assert_eq!(
                yes(&j, "objects.0.head_locked"),
                object_value.unwrap_or(false),
                "{what}"
            );
            let effective = match ty {
                Type::Objects => yes(&j, "objects.0.tracks.0.object_blocks.0.head_locked"),
                Type::DirectSpeakers => yes(&j, "objects.0.tracks.0.ds_blocks.0.head_locked"),
                _ => {
                    assert_eq!(
                        yes(&j, "hoa_tracks.0.head_locked"),
                        object_value.unwrap_or(false),
                        "{what}"
                    );
                    yes(&j, "hoa_tracks.0.channels.0.blocks.0.head_locked")
                }
            };
            assert_eq!(effective, expected, "{what}");
        }
    }
}

#[test]
fn hoa_pack_attributes_import() {
    let mut s = Single::new(Type::Hoa, vec![hoa_block([order(0), degree(0)])]);
    s.doc.set(s.pack, Param::attribute("normalization", "N3D"));
    s.doc.set(s.pack, Param::attribute("nfcRefDist", 2.0f32));
    s.doc.set(s.pack, Param::attribute("screenRef", true));
    let (_, j) = single(s);
    assert_eq!(len(&j, "hoa_tracks"), 1);
    assert_eq!(text(&j, "hoa_tracks.0.normalization"), "N3D");
    assert_eq!(num(&j, "hoa_tracks.0.nfc_ref_dist"), 2.);
    assert!(yes(&j, "hoa_tracks.0.screen_ref"));
}

#[test]
fn mixed_objects_keep_their_own_block_kinds() {
    let (axml, uids) = tracks(
        vec![
            (Type::Objects, vec![polar(0., 0., [])]),
            (Type::DirectSpeakers, vec![speaker_block("M+030", 30., 0.)]),
        ],
        false,
    );
    let (_, j) = import(&axml, &[&uids[0], &uids[1]], 1);
    assert_eq!(len(&j, "objects"), 2);
    assert_eq!(
        (
            len(&j, "objects.0.tracks.0.object_blocks"),
            len(&j, "objects.0.tracks.0.ds_blocks")
        ),
        (1, 0)
    );
    assert_eq!(
        (
            len(&j, "objects.1.tracks.0.object_blocks"),
            len(&j, "objects.1.tracks.0.ds_blocks")
        ),
        (0, 1)
    );
    assert_eq!(
        text(&j, "objects.1.tracks.0.ds_blocks.0.speaker_labels.0"),
        "M+030"
    );
}

#[test]
fn pack_channels_are_scoped_to_their_track_and_chna_uids_match_any_case() {
    let mut doc = Document::new();
    let pack = doc.pack(Type::DirectSpeakers, [pack_name("BedPF")]);
    let mut uids = Vec::new();
    for (label, az) in [("M+030", 30f32), ("M-030", -30.)] {
        let channel = doc.channel(
            Type::DirectSpeakers,
            [channel_name(format!("Bed{label}CF"))],
        );
        doc.block(
            channel,
            BlockKind::DirectSpeakers,
            speaker_block(label, az, 0.),
        );
        doc.add(channel);
        doc.reference(pack, channel);
        uids.push(wire_channel(&mut doc, channel, pack, label));
    }
    doc.add(pack);
    let object = doc.object([object_name("BedObject")]);
    for &u in &uids {
        doc.reference(object, u);
    }
    doc.add(object);
    let content = doc.content([content_name("BedContent")]);
    doc.reference(content, object);
    doc.add(content);
    let programme = doc.programme([programme_name("BedProgramme")]);
    doc.reference(programme, content);
    doc.add(programme);
    doc.reassign_ids();
    // Lower-case hex in the AXML, upper-case in the CHNA.
    let (a, b) = (doc.id(uids[0]).to_owned(), doc.id(uids[1]).to_owned());
    let xml = doc
        .xml()
        .replace(&a, "ATU_0000000a")
        .replace(&b, "ATU_0000000b");
    let (_, j) = import(&xml, &["ATU_0000000A", "ATU_0000000B"], 1);
    assert_eq!((len(&j, "objects"), len(&j, "objects.0.tracks")), (1, 2));
    for (track, label) in [(0, "M+030"), (1, "M-030")] {
        let labels = at(
            &j,
            &format!("objects.0.tracks.{track}.ds_blocks.0.speaker_labels"),
        );
        assert_eq!(labels.as_array().unwrap().len(), 1);
        assert_eq!(labels.as_array().unwrap()[0].as_str().unwrap(), label);
    }
}

#[test]
fn binaural_channels_are_skipped_with_a_warning() {
    let (_, j) = single(Single::new(Type::Binaural, vec![]));
    let warnings = at(&j, "import_warnings").as_array().unwrap();
    assert!(
        warnings
            .iter()
            .any(|w| w.as_str().unwrap().contains("Binaural")),
        "{warnings:?}"
    );
}

#[test]
fn the_lfe_low_pass_imports_without_warnings() {
    let mut s = Single::new(Type::DirectSpeakers, vec![speaker_block("LFE1", 45., -30.)]);
    s.doc.set(s.channel, low_pass(120.0f32));
    let (_, j) = single(s);
    assert_eq!(len(&j, "import_warnings"), 0);
    assert!((num(&j, "objects.0.tracks.0.ds_blocks.0.low_pass_hz") - 120.).abs() < 0.5);
}

/// The single-object fixture with `edit` applied to its programme and content.
fn described(edit: impl FnOnce(&mut Document, Id, Id, Id)) -> Value {
    let mut s = Single::bare(Type::Objects, vec![polar(0., 0., [])]);
    let content = s.doc.content([content_name("TestContent")]);
    s.doc.reference(content, s.object);
    let programme = s.doc.programme([programme_name("TestProgramme")]);
    edit(&mut s.doc, programme, content, s.object);
    s.doc.add(content);
    s.doc.reference(programme, content);
    s.doc.add(programme);
    single(s).1
}

#[test]
fn programme_and_content_metadata_import() {
    let j = described(|doc, programme, _, _| {
        doc.set(
            programme,
            list([loudness_metadata([
                loudness_method("ITU-R BS.1770-4"),
                integrated_loudness(-23.0f32),
                max_true_peak(-1.0f32),
                loudness_range(6.5f32),
                max_momentary(-18.5f32),
                max_short_term(-20.0f32),
                dialogue_loudness(-24.0f32),
            ])]),
        );
    });
    let l = at(&j, "programmes.0.loudness");
    for (key, value) in [
        ("integrated_loudness", -23.),
        ("max_true_peak", -1.),
        ("loudness_range", 6.5),
        ("max_momentary", -18.5),
        ("max_short_term", -20.),
        ("dialogue_loudness", -24.),
    ] {
        assert!(near(num(l, key), value), "{key}");
    }
    assert_eq!(text(l, "loudness_method"), "ITU-R BS.1770-4");

    let j = described(|doc, _, content, _| {
        doc.set(content, content_language("en"));
        doc.add_param(
            content,
            label([Param::text("English Commentary"), label_language("en")]),
        );
        doc.set(
            content,
            list([loudness_metadata([
                integrated_loudness(-24.0f32),
                max_true_peak(-2.0f32),
            ])]),
        );
        // DialogueContent::voiceover.
        doc.set(
            content,
            Param::element("dialogue", 1).with_attr("dialogueContentKind", "2"),
        );
    });
    assert_eq!(text(&j, "contents.0.language"), "en");
    assert_eq!(text(&j, "contents.0.labels.0"), "English Commentary");
    assert_eq!(text(&j, "contents.0.dialogue_kind"), "dialogue");
    assert_eq!(text(&j, "contents.0.content_kind"), "voiceover");
    assert!(near(
        num(&j, "contents.0.loudness.integrated_loudness"),
        -24.
    ));
    assert!(near(num(&j, "contents.0.loudness.max_true_peak"), -2.));

    let j = described(|doc, programme, _, _| {
        doc.set(programme, programme_language("fr"));
        doc.add_param(
            programme,
            label([Param::text("French Main Mix"), label_language("fr")]),
        );
        doc.set(programme, start(1_000 * MS));
        doc.set(programme, end(5_000 * MS));
    });
    assert_eq!(text(&j, "programmes.0.language"), "fr");
    assert_eq!(text(&j, "programmes.0.labels.0"), "French Main Mix");
    assert_eq!(
        (
            num(&j, "programmes.0.start_sample"),
            num(&j, "programmes.0.end_sample")
        ),
        (48000., 240000.)
    );

    let j = described(|doc, _, _, object| {
        doc.add_param(
            object,
            label([Param::text("Main Dialogue"), label_language("en")]),
        );
        doc.set(object, importance(7));
        doc.set(object, dialogue_id(1));
    });
    assert_eq!(text(&j, "objects.0.labels.0"), "Main Dialogue");
    assert_eq!(
        (
            num(&j, "objects.0.importance"),
            num(&j, "objects.0.dialogue_id")
        ),
        (7., 1.)
    );
}

#[test]
fn the_reference_screen_flag_follows_the_programme() {
    let (axml, uid) = Single::objects(vec![polar(0., 0., [])]).built();
    let (_, j) = import(&axml, &[&uid], 1);
    assert!(!yes(&j, "programmes.0.has_reference_screen"));
    let tag = axml.find("<audioProgramme ").unwrap();
    let close = tag + axml[tag..].find('>').unwrap() + 1;
    let screened = format!(
        "{}<audioProgrammeReferenceScreen />{}",
        &axml[..close],
        &axml[close..]
    );
    let (_, j) = import(&screened, &[&uid], 1);
    assert!(yes(&j, "programmes.0.has_reference_screen"));
}
