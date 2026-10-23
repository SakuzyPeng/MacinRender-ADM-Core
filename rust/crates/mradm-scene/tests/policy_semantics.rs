//! Semantic policy behaviour on small ADM scenes, moved from the C++ `semantic_policy_test.cpp`
//! when the C++ policy was removed (PCM consistency 4.4). The scenes reproduce the C++ test's
//! hand-built ones as AXML; byte identity with the C++ implementation is `policy.rs`.
use mradm_adm::{Document, Snapshot};
use mradm_scene::{Info, RenderScene, SourceKind, policy};

const RATE: u32 = 48000;

#[derive(Default)]
struct Adm {
    xml: String,
    uids: Vec<String>,
}

impl Adm {
    fn uid(&mut self) -> String {
        let uid = format!("ATU_{:08}", self.uids.len() + 1);
        self.uids.push(uid.clone());
        uid
    }

    /// One channel of `pack`; returns its track UID.
    fn channel(
        &mut self,
        type_label: &str,
        pack: &str,
        channel_xml: &str,
        block_xml: &str,
    ) -> String {
        // Custom IDs start at 0x1001, past the BS.2094 common definitions.
        let n = self.uids.len() + 1001;
        let uid = self.uid();
        let c = format!("AC_{type_label}{n:04}");
        let t = format!("AT_{type_label}{n:04}_01");
        let s = format!("AS_{type_label}{n:04}");
        self.xml += &format!(
            r#"<audioChannelFormat audioChannelFormatID="{c}" audioChannelFormatName="C{n}" typeLabel="{type_label}">{channel_xml}<audioBlockFormat audioBlockFormatID="AB_{type_label}{n:04}_00000001">{block_xml}</audioBlockFormat></audioChannelFormat><audioStreamFormat audioStreamFormatID="{s}" audioStreamFormatName="S{n}" formatDefinition="pcm" formatLabel="0001"><audioChannelFormatIDRef>{c}</audioChannelFormatIDRef><audioTrackFormatIDRef>{t}</audioTrackFormatIDRef></audioStreamFormat><audioTrackFormat audioTrackFormatID="{t}" audioTrackFormatName="T{n}" formatDefinition="pcm" formatLabel="0001"><audioStreamFormatIDRef>{s}</audioStreamFormatIDRef></audioTrackFormat><audioTrackUID UID="{uid}"><audioTrackFormatIDRef>{t}</audioTrackFormatIDRef><audioPackFormatIDRef>{pack}</audioPackFormatIDRef></audioTrackUID>"#
        );
        uid
    }

    fn pack(&mut self, type_label: &str, pack: &str, channels: usize) {
        let first = self.uids.len() + 1001;
        let refs: String = (first..first + channels)
            .map(|n| {
                format!("<audioChannelFormatIDRef>AC_{type_label}{n:04}</audioChannelFormatIDRef>")
            })
            .collect();
        self.xml += &format!(
            r#"<audioPackFormat audioPackFormatID="{pack}" audioPackFormatName="P{pack}" typeLabel="{type_label}">{refs}</audioPackFormat>"#
        );
    }

