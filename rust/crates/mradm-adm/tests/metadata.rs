use mradm_adm::{Document, Patch, Snapshot, Speaker, generate};
fn document(inner: &str) -> String {
    format!(
        "<?xml version=\"1.0\"?><ebuCoreMain><coreMetadata><format><audioFormatExtended>{inner}</audioFormatExtended></format></coreMetadata></ebuCoreMain>"
    )
}
fn objects(extra: &str, block: &str) -> String {
    document(&format!(
        r#"
<audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="A &amp; B"><audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme>
<audioContent audioContentID="ACO_1001" audioContentName="Content"><audioObjectIDRef>AO_1001</audioObjectIDRef></audioContent>
<audioObject audioObjectID="AO_1001" audioObjectName="Object" start="00:00:01.0"><audioTrackUIDRef>ATU_0000000a</audioTrackUIDRef>{extra}</audioObject>
<audioTrackUID UID="ATU_0000000a"><audioTrackFormatIDRef>AT_00031001_01</audioTrackFormatIDRef><audioPackFormatIDRef>AP_00031001</audioPackFormatIDRef></audioTrackUID>
<audioTrackFormat audioTrackFormatID="AT_00031001_01" audioTrackFormatName="Track"><audioStreamFormatIDRef>AS_00031001</audioStreamFormatIDRef></audioTrackFormat>
<audioStreamFormat audioStreamFormatID="AS_00031001" audioStreamFormatName="Stream"><audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef></audioStreamFormat>
<audioPackFormat audioPackFormatID="AP_00031001" audioPackFormatName="Pack" typeDefinition="Objects"><audioChannelFormatIDRef>AC_00031001</audioChannelFormatIDRef></audioPackFormat>
<audioChannelFormat audioChannelFormatID="AC_00031001" audioChannelFormatName="Channel" typeDefinition="Objects"><audioBlockFormat rtime="00:00:00.1S48000" duration="00:00:00.01"><position coordinate="azimuth">30</position><position coordinate="elevation">10</position>{block}</audioBlockFormat></audioChannelFormat>"#
    ))
}
fn snapshot(xml: &str) -> Snapshot {
    Snapshot::extract(
        &Document::parse(xml).unwrap(),
        48000,
        &[("ATU_0000000A \0".to_owned(), 2)],
    )
    .unwrap()
}
#[test]
fn imported_values_presence_entities_and_sample_rounding() {
    let x = objects(
        "<headLocked>1</headLocked><gain gainUnit=\"dB\">-6.020599913279624</gain>",
        "<headLocked>0</headLocked><gain>0.8</gain>",
    );
    let s = snapshot(&x);
    assert_eq!(s.strings[s.programmes[0].name as usize], "A & B");
    assert_eq!(s.tracks[0].channel, 2);
    assert_eq!(s.objects[0].source_gain.decibels, 1);
    assert_eq!(s.objects[0].gain, 0.5);
    assert_eq!(s.object_blocks[0].start, 48001);
    assert_eq!(s.object_blocks[0].end, 48481);
    assert_eq!(s.object_blocks[0].head_locked, 0);
    assert_eq!(s.object_blocks[0].source.rtime_present, 1);
    assert_eq!(s.object_blocks[0].azimuth_range, 45.0);
    assert_eq!(
        snapshot(&objects("<headLocked>1</headLocked>", "")).object_blocks[0].head_locked,
        1
    );
}
#[test]
fn common_definitions_are_available_without_runtime_files() {
    let xml = document(
        r#"<audioObject audioObjectID="AO_1001" audioObjectName="Bed"><audioTrackUIDRef>ATU_00000001</audioTrackUIDRef></audioObject><audioTrackUID UID="ATU_00000001"><audioPackFormatIDRef>AP_00010002</audioPackFormatIDRef><audioTrackFormatIDRef>AT_00010001_01</audioTrackFormatIDRef></audioTrackUID>"#,
    );
    let s = snapshot(&xml);
    assert_eq!(s.direct_blocks.len(), 1);
    assert_eq!(s.direct_blocks[0].azimuth, 30.0);
    assert_eq!(
        s.strings[s.indices[s.direct_blocks[0].labels.offset] as usize],
        "urn:itu:bs:2051:0:speaker:M+030"
    );
    let patch = Patch {
        node: s.direct_blocks[0].node,
        field: 1,
        present: 1,
        original_present: 1,
        value: 0.5,
        original: 1.0,
        ..Patch::default()
    };
    assert_eq!(
        Document::parse(&xml)
            .unwrap()
            .apply_patches(&[patch], 48000)
            .unwrap_err()
            .code,
        2
    );
}
#[test]
fn lossless_patch_preserves_unknown_xml_and_unchanged_bytes() {
    let xml = objects(
        "<x:private xmlns:x='urn:test' keep='yes'>opaque &amp; intact</x:private>",
        "<gain gainUnit='dB' vendor='kept'>-6.020599913279624</gain><zoneExclusion><zone name='screen'/></zoneExclusion>",
    );
    let d = Document::parse(&xml).unwrap();
    let s = snapshot(&xml);
    assert_eq!(d.apply_patches(&[], 48000).unwrap(), xml);
    let patch = Patch {
        node: s.object_blocks[0].node,
        field: 1,
        present: 1,
        original_present: 1,
        value: 0.25,
        original: 0.5,
        ..Patch::default()
    };
    let updated = d.apply_patches(&[patch], 48000).unwrap();
    assert_eq!(
        updated,
        xml.replace(
            "gainUnit='dB' vendor='kept'>-6.020599913279624",
            "gainUnit='linear' vendor='kept'>0.25"
        )
    );
    assert_eq!(snapshot(&updated).object_blocks[0].gain, 0.25);
}
#[test]
fn inserts_explicit_false_and_retains_nested_unknowns() {
    let xml = objects("<headLocked>1</headLocked>", "<unknown foo='x'/>");
    let s = snapshot(&xml);
    let patch = Patch {
        node: s.object_blocks[0].node,
        field: 3,
        present: 1,
        original_present: 1,
        value: 0.0,
        original: 1.0,
        ..Patch::default()
    };
    let updated = Document::parse(&xml)
        .unwrap()
        .apply_patches(&[patch], 48000)
        .unwrap();
    assert!(updated.contains("<unknown foo='x'/><headLocked>0</headLocked>"));
    assert_eq!(snapshot(&updated).object_blocks[0].head_locked, 0);
}
#[test]
fn conflicting_shared_node_edits_are_rejected() {
    let xml = objects("", "");
    let s = snapshot(&xml);
    let a = Patch {
        node: s.object_blocks[0].node,
        field: 1,
        present: 1,
        original_present: 1,
        value: 0.5,
        original: 1.0,
        ..Patch::default()
    };
    let b = Patch { value: 1.0, ..a };
    assert_eq!(
        Document::parse(&xml)
            .unwrap()
            .apply_patches(&[a, b], 48000)
            .unwrap_err()
            .code,
        2
    );
}
#[test]
fn malformed_xml_ids_references_and_numeric_values_are_errors() {
    let valid = objects("", "");
    for bad in [
        valid.replace("</audioObject>", "</broken>"),
        valid.replace(
            "AT_00031001_01</audioTrackFormatIDRef>",
            "AT_00031002_01</audioTrackFormatIDRef>",
        ),
        valid.replace(
            "<position coordinate=\"azimuth\">30",
            "<position coordinate=\"azimuth\">NaN",
        ),
        valid.replace("00:00:00.1S48000", "00:00:00.1S0"),
        valid.replace("<audioObject ", "<audioObject audioObjectID=\"AO_1001\" "),
    ] {
        let result = Document::parse(&bad).and_then(|d| Snapshot::extract(&d, 48000, &[]));
        assert!(result.is_err());
    }
    assert!(Document::parse("<!DOCTYPE root SYSTEM 'secret'><root/>").is_err());
    assert!(Document::parse(&document("<audioObject audioObjectID='AO_1001' audioObjectName='cycle'><audioObjectIDRef>AO_1001</audioObjectIDRef></audioObject>")).is_err());
}
#[test]
fn generated_layouts_have_matching_chna_and_semantics() {
    let speakers = [Speaker {
        label: "M+030 & <left>".into(),
        azimuth: 30.0,
        elevation: 0.0,
        lfe: false,
        azimuth_range: Some((22.0, 30.0)),
        elevation_range: None,
    }];
    let output = generate(1, "Test & layout", &speakers).unwrap();
    let d = Document::parse(&output.axml).unwrap();
    let s = Snapshot::extract(
        &d,
        48000,
        &output
            .chna
            .iter()
            .map(|c| (c.uid.clone(), c.track - 1))
            .collect::<Vec<_>>(),
    )
    .unwrap();
    assert_eq!(s.tracks[0].channel, 0);
    assert_eq!(s.direct_blocks[0].azimuth, 30.0);
    assert_eq!(s.direct_blocks[0].azimuth_min, 22.0);
    let hoa = generate(4, "HOA3 ACN SN3D", &[]).unwrap();
    let s = snapshot(&hoa.axml);
    assert_eq!(hoa.chna.len(), 16);
    assert_eq!(s.hoa_channels.len(), 16);
    assert_eq!(s.hoa_channels[15].order, 3);
    assert_eq!(s.hoa_channels[15].degree, 3);
    let binaural = generate(5, "Binaural", &[]).unwrap();
    assert_eq!(binaural.chna.len(), 2);
    Document::parse(&binaural.axml).unwrap();
}

