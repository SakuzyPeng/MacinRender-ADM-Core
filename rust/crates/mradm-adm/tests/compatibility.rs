use mradm_adm::{Document, Patch, Snapshot};

// Fixed inputs also exercised by the optional libadm reference comparison.
const OBJECTS: &str = include_str!("../../../../tests/fixtures/adm/objects-stream-only.xml");
const HOA: &str = include_str!("../../../../tests/fixtures/adm/hoa-pack-attributes.xml");
const TYPELESS: &str = include_str!("../../../../tests/fixtures/adm/types-from-ids.xml");

fn snapshot(xml: &str) -> Snapshot {
    let channels = (0..4)
        .map(|i| (format!("ATU_{:08x}", i + 1), i))
        .collect::<Vec<_>>();
    Snapshot::extract(&Document::parse(xml).unwrap(), 48000, &channels).unwrap()
}

fn check_hoa(s: &Snapshot, normalization: &str, nfc_ref_dist: f64, screen_ref: u32) {
    assert_eq!(s.hoa.len(), 1);
    let pack = &s.hoa[0];
    assert_eq!(s.strings[pack.normalization as usize], normalization);
    assert_eq!(pack.nfc_ref_dist, nfc_ref_dist);
    assert_eq!(pack.screen_ref, screen_ref);
    assert_eq!(pack.channels.len, 4);
}

#[test]
fn hoa_pack_attributes_are_preserved() {
    check_hoa(&snapshot(HOA), "N3D", 2.0, 1);
}

#[test]
fn hoa_block_overrides_preserve_other_pack_attributes() {
    for (block, normalization, distance, screen_ref) in [
        ("<normalization>SN3D</normalization>", "SN3D", 2.0, 1),
        ("<nfcRefDist>0</nfcRefDist>", "N3D", 0.0, 1),
        ("<screenRef>0</screenRef>", "N3D", 2.0, 0),
        (
            "<normalization>SN3D</normalization><nfcRefDist>0</nfcRefDist><screenRef>0</screenRef>",
            "SN3D",
            0.0,
            0,
        ),
    ] {
        let xml = HOA.replace("<order>0</order>", &format!("<order>0</order>{block}"));
        check_hoa(&snapshot(&xml), normalization, distance, screen_ref);
    }
}

#[test]
fn stream_only_references_keep_objects_on_their_own_tracks() {
    let s = snapshot(OBJECTS);
    assert_eq!(s.tracks.len(), 2);
    assert_eq!(s.object_blocks.len(), 2);
    for (i, azimuth) in [30.0, -30.0].into_iter().enumerate() {
        let track = &s.tracks[i];
        assert_eq!(track.channel, i as i32);
        assert_eq!(track.blocks.len, 1);
        assert_eq!(
            s.object_blocks[track.blocks.offset].position.azimuth,
            azimuth
        );
    }
}

#[test]
fn stream_only_references_retain_all_hoa_channels() {
    let mut xml = HOA.to_owned();
    for suffix in 0x1001..=0x1004 {
        xml = xml.replace(
            &format!("<audioStreamFormatIDRef>AS_0004{suffix:04x}</audioStreamFormatIDRef>"),
            "",
        );
    }
    let s = snapshot(&xml);
    check_hoa(&s, "N3D", 2.0, 1);
    assert_eq!(s.hoa_channels.len(), 4);
    for (i, (order, degree)) in [(0, 0), (1, -1), (1, 0), (1, 1)].into_iter().enumerate() {
        let channel = &s.hoa_channels[i];
        assert_eq!(channel.channel, i as i32);
        assert_eq!((channel.order, channel.degree), (order, degree));
        assert_eq!(channel.blocks.len, 1);
    }
}

#[test]
fn stream_only_references_survive_independent_track_edits() {
    let doc = Document::parse(OBJECTS).unwrap();
    assert_eq!(doc.apply_patches(&[], 48000).unwrap(), OBJECTS);
    let s = snapshot(OBJECTS);
    let patch = Patch {
        node: s.object_blocks[s.tracks[0].blocks.offset].node,
        field: 1,
        present: 1,
        original_present: 1,
        value: 0.5,
        original: 1.0,
        ..Patch::default()
    };
    let xml = doc.apply_patches(&[patch], 48000).unwrap();
    assert!(!xml.contains("<audioStreamFormatIDRef>"));
    let actual = snapshot(&xml);
    assert_eq!(actual.object_blocks.len(), 2);
    assert_eq!(actual.object_blocks[0].gain, 0.5);
    assert_eq!(actual.object_blocks[1].gain, 1.0);
}

#[test]
fn conflicting_track_stream_links_are_rejected() {
    // Reject both competing reverse declarations and disagreement between a
    // forward and a reverse declaration, without relying on hash-map order.
    for input in [OBJECTS, TYPELESS] {
        let xml = input.replace(
            "<audioTrackFormatIDRef>AT_00031002_01</audioTrackFormatIDRef>",
            "<audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef>",
        );
        assert!(Document::parse(&xml).is_err());
    }
}

#[test]
fn omitted_type_attributes_are_inferred_from_ids() {
    let s = snapshot(TYPELESS);
    assert_eq!(s.tracks.len(), 2);
    assert_eq!(s.object_blocks.len(), 2);
    assert_eq!(s.object_blocks[0].position.azimuth, 30.0);
    assert_eq!(s.object_blocks[1].position.azimuth, -30.0);
}

#[test]
fn explicit_type_attributes_must_match_ids() {
    let valid = TYPELESS
        .replace("<audioPackFormat ", "<audioPackFormat typeLabel='0003' ")
        .replace(
            "<audioChannelFormat ",
            "<audioChannelFormat typeLabel='0003' ",
        );
    for element in ["audioPackFormat", "audioChannelFormat"] {
        for attribute in [
            "typeLabel='0001'",
            "typeDefinition='DirectSpeakers'",
            "typeLabel='0001' typeDefinition='DirectSpeakers'",
        ] {
            let xml = valid.replace(
                &format!("<{element} typeLabel='0003' "),
                &format!("<{element} {attribute} "),
            );
            assert!(Document::parse(&xml).is_err(), "{element}: {attribute}");
        }
    }
}