    fn object(&mut self, id: &str, attributes: &str, uids: &[String]) {
        let refs: String = uids
            .iter()
            .map(|u| format!("<audioTrackUIDRef>{u}</audioTrackUIDRef>"))
            .collect();
        self.xml +=
            &format!(r#"<audioObject audioObjectID="{id}" {attributes}>{refs}</audioObject>"#);
    }

    /// An Objects-type object with one block.
    fn objects(&mut self, id: &str, attributes: &str, block_xml: &str) {
        let pack = format!("AP_0003{:04}", self.uids.len() + 1001);
        self.pack("0003", &pack, 1);
        let uid = self.channel("0003", &pack, "", block_xml);
        self.object(id, attributes, &[uid]);
    }

    fn scene(&self) -> RenderScene {
        let xml = format!(
            r#"<?xml version="1.0" encoding="UTF-8"?><ebuCoreMain><coreMetadata><format><audioFormatExtended>{}</audioFormatExtended></format></coreMetadata></ebuCoreMain>"#,
            self.xml
        );
        let document = Document::parse(&xml).unwrap();
        let chna: Vec<_> = (0..)
            .zip(&self.uids)
            .map(|(channel, uid)| (uid.clone(), channel))
            .collect();
        let snapshot = Snapshot::extract(&document, RATE, &chna).unwrap();
        RenderScene::new(
            Info {
                sample_rate: RATE,
                channels: self.uids.len() as u16,
                frames: 48000,
                source_kind: SourceKind::Adm,
                input_layout: String::new(),
                input_channel_labels: Vec::new(),
            },
            snapshot,
        )
    }
}

const POLAR: &str = r#"<position coordinate="azimuth">0</position><position coordinate="elevation">0</position><position coordinate="distance">1</position>"#;

/// Kick (AO_1001) and cymbal (AO_1002), as the C++ `make_scene`.
fn objects_scene(kick_position: &str) -> RenderScene {
    let mut adm = Adm::default();
    adm.objects(
        "AO_1001",
        r#"audioObjectName="Kick Stem""#,
        &format!(
            r#"{kick_position}<width>0.4</width><height>0.2</height><depth>0.1</depth><diffuse>1</diffuse><channelLock maxDistance="0.5">1</channelLock><objectDivergence azimuthRange="90" positionRange="0.25">0.5</objectDivergence><jumpPosition interpolationLength="0.2">1</jumpPosition>"#
        ),
    );
    adm.objects(
        "AO_1002",
        r#"audioObjectName="Cymbal Stem""#,
        &format!(r#"{POLAR}<width>0.5</width><height>0.5</height><depth>0.5</depth><diffuse>0.5</diffuse>"#),
    );
    adm.scene()
}

/// Dialogue (AO_2001, importance 9, dialogue) in content and programme; ambience (AO_2002).
fn rich_scene() -> RenderScene {
    let mut adm = Adm::default();
    adm.objects(
        "AO_2001",
        r#"audioObjectName="Dialogue" importance="9" dialogue="1""#,
        POLAR,
    );
    adm.objects(
        "AO_2002",
        r#"audioObjectName="Ambience" importance="2" dialogue="0""#,
        POLAR,
    );
    adm.xml += r#"<audioContent audioContentID="ACO_1001" audioContentName="Main Dialogue"><audioObjectIDRef>AO_2001</audioObjectIDRef></audioContent><audioProgramme audioProgrammeID="APR_1001" audioProgrammeName="English"><audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme>"#;
    adm.scene()
}

/// One bed (AO_4001): M+000 and an LFE channel.
fn bed_scene() -> RenderScene {
    let mut adm = Adm::default();
    adm.pack("0001", "AP_00011001", 2);
    let center = adm.channel(
        "0001",
        "AP_00011001",
        "",
        &format!("{POLAR}<speakerLabel>M+000</speakerLabel>"),
    );
    let lfe = adm.channel(
        "0001",
        "AP_00011001",
        r#"<frequency typeDefinition="lowPass">120</frequency>"#,
        &format!("{POLAR}<speakerLabel>LFE</speakerLabel>"),
    );
    adm.object("AO_4001", r#"audioObjectName="Bed""#, &[center, lfe]);
    adm.scene()
}

/// One first-channel HOA pack (AO_5001, AP_00041001).
fn hoa_scene() -> RenderScene {
    let mut adm = Adm::default();
    adm.pack("0004", "AP_00041001", 1);
    let uid = adm.channel(
        "0004",
        "AP_00041001",
        "",
        "<order>0</order><degree>0</degree>",
    );
    adm.object("AO_5001", r#"audioObjectName="Ambisonics""#, &[uid]);
    adm.scene()
}

fn apply(scene: &mut RenderScene, json: &str) -> Vec<String> {
    let policy = policy::parse(json.as_bytes()).unwrap_or_else(|e| panic!("{json}: {}", e.message));
    scene.apply_policy(policy).to_vec()
}

fn near(a: f32, b: f32) -> bool {
    (a - b).abs() < 1e-5
}

fn object_block(scene: &RenderScene, object: usize) -> mradm_adm::records::ObjectBlock {
    let s = &scene.snapshot;
    let track = s.tracks[s.objects[object].tracks.offset];
    s.object_blocks[track.blocks.offset]
}

fn direct_block(scene: &RenderScene, track: usize) -> mradm_adm::records::DirectBlock {
    let s = &scene.snapshot;
    let track = s.tracks[s.objects[0].tracks.offset + track];
    s.direct_blocks[track.ds_blocks.offset]
}

fn muted(scene: &RenderScene) -> Vec<bool> {
    scene.overlay.objects.iter().map(|o| o.mute).collect()
}

#[test]
fn block_sections_scale_clamp_and_disable() {
    let mut scene = objects_scene(POLAR);
    let warnings = apply(
        &mut scene,
        r#"{"schema": "mradm.semantic-policy.v1",
  "global": {
    "diffuse": { "scale": 0.5, "max": 0.3 },
    "extent": { "scale": 0.5, "height_scale": 0.25, "max": 0.2 },
    "divergence": { "scale": 0.5, "range_scale": 0.5, "max_range": 30.0 },
    "channel_lock": { "enabled": false },
    "interpolation": { "honor_jump_position": false, "max_ms": 50 }
  },
  "objects": [ { "name_glob": "*kick*", "diffuse": { "enabled": false }, "extent": { "enabled": false } } ]}"#,
    );
    assert!(warnings.is_empty(), "all policy object rules matched");
    let kick = object_block(&scene, 0);
    assert!(near(kick.diffuse, 0.), "object rule disables kick diffuse");
    assert!(
        near(kick.width, 0.) && near(kick.height, 0.) && near(kick.depth, 0.),
        "kick extent disabled"
    );
    assert!(
        near(kick.divergence, 0.25),
        "global divergence scale applied"
    );
    assert!(
        near(kick.azimuth_range, 30.),
        "global divergence range clamped"
    );
    assert!(
        kick.channel_lock == 0 && kick.max_distance_present == 0,
        "channelLock disabled"
    );
    assert_eq!(kick.jump, 0, "jumpPosition ignored");
    assert!(
        kick.interpolation_present != 0 && kick.interpolation == 2400,
        "interpolationLength max_ms clamped"
    );
    let cymbal = object_block(&scene, 1);
    assert!(
        near(cymbal.diffuse, 0.25),
        "global diffuse scale/max applied"
    );
    assert!(near(cymbal.width, 0.2), "extent width clamped");
    assert!(near(cymbal.height, 0.0625), "extent height scaled");
    assert!(near(cymbal.depth, 0.2), "extent depth clamped");
}

/// Polar extents are in degrees: without `max` a scale keeps them proportional within the ADM
/// ranges (360 degrees for width and height, 1 for depth) instead of clamping them to 1, the
/// default before the extent fix.
#[test]
fn extent_without_max_keeps_the_adm_ranges() {
    let mut adm = Adm::default();
    adm.objects(
        "AO_1001",
        r#"audioObjectName="Wide""#,
        &format!(r#"{POLAR}<width>30</width><height>200</height><depth>0.5</depth>"#),
    );
    let mut scene = adm.scene();
    let warnings = apply(
        &mut scene,
        r#"{"schema": "mradm.semantic-policy.v1", "global": {"extent": {"scale": 0.5, "height_scale": 4}}}"#,
    );
    assert!(warnings.is_empty());
    let block = object_block(&scene, 0);
    assert_eq!((block.width, block.height, block.depth), (15., 360., 0.25));
}

#[test]
fn invalid_policies_are_rejected() {
    let unknown = policy::parse(
        br#"{"schema": "mradm.semantic-policy.v1", "global": { "diffuse": { "scle": 1.0 } }}"#,
    );
    assert_eq!(unknown.unwrap_err().code, 1, "unknown field rejected");
    let overflow = policy::parse(
        br#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"gain_db":1e400}}}"#,
    );
    assert_eq!(
        overflow.unwrap_err().code,
        1,
        "number overflow rejected as invalid policy"
    );
    let head_locked =
        policy::parse(br#"{"schema":"mradm.semantic-policy.v1","global":{"head_locked":"false"}}"#);
    assert!(head_locked.is_err(), "non-boolean head_locked is rejected");
}

#[test]
fn ids_and_track_uids_match_case_insensitively() {
    let mut scene = objects_scene(POLAR);
    let warnings = apply(
        &mut scene,
        r#"{"schema": "mradm.semantic-policy.v1", "objects": [
  { "id": "ao_1001", "diffuse": { "enabled": false } },
  { "track_uid": "atu_00000002", "extent": { "enabled": false } } ]}"#,
    );
    assert!(warnings.is_empty());
    assert!(
        near(object_block(&scene, 0).diffuse, 0.),
        "object id match ignores case"
    );
    let cymbal = object_block(&scene, 1);
    assert!(
        near(cymbal.width, 0.) && near(cymbal.height, 0.) && near(cymbal.depth, 0.),
        "track_uid match ignores case"
    );
}

#[test]
fn object_gain_and_mute() {
    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":0.5}}}"#,
    );
    let s = &scene.snapshot;
    assert!(
        near(s.objects[0].gain, 0.5) && near(s.objects[1].gain, 0.5),
        "gain scale halves all object gains"
    );
    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"gain_db":-6.0205999}}}"#,
    );
    assert!(
        near(scene.snapshot.objects[0].gain, 0.5),
        "gain_db -6.02 dB halves object gain"
    );
    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"name_glob":"*kick*","gain":{"mute":true}}]}"#,
    );
    assert_eq!(
        muted(&scene),
        [true, false],
        "mute targets only matched object"
    );
    assert_eq!(
        scene.snapshot.objects[0].mute, 0,
        "the authored mute stays in the record"
    );
}

