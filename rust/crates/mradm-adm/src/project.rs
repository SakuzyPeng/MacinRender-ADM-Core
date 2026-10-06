use crate::{
    Document, Error, Result,
    document::{boolean, canonical_id, number, samples},
    records::*,
};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

#[derive(Debug, Default)]
pub struct Snapshot {
    pub strings: Vec<String>,
    pub indices: Vec<u32>,
    pub programmes: Vec<Programme>,
    pub contents: Vec<Content>,
    pub objects: Vec<Object>,
    pub tracks: Vec<Track>,
    pub object_blocks: Vec<ObjectBlock>,
    pub direct_blocks: Vec<DirectBlock>,
    pub hoa: Vec<Hoa>,
    pub hoa_channels: Vec<HoaChannel>,
    pub hoa_blocks: Vec<HoaBlock>,
    pub warnings: Vec<u32>,
}

impl Snapshot {
    fn string(&mut self, value: impl Into<String>) -> u32 {
        let index = u32::try_from(self.strings.len()).expect("ADM string table overflow");
        self.strings.push(value.into());
        index
    }
    fn optional_string(&mut self, value: Option<&str>) -> u32 {
        value.map_or(NONE, |s| self.string(s))
    }
    fn list(&mut self, values: impl IntoIterator<Item = String>) -> Span {
        let offset = self.indices.len();
        for value in values {
            let index = self.string(value);
            self.indices.push(index);
        }
        Span {
            offset,
            len: self.indices.len() - offset,
        }
    }
    fn labels(&mut self, doc: &Document, key: u32, name: &str) -> Span {
        self.list(
            doc.children(key, name)
                .iter()
                .map(|k| doc.node(*k).text.clone()),
        )
    }
    fn references(&mut self, doc: &Document, key: u32, name: &str) -> Span {
        self.list(
            doc.children(key, name)
                .iter()
                .map(|k| canonical_id(&doc.node(*k).text)),
        )
    }
    fn loudness(&mut self, doc: &Document, key: u32) -> Result<Loudness> {
        let Some(key) = doc.child(key, "loudnessMetadata") else {
            return Ok(Loudness {
                method: NONE,
                ..Loudness::default()
            });
        };
        let mut out = Loudness {
            present: 1,
            method: self.optional_string(doc.node(key).attr("loudnessMethod")),
            ..Loudness::default()
        };
        for (i, name) in [
            "integratedLoudness",
            "maxTruePeak",
            "loudnessRange",
            "maxMomentary",
            "maxShortTerm",
            "dialogueLoudness",
        ]
        .iter()
        .enumerate()
        {
            if let Some(value) = doc.text(key, name) {
                out.fields |= 1 << i;
                let value = float(value)?;
                match i {
                    0 => out.integrated = value,
                    1 => out.true_peak = value,
                    2 => out.range = value,
                    3 => out.momentary = value,
                    4 => out.short_term = value,
                    _ => out.dialogue = value,
                }
            }
        }
        Ok(out)
    }
    pub fn extract(doc: &Document, rate: u32, chna: &[(String, u16)]) -> Result<Self> {
        if rate == 0 {
            return Err(Error::invalid("采样率必须大于零"));
        }
        let mut out = Self::default();
        let uid_map: HashMap<_, _> = chna
            .iter()
            .map(|(id, c)| (canonical_id(id), i32::from(*c)))
            .collect();
        for key in doc.of_type("audioProgramme") {
            let n = doc.node(key);
            let item = Programme {
                id: out.string(doc.id(key)),
                name: out.string(required_name(doc, key, "audioProgrammeName")?),
                language: out.optional_string(n.attr("audioProgrammeLanguage")),
                content_ids: out.references(doc, key, "audioContentIDRef"),
                labels: out.labels(doc, key, "audioProgrammeLabel"),
                start: time_attr(doc, key, "start", rate)?.unwrap_or(0),
                end_present: u32::from(n.attr("end").is_some()),
                end: time_attr(doc, key, "end", rate)?.unwrap_or(0),
                loudness: out.loudness(doc, key)?,
                reference_screen: u32::from(
                    doc.child(key, "audioProgrammeReferenceScreen").is_some(),
                ),
            };
            out.programmes.push(item);
        }
        for key in doc.of_type("audioContent") {
            let dialogue = doc.child(key, "dialogue");
            let (kind, subkind) = if let Some(d) = dialogue {
                let n = doc.node(d);
                let (kind, attribute, kinds): (&str, &str, &[&str]) = match n.text.trim() {
                    "0" => (
                        "non-dialogue",
                        "nonDialogueContentKind",
                        &["", "music", "effect"],
                    ),
                    "1" => (
                        "dialogue",
                        "dialogueContentKind",
                        &[
                            "",
                            "dialogue",
                            "voiceover",
                            "spoken-subtitle",
                            "audio-description",
                            "commentary",
                            "emergency",
                        ],
                    ),
                    "2" => (
                        "mixed",
                        "mixedContentKind",
                        &["", "complete-main", "mixed", "hearing-impaired"],
                    ),
                    _ => return Err(Error::xml("无效 dialogue")),
                };
                let sub = n
                    .attr(attribute)
                    .map(|v| {
                        v.parse::<usize>()
                            .map_err(|_| Error::xml("无效 contentKind"))
                    })
                    .transpose()?
                    .and_then(|i| kinds.get(i))
                    .filter(|s| !s.is_empty())
                    .map(|s| out.string(*s))
                    .unwrap_or(NONE);
                (out.string(kind), sub)
            } else {
                (NONE, NONE)
            };
            let item = Content {
                id: out.string(doc.id(key)),
                name: out.string(required_name(doc, key, "audioContentName")?),
                language: out.optional_string(doc.node(key).attr("audioContentLanguage")),
                object_ids: out.references(doc, key, "audioObjectIDRef"),
                labels: out.labels(doc, key, "audioContentLabel"),
                loudness: out.loudness(doc, key)?,
                dialogue_kind: kind,
                content_kind: subkind,
            };
            out.contents.push(item);
        }
        let (offsets, parented) = offsets(doc, rate)?;
        let mut skipped = BTreeSet::new();
        for key in doc.of_type("audioObject") {
            let n = doc.node(key);
            let source_gain = gain(doc, key)?;
            let start = time_attr(doc, key, "start", rate)?.unwrap_or(0);
            let absolute = offsets[&key];
            let duration = time_attr(doc, key, "duration", rate)?;
            let locked = bool_element(doc, key, "headLocked", 0)?;
            let mut object = Object {
                node: key,
                id: out.string(doc.id(key)),
                name: out.string(required_name(doc, key, "audioObjectName")?),
                gain: source_gain.linear,
                mute: bool_element(doc, key, "mute", 0)?,
                head_locked: locked,
                end: duration.map_or(u64::MAX, |d| absolute.saturating_add(d)),
                offset_present: u32::from(doc.child(key, "positionOffset").is_some()),
                position_offset: position(doc, key, "positionOffset", true)?,
                labels: out.labels(doc, key, "audioObjectLabel"),
                importance_present: u32::from(n.attr("importance").is_some()),
                importance: integer(n.attr("importance"), 0)?,
                dialogue_present: u32::from(n.attr("dialogue").is_some()),
                dialogue: integer(n.attr("dialogue"), 0)?
                    .try_into()
                    .map_err(|_| Error::xml("无效 dialogue"))?,
                source_gain,
                mute_present: u32::from(doc.child(key, "mute").is_some()),
                start_present: u32::from(n.attr("start").is_some()),
                start,
                absolute_start: absolute,
                duration_present: u32::from(duration.is_some()),
                duration: duration.unwrap_or(0),
                child_objects: out.references(doc, key, "audioObjectIDRef"),
                has_parent: u32::from(parented.contains(&key)),
                ..Object::default()
            };
            object.tracks.offset = out.tracks.len();
            for reference in doc.children(key, "audioTrackUIDRef") {
                let uid_id = canonical_id(&doc.node(reference).text);
                let uid = doc.ids.get(&uid_id).copied();
                let mut track = Track {
                    uid: out.string(&uid_id),
                    channel: uid_map.get(&uid_id).copied().unwrap_or(-1),
                    blocks: Span {
                        offset: out.object_blocks.len(),
                        len: 0,
                    },
                    ds_blocks: Span {
                        offset: out.direct_blocks.len(),
                        len: 0,
                    },
                };
                if let Some((uid, pack)) =
                    uid.and_then(|u| doc.reference(u, "audioPackFormatIDRef").map(|p| (u, p)))
                {
                    let channels = doc
                        .channel(uid)
                        .map_or_else(|| doc.refs(pack, "audioChannelFormatIDRef"), |c| vec![c]);
                    for channel in channels {
                        match doc.type_of(channel)? {
                            3 => {
                                for block in doc.children(channel, "audioBlockFormat") {
                                    out.object_blocks
                                        .push(object_block(doc, block, absolute, locked, rate)?);
                                }
                            }
                            1 => {
                                for block in doc.children(channel, "audioBlockFormat") {
                                    let block = out.direct_block(
                                        doc, block, channel, pack, absolute, locked, rate,
                                    )?;
                                    out.direct_blocks.push(block);
                                }
                            }
                            4 => {}
                            2 => {
                                skipped.insert("Matrix");
                            }
                            5 => {
                                skipped.insert("Binaural");
                            }
                            _ => unreachable!(),
                        }
                    }
                }
                track.blocks.len = out.object_blocks.len() - track.blocks.offset;
                track.ds_blocks.len = out.direct_blocks.len() - track.ds_blocks.offset;
                out.tracks.push(track);
            }
            object.tracks.len = out.tracks.len() - object.tracks.offset;
            out.hoa_for_object(doc, key, &uid_map, absolute, object.end, locked, rate)?;
            out.objects.push(object);
        }
        for name in skipped {
            let id = out.string(format!(
                "typeDefinition={name} is not supported — tracks of this type are silently skipped"
            ));
            out.warnings.push(id);
        }
        Ok(out)
    }
    #[allow(clippy::too_many_arguments)]
    fn direct_block(
        &mut self,
        doc: &Document,
        key: u32,
        channel: u32,
        pack: u32,
        start: u64,
        locked: u32,
        rate: u32,
    ) -> Result<DirectBlock> {
        let pos = position(doc, key, "position", false)?;
        let source = block_source(doc, key, rate)?;
        let begin = start.saturating_add(source.rtime_samples);
        let low_pass = doc
            .children(channel, "frequency")
            .into_iter()
            .chain(doc.children(channel, "channelFrequency"))
            .find(|k| doc.node(*k).attr("typeDefinition") == Some("lowPass"))
            .map(|k| float(&doc.node(k).text))
            .transpose()?;
        let mut out = DirectBlock {
            node: key,
            labels: self.labels(doc, key, "speakerLabel"),
            pack_id: self.string(doc.id(pack)),
            has_position: 1,
            position: pos,
            azimuth: pos.azimuth,
            elevation: pos.elevation,
            distance: pos.distance,
            gain: source.gain.linear,
            low_pass_present: u32::from(low_pass.is_some()),
            low_pass: low_pass.unwrap_or(0.0),
            start: begin,
            end: if source.duration_present != 0 {
                begin.saturating_add(source.duration_samples)
            } else {
                u64::MAX
            },
            head_locked: bool_element(doc, key, "headLocked", locked)?,
            source,
            ..DirectBlock::default()
        };
        if pos.cartesian != 0 {
            let (x, y, z) = (f64::from(pos.x), f64::from(pos.y), f64::from(pos.z));
            out.azimuth = ((-x).atan2(y) * (180.0 / std::f64::consts::PI)) as f32;
            out.elevation =
                (z.atan2((x * x + y * y).sqrt()) * (180.0 / std::f64::consts::PI)) as f32;
            out.distance = (x * x + y * y + z * z).sqrt() as f32;
        } else {
            for p in doc.children(key, "position") {
                let n = doc.node(p);
                let Some(bound) = n.attr("bound") else {
                    continue;
                };
                let i = match (n.attr("coordinate"), bound) {
                    (Some("azimuth"), "min") => 0,
                    (Some("azimuth"), "max") => 1,
                    (Some("elevation"), "min") => 2,
                    (Some("elevation"), "max") => 3,
                    (Some("distance"), "min") => 4,
                    (Some("distance"), "max") => 5,
                    _ => return Err(Error::xml("无效扬声器坐标边界")),
                };
                out.bounds_present |= 1 << i;
                let v = float(&n.text)?;
                match i {
                    0 => out.azimuth_min = v,
                    1 => out.azimuth_max = v,
                    2 => out.elevation_min = v,
                    3 => out.elevation_max = v,
                    4 => out.distance_min = v,
                    _ => out.distance_max = v,
                }
            }
        }
        Ok(out)
    }
    #[allow(clippy::too_many_arguments)]
    fn hoa_for_object(
        &mut self,
        doc: &Document,
        object: u32,
        uid_map: &HashMap<String, i32>,
        start: u64,
        end: u64,
        locked: u32,
        rate: u32,
    ) -> Result<()> {
        let mut packs = BTreeMap::<String, (u32, Vec<u32>)>::new();
        for uid in doc.refs(object, "audioTrackUIDRef") {
            if let Some(pack) = doc.reference(uid, "audioPackFormatIDRef")
                && doc.type_of(pack)? == 4
            {
                packs
                    .entry(doc.id(pack))
                    .or_insert_with(|| (pack, Vec::new()))
                    .1
                    .push(uid);
            }
        }
        for (id, (pack, uids)) in packs {
            let pack_node = doc.node(pack);
            let mut item = Hoa {
                object_id: self.string(doc.id(object)),
                pack_id: self.string(id),
                normalization: self.string(pack_node.attr("normalization").unwrap_or("SN3D")),
                nfc_ref_dist: pack_node.attr("nfcRefDist").map_or(Ok(0.0), number)?,
                screen_ref: pack_node.attr("screenRef").map_or(Ok(0), boolean)?,
                gain: gain(doc, object)?.linear,
                mute: bool_element(doc, object, "mute", 0)?,
                head_locked: locked,
                start,
                end,
                channels: Span {
                    offset: self.hoa_channels.len(),
                    len: 0,
                },
            };
            let mut got_metadata = false;
            for uid in uids {
                let Some(cf) = doc.channel(uid) else {
                    continue;
                };
                if doc.type_of(cf)? != 4 {
                    continue;
                }
                let blocks = doc.children(cf, "audioBlockFormat");
                let Some(&first) = blocks.first() else {
                    continue;
                };
                if !got_metadata {
                    if let Some(s) = doc.text(first, "normalization") {
                        item.normalization = self.string(s);
                    }
                    if let Some(s) = doc.text(first, "nfcRefDist") {
                        item.nfc_ref_dist = number(s)?;
                    }
                    if let Some(s) = doc.text(first, "screenRef") {
                        item.screen_ref = boolean(s)?;
                    }
                    got_metadata = true;
                }
                let id = doc.id(uid);
                let mut channel = HoaChannel {
                    uid: self.string(&id),
                    channel: uid_map.get(&id).copied().unwrap_or(-1),
                    order: integer(doc.text(first, "order"), 0)?,
                    degree: integer(doc.text(first, "degree"), 0)?,
                    blocks: Span {
                        offset: self.hoa_blocks.len(),
                        len: 0,
                    },
                };
                for block in blocks {
                    let begin =
                        start.saturating_add(time_attr(doc, block, "rtime", rate)?.unwrap_or(0));
                    let finish = time_attr(doc, block, "duration", rate)?
                        .map_or(end, |d| begin.saturating_add(d).min(end));
                    self.hoa_blocks.push(HoaBlock {
                        node: block,
                        gain: gain(doc, block)?.linear,
                        head_locked: bool_element(doc, block, "headLocked", locked)?,
                        start: begin,
                        end: finish,
                    });
                }
                channel.blocks.len = self.hoa_blocks.len() - channel.blocks.offset;
                self.hoa_channels.push(channel);
            }
            item.channels.len = self.hoa_channels.len() - item.channels.offset;
            if item.channels.len != 0 {
                self.hoa.push(item);
            }
        }
        Ok(())
    }
}