#[test]
fn self_closing_object_can_receive_multiple_edits_without_losing_attributes() {
    let xml =
        document("<audioObject audioObjectID='AO_1001' audioObjectName='Empty' vendor='kept'/>");
    let d = Document::parse(&xml).unwrap();
    let s = snapshot(&xml);
    let patches = [
        Patch {
            node: s.objects[0].node,
            field: 1,
            present: 1,
            original_present: 1,
            value: 0.5,
            original: 1.0,
            ..Patch::default()
        },
        Patch {
            node: s.objects[0].node,
            field: 2,
            present: 1,
            original_present: 1,
            value: 1.0,
            original: 0.0,
            ..Patch::default()
        },
    ];
    let changed = d.apply_patches(&patches, 48000).unwrap();
    assert!(changed.contains("vendor='kept'>"));
    let actual = snapshot(&changed);
    assert_eq!(actual.objects[0].gain, 0.5);
    assert_eq!(actual.objects[0].mute, 1);
}
#[test]
fn optional_interpolation_attribute_is_removed_without_changing_other_attributes() {
    let xml = objects(
        "",
        "<jumpPosition vendor='retained' interpolationLength='0.005'>1</jumpPosition>",
    );
    let s = snapshot(&xml);
    let p = Patch {
        node: s.object_blocks[0].node,
        field: 14,
        present: 0,
        original_present: 1,
        original_samples: 240,
        ..Patch::default()
    };
    let changed = Document::parse(&xml)
        .unwrap()
        .apply_patches(&[p], 48000)
        .unwrap();
    assert_eq!(changed, xml.replace("interpolationLength='0.005'", ""));
    assert_eq!(snapshot(&changed).object_blocks[0].interpolation_present, 0);
}

#[test]
fn namespace_extensions_do_not_override_adm_fields() {
    let xml = objects(
        "",
        "<x:gain xmlns:x='urn:vendor'>99</x:gain><headLocked>0</headLocked>",
    )
    .replace(
        "<ebuCoreMain>",
        "<ebuCoreMain xmlns='urn:ebu:metadata-schema:ebuCore_2017'>",
    );
    let s = snapshot(&xml);
    assert_eq!(s.object_blocks[0].gain, 1.0);
    let p = Patch {
        node: s.object_blocks[0].node,
        field: 1,
        present: 1,
        original_present: 1,
        value: 0.25,
        original: 1.0,
        ..Patch::default()
    };
    let changed = Document::parse(&xml)
        .unwrap()
        .apply_patches(&[p], 48000)
        .unwrap();
    assert!(changed.contains("<x:gain xmlns:x='urn:vendor'>99</x:gain>"));
    assert_eq!(snapshot(&changed).object_blocks[0].gain, 0.25);
}