#[test]
fn position_overrides() {
    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1001","position":{"azimuth":30.0,"offset":{"azimuth":10.0},"lock_elevation":45.0}}]}"#,
    );
    let p = object_block(&scene, 0).position;
    assert_eq!(p.cartesian, 0, "position output is polar");
    assert!(near(p.azimuth, 40.), "absolute + offset azimuth (30+10)");
    assert!(near(p.elevation, 45.), "lock_elevation forces elevation");

    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1001","position":{"azimuth":30.0,"lock_azimuth":90.0}}]}"#,
    );
    assert!(
        near(object_block(&scene, 0).position.azimuth, 90.),
        "lock_azimuth wins over absolute"
    );

    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1001","position":{"azimuth":200.0,"elevation":200.0}}]}"#,
    );
    let p = object_block(&scene, 0).position;
    assert!(near(p.azimuth, -160.), "azimuth wraps to [-180,180]");
    assert!(near(p.elevation, 90.), "elevation clamps to 90");

    let front = r#"<position coordinate="X">0</position><position coordinate="Y">1</position><position coordinate="Z">0</position><cartesian>1</cartesian>"#;
    let mut scene = objects_scene(front);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1001","position":{"offset":{"azimuth":10.0}}}]}"#,
    );
    let p = object_block(&scene, 0).position;
    assert_eq!(
        p.cartesian, 0,
        "cartesian block converted to polar on real change"
    );
    assert!(
        near(p.azimuth, 10.),
        "front cartesian az 0 + offset 10 = 10"
    );

    let point = r#"<position coordinate="X">0.2</position><position coordinate="Y">0.5</position><position coordinate="Z">0.1</position><cartesian>1</cartesian>"#;
    let mut scene = objects_scene(point);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1001","position":{"offset":{"azimuth":0.0,"elevation":0.0,"distance":0.0}}}]}"#,
    );
    let p = object_block(&scene, 0).position;
    assert_eq!(p.cartesian, 1, "zero-offset leaves block Cartesian");
    assert!(
        near(p.x, 0.2) && near(p.y, 0.5) && near(p.z, 0.1),
        "zero-offset leaves coordinates unchanged"
    );
}