fn required_name<'a>(doc: &'a Document, key: u32, name: &str) -> Result<&'a str> {
    doc.node(key)
        .attr(name)
        .ok_or_else(|| Error::xml(format!("ADM 元素缺少 {name}")))
}
fn integer(value: Option<&str>, default: i32) -> Result<i32> {
    value.map_or(Ok(default), |s| {
        s.trim().parse::<i32>().map_err(|_| Error::xml("无效整数"))
    })
}
fn float(text: &str) -> Result<f32> {
    let n = number(text)? as f32;
    if !n.is_finite() {
        return Err(Error::xml("ADM 浮点值超出范围"));
    }
    Ok(n)
}
fn float_element(doc: &Document, key: u32, name: &str, default: f32) -> Result<f32> {
    doc.text(key, name).map_or(Ok(default), float)
}
fn bool_element(doc: &Document, key: u32, name: &str, default: u32) -> Result<u32> {
    doc.text(key, name).map_or(Ok(default), boolean)
}
fn time_attr(doc: &Document, key: u32, name: &str, rate: u32) -> Result<Option<u64>> {
    doc.node(key)
        .attr(name)
        .map(|s| samples(s, rate))
        .transpose()
}
fn gain(doc: &Document, key: u32) -> Result<Gain> {
    let Some(k) = doc.child(key, "gain") else {
        return Ok(Gain {
            value: 1.0,
            linear: 1.0,
            ..Gain::default()
        });
    };
    let n = doc.node(k);
    let value = number(&n.text)?;
    let decibels = match n.attr("gainUnit").unwrap_or("linear") {
        "linear" => 0,
        "dB" => 1,
        _ => return Err(Error::xml("无效 gainUnit")),
    };
    let linear = if decibels != 0 {
        10.0f64.powf(value / 20.0) as f32
    } else {
        value as f32
    };
    if !linear.is_finite() || linear < 0.0 {
        return Err(Error::xml("无效 gain"));
    }
    Ok(Gain {
        present: 1,
        decibels,
        value,
        linear,
    })
}
fn block_source(doc: &Document, key: u32, rate: u32) -> Result<BlockSource> {
    let duration = time_attr(doc, key, "duration", rate)?;
    Ok(BlockSource {
        gain: gain(doc, key)?,
        rtime_present: u32::from(doc.node(key).attr("rtime").is_some()),
        rtime_samples: time_attr(doc, key, "rtime", rate)?.unwrap_or(0),
        duration_present: u32::from(duration.is_some()),
        duration_samples: duration.unwrap_or(0),
    })
}
fn position(doc: &Document, key: u32, name: &str, offset: bool) -> Result<AdmPosition> {
    let positions = doc.children(key, name);
    let cartesian = positions
        .first()
        .and_then(|p| doc.node(*p).attr("coordinate"))
        .is_some_and(|s| matches!(s, "X" | "Y" | "Z"));
    let mut out = AdmPosition {
        cartesian: u32::from(cartesian),
        distance: if offset { 0.0 } else { 1.0 },
        ..AdmPosition::default()
    };
    for p in positions {
        let n = doc.node(p);
        let coordinate = n
            .attr("coordinate")
            .ok_or_else(|| Error::xml("position 缺少 coordinate"))?;
        let is_cartesian = matches!(coordinate, "X" | "Y" | "Z");
        if is_cartesian != cartesian {
            return Err(Error::xml("混合的坐标系统"));
        }
        if let Some(bound) = n.attr("bound") {
            if bound != "min" && bound != "max" {
                return Err(Error::xml("无效 position bound"));
            }
            continue;
        }
        let v = float(&n.text)?;
        match coordinate {
            "X" => out.x = v,
            "Y" => out.y = v,
            "Z" => out.z = v,
            "azimuth" => out.azimuth = v,
            "elevation" => out.elevation = v,
            "distance" => out.distance = v,
            _ => return Err(Error::xml("无效 position coordinate")),
        }
    }
    Ok(out)
}
fn object_block(
    doc: &Document,
    key: u32,
    start: u64,
    locked: u32,
    rate: u32,
) -> Result<ObjectBlock> {
    let source = block_source(doc, key, rate)?;
    let begin = start.saturating_add(source.rtime_samples);
    let jump = doc.child(key, "jumpPosition");
    let interpolation = jump
        .and_then(|j| doc.node(j).attr("interpolationLength"))
        .map(|s| {
            let seconds = number(s)?;
            if seconds < 0.0 || seconds * 1e9 > i64::MAX as f64 {
                return Err(Error::xml("无效 interpolationLength"));
            }
            let ns = (seconds * 1e9) as u64;
            Ok(((u128::from(ns) * u128::from(rate) + 500_000_000) / 1_000_000_000) as u64)
        })
        .transpose()?;
    let lock = doc.child(key, "channelLock");
    let max = lock
        .and_then(|l| doc.node(l).attr("maxDistance"))
        .map(float)
        .transpose()?;
    let divergence = doc.child(key, "objectDivergence");
    Ok(ObjectBlock {
        node: key,
        position: position(doc, key, "position", false)?,
        gain: source.gain.linear,
        diffuse: float_element(doc, key, "diffuse", 0.0)?,
        width: float_element(doc, key, "width", 0.0)?,
        height: float_element(doc, key, "height", 0.0)?,
        depth: float_element(doc, key, "depth", 0.0)?,
        start: begin,
        end: if source.duration_present != 0 {
            begin.saturating_add(source.duration_samples)
        } else {
            u64::MAX
        },
        jump: bool_element(doc, key, "jumpPosition", 0)?,
        interpolation_present: u32::from(interpolation.is_some()),
        interpolation: interpolation.unwrap_or(0),
        channel_lock: bool_element(doc, key, "channelLock", 0)?,
        max_distance_present: u32::from(max.is_some()),
        max_distance: max.unwrap_or(0.0),
        divergence: float_element(doc, key, "objectDivergence", 0.0)?,
        azimuth_range: divergence
            .and_then(|d| doc.node(d).attr("azimuthRange"))
            .map_or(Ok(45.0), float)?,
        position_range: divergence
            .and_then(|d| doc.node(d).attr("positionRange"))
            .map_or(Ok(0.0), float)?,
        screen_ref: bool_element(doc, key, "screenRef", 0)?,
        head_locked: bool_element(doc, key, "headLocked", locked)?,
        source,
    })
}
fn offsets(doc: &Document, rate: u32) -> Result<(HashMap<u32, u64>, HashSet<u32>)> {
    let mut offsets = HashMap::new();
    let mut parented = HashSet::new();
    let mut work = Vec::new();
    for content in doc.of_type("audioContent") {
        for object in doc.refs(content, "audioObjectIDRef") {
            work.push((object, 0u64));
        }
    }
    while let Some((object, inherited)) = work.pop() {
        let start = inherited.saturating_add(time_attr(doc, object, "start", rate)?.unwrap_or(0));
        if offsets.get(&object).is_some_and(|s| *s <= start) {
            continue;
        }
        offsets.insert(object, start);
        for child in doc.refs(object, "audioObjectIDRef") {
            work.push((child, start));
        }
    }
    for object in doc.of_type("audioObject") {
        offsets
            .entry(object)
            .or_insert(time_attr(doc, object, "start", rate)?.unwrap_or(0));
        parented.extend(doc.refs(object, "audioObjectIDRef"));
    }
    Ok((offsets, parented))
}