#[test]
fn match_dimensions() {
    for (rule, expected, what) in [
        (r#""all": true"#, [true, true], "all matches every object"),
        (
            r#""importance_max": 5"#,
            [false, true],
            "importance_max matches low-importance only",
        ),
        (
            r#""dialogue_id": 1"#,
            [true, false],
            "dialogue_id matches dialogue only",
        ),
        (
            r#""content": "Main Dialogue""#,
            [true, false],
            "content name matches its member object",
        ),
        (
            r#""programme": "APR_1001""#,
            [true, false],
            "programme id matches its member object",
        ),
    ] {
        let mut scene = rich_scene();
        apply(
            &mut scene,
            &format!(
                r#"{{"schema":"mradm.semantic-policy.v1","objects":[{{{rule},"gain":{{"mute":true}}}}]}}"#
            ),
        );
        assert_eq!(muted(&scene), expected, "{what}");
    }
}

#[test]
fn channel_lock_enable_and_disable() {
    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1002","channel_lock":{"enabled":true,"max_distance":0.3}}]}"#,
    );
    let b = object_block(&scene, 1);
    assert_eq!(b.channel_lock, 1, "channel_lock force-enabled");
    assert!(
        b.max_distance_present != 0 && near(b.max_distance, 0.3),
        "channel_lock max_distance set"
    );
    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_1001","channel_lock":{"enabled":false}}]}"#,
    );
    let b = object_block(&scene, 0);
    assert!(
        b.channel_lock == 0 && b.max_distance_present == 0,
        "disable clears max_distance"
    );
}

#[test]
fn direct_speakers_filters_gain_and_position() {
    let gains = |json: &str| {
        let mut scene = bed_scene();
        apply(&mut scene, json);
        [direct_block(&scene, 0).gain, direct_block(&scene, 1).gain]
    };
    let rule = |direct: &str| {
        format!(
            r#"{{"schema":"mradm.semantic-policy.v1","objects":[{{"id":"AO_4001","direct_speakers":{direct}}}]}}"#
        )
    };
    assert_eq!(
        gains(&rule(r#"{"gain":{"scale":0.5}}"#)),
        [0.5, 0.5],
        "ds gain scale applies to all DS blocks"
    );
    assert_eq!(
        gains(&rule(r#"{"gain":{"mute":true}}"#))[0],
        0.,
        "ds mute zeroes gain"
    );
    assert_eq!(
        gains(&rule(r#"{"speaker_label":"M+000","gain":{"scale":0.25}}"#)),
        [0.25, 1.],
        "label filter hits M+000 and spares LFE"
    );
    assert_eq!(
        gains(&rule(r#"{"lfe":true,"gain":{"mute":true}}"#)),
        [1., 0.],
        "lfe:true mutes LFE only"
    );
    assert_eq!(
        gains(&rule(r#"{"lfe":false,"gain":{"scale":0.5}}"#)),
        [0.5, 1.],
        "lfe:false hits non-LFE only"
    );
    assert_eq!(
        gains(&rule(
            r#"{"speaker_label":"M+000","lfe":true,"gain":{"scale":0.1}}"#
        )),
        [1., 1.],
        "AND filter (M+000 && lfe) matches nothing"
    );
    assert_eq!(
        gains(
            r#"{"schema":"mradm.semantic-policy.v1",
  "global": { "direct_speakers": { "speaker_label": "M+000", "gain": { "scale": 0.5 } } },
  "objects": [ { "id": "AO_4001", "direct_speakers": { "lfe": true, "gain": { "mute": true } } } ]}"#
        ),
        [0.5, 0.],
        "global and object DirectSpeakers rules keep their own filters"
    );

    let mut scene = bed_scene();
    apply(
        &mut scene,
        &rule(r#"{"speaker_label":"M+000","position":{"azimuth":30.0}}"#),
    );
    let state = scene.overlay.direct_blocks[scene.snapshot.tracks[0].ds_blocks.offset];
    assert!(
        state.has_position && state.position_override,
        "ds position re-aim sets has_position"
    );
    assert!(
        near(direct_block(&scene, 0).azimuth, 30.),
        "ds position re-aim sets azimuth"
    );
}

#[test]
fn head_locked_precedence() {
    let mut scene = objects_scene(POLAR);
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1", "global": { "head_locked": true }, "objects": [
    { "id": "AO_1001", "head_locked": false },
    { "id": "AO_1002", "head_locked": false },
    { "id": "AO_1002", "head_locked": true } ]}"#,
    );
    assert_eq!(
        object_block(&scene, 0).head_locked,
        0,
        "object head_locked=false overrides global true"
    );
    assert_eq!(
        object_block(&scene, 1).head_locked,
        1,
        "later object head_locked rule wins"
    );

    let mut scene = bed_scene();
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_4001","head_locked":true,
    "direct_speakers":{"speaker_label":"M+000","head_locked":false}}]}"#,
    );
    assert_eq!(
        direct_block(&scene, 0).head_locked,
        0,
        "filtered DS head_locked=false overrides the object"
    );
    assert_eq!(
        direct_block(&scene, 1).head_locked,
        1,
        "unmatched DS channel keeps head_locked=true"
    );

    let mut scene = hoa_scene();
    apply(
        &mut scene,
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"pack_format":"AP_00041001","head_locked":true}]}"#,
    );
    assert!(
        scene.snapshot.hoa_blocks.iter().all(|b| b.head_locked == 1),
        "HOA head_locked reaches channel blocks"
    );
}

#[test]
fn hoa_gain_mute_and_warnings() {
    let gain = |json: &str| {
        let mut scene = hoa_scene();
        let warnings = apply(&mut scene, json);
        (
            scene.snapshot.hoa[0].gain,
            scene.snapshot.hoa[0].mute,
            warnings,
        )
    };
    assert!(near(
        gain(r#"{"schema":"mradm.semantic-policy.v1","objects":[{"id":"AO_5001","gain":{"scale":0.5}}]}"#).0,
        0.5
    ));
    assert_eq!(
        gain(r#"{"schema":"mradm.semantic-policy.v1","objects":[{"pack_format":"AP_00041001","gain":{"mute":true}}]}"#).1,
        1,
        "hoa pack_format match mutes pack"
    );
    assert!(near(
        gain(r#"{"schema":"mradm.semantic-policy.v1","objects":[{"all":true,"gain":{"gain_db":-6.0205999}}]}"#).0,
        0.5
    ));
    assert!(near(
        gain(r#"{"schema":"mradm.semantic-policy.v1","global":{"gain":{"scale":0.25}}}"#).0,
        0.25
    ));
    let (_, _, warnings) = gain(
        r#"{"schema":"mradm.semantic-policy.v1","objects":[{"pack_format":"AP_00041001","gain":{"scale":0.9}}]}"#,
    );
    assert!(
        warnings.is_empty(),
        "hoa-only rule is not flagged unmatched"
    );
}
