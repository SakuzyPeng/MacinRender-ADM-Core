//! The scene inspect document `mradm.scene-inspect` v1 (PCM consistency 5.2 A), moved from the
//! C++ engine's `scene_json`. It reads the Rust render scene the C++ `AdmScene` copied field by
//! field: element records, the overlay's effective object mute and DirectSpeakers `has_position`,
//! and the import info. Byte-identical to the former nlohmann `dump(2)`.
use mradm_adm::{Snapshot, records::*};
use mradm_json::{Map, Value};
use mradm_scene::{RenderScene, SourceKind};

const NONE: u32 = u32::MAX;

struct Doc<'a> {
    s: &'a Snapshot,
}

impl Doc<'_> {
    fn text(&self, id: u32) -> Value {
        Value::from(self.s.strings[id as usize].as_str())
    }

    fn texts(&self, span: Span) -> Value {
        Value::Array(
            self.s.indices[span.offset..span.offset + span.len]
                .iter()
                .map(|&id| self.text(id))
                .collect(),
        )
    }

    fn optional_text(&self, map: &mut Map, key: &str, id: u32) {
        if id != NONE {
            map.insert(key.into(), self.text(id));
        }
    }

    fn loudness(&self, l: &Loudness) -> Value {
        let mut j = Map::new();
        for (bit, key, value) in [
            (1, "integrated_loudness", l.integrated),
            (2, "max_true_peak", l.true_peak),
            (4, "loudness_range", l.range),
            (8, "max_momentary", l.momentary),
            (16, "max_short_term", l.short_term),
            (32, "dialogue_loudness", l.dialogue),
        ] {
            if l.fields & bit != 0 {
                j.insert(key.into(), Value::from(value));
            }
        }
        self.optional_text(&mut j, "loudness_method", l.method);
        Value::Object(j)
    }
}

fn end_sample(map: &mut Map, end: u64) {
    if end != u64::MAX {
        map.insert("end_sample".into(), Value::from(end));
    }
}

fn position(cartesian: bool, polar: [f32; 3], xyz: [f32; 3]) -> Value {
    let mut j = Map::new();
    j.insert("cartesian".into(), Value::from(cartesian));
    let (keys, values) = if cartesian {
        (["x", "y", "z"], xyz)
    } else {
        (["azimuth", "elevation", "distance"], polar)
    };
    for (key, value) in keys.iter().zip(values) {
        j.insert((*key).into(), Value::from(value));
    }
    Value::Object(j)
}

fn adm_position(p: &AdmPosition) -> Value {
    position(
        p.cartesian != 0,
        [p.azimuth, p.elevation, p.distance],
        [p.x, p.y, p.z],
    )
}

fn object_block(b: &ObjectBlock) -> Value {
    let mut j = Map::new();
    j.insert("position".into(), adm_position(&b.position));
    for (key, value) in [
        ("gain", b.gain),
        ("diffuse", b.diffuse),
        ("width", b.width),
        ("height", b.height),
        ("depth", b.depth),
        ("divergence", b.divergence),
        ("divergence_azimuth_range", b.azimuth_range),
        ("divergence_position_range", b.position_range),
    ] {
        j.insert(key.into(), Value::from(value));
    }
    j.insert("head_locked".into(), Value::from(b.head_locked != 0));
    j.insert("start_sample".into(), Value::from(b.start));
    end_sample(&mut j, b.end);
    j.insert("jump_position".into(), Value::from(b.jump != 0));
    if b.interpolation_present != 0 {
        j.insert("interp_length_samples".into(), Value::from(b.interpolation));
    }
    j.insert("channel_lock".into(), Value::from(b.channel_lock != 0));
    if b.max_distance_present != 0 {
        j.insert(
            "channel_lock_max_distance".into(),
            Value::from(b.max_distance),
        );
    }
    j.insert("screen_ref".into(), Value::from(b.screen_ref != 0));
    Value::Object(j)
}

fn direct_block(d: &Doc<'_>, b: &DirectBlock, has_position: bool) -> Value {
    let mut j = Map::new();
    j.insert("speaker_labels".into(), d.texts(b.labels));
    j.insert("pack_format_id".into(), d.text(b.pack_id));
    j.insert("has_position".into(), Value::from(has_position));
    if has_position {
        j.insert("azimuth".into(), Value::from(b.azimuth));
        j.insert("elevation".into(), Value::from(b.elevation));
        j.insert("distance".into(), Value::from(b.distance));
    }
    for (bit, key, value) in [
        (1, "azimuth_min", b.azimuth_min),
        (2, "azimuth_max", b.azimuth_max),
        (4, "elevation_min", b.elevation_min),
        (8, "elevation_max", b.elevation_max),
        (16, "distance_min", b.distance_min),
        (32, "distance_max", b.distance_max),
    ] {
        if b.bounds_present & bit != 0 {
            j.insert(key.into(), Value::from(value));
        }
    }
    j.insert("gain".into(), Value::from(b.gain));
    j.insert("head_locked".into(), Value::from(b.head_locked != 0));
    if b.low_pass_present != 0 {
        j.insert("low_pass_hz".into(), Value::from(b.low_pass));
    }
    j.insert("start_sample".into(), Value::from(b.start));
    end_sample(&mut j, b.end);
    Value::Object(j)
}

fn object(d: &Doc<'_>, scene: &RenderScene, index: usize) -> Value {
    let s = d.s;
    let o = &s.objects[index];
    let mut j = Map::new();
    j.insert("id".into(), d.text(o.id));
    j.insert("name".into(), d.text(o.name));
    j.insert("gain".into(), Value::from(o.gain));
    j.insert(
        "mute".into(),
        Value::from(scene.overlay.objects[index].mute),
    );
    j.insert("head_locked".into(), Value::from(o.head_locked != 0));
    end_sample(&mut j, o.end);
    j.insert("labels".into(), d.texts(o.labels));
    if o.importance_present != 0 {
        j.insert("importance".into(), Value::from(o.importance));
    }
    if o.dialogue_present != 0 {
        j.insert("dialogue_id".into(), Value::from(o.dialogue));
    }
    if o.offset_present != 0 {
        j.insert("position_offset".into(), adm_position(&o.position_offset));
    }
    let tracks = s.tracks[o.tracks.offset..o.tracks.offset + o.tracks.len]
        .iter()
        .map(|t| {
            let mut tj = Map::new();
            tj.insert("track_uid".into(), d.text(t.uid));
            if t.channel >= 0 {
                tj.insert("channel_index".into(), Value::from(t.channel as u16 as u32));
            }
            let blocks = &s.object_blocks[t.blocks.offset..t.blocks.offset + t.blocks.len];
            tj.insert(
                "object_blocks".into(),
                Value::Array(blocks.iter().map(object_block).collect()),
            );
            let ds = (t.ds_blocks.offset..t.ds_blocks.offset + t.ds_blocks.len)
                .map(|bi| {
                    direct_block(
                        d,
                        &s.direct_blocks[bi],
                        scene.overlay.direct_blocks[bi].has_position,
                    )
                })
                .collect();
            tj.insert("ds_blocks".into(), Value::Array(ds));
            Value::Object(tj)
        })
        .collect();
    j.insert("tracks".into(), Value::Array(tracks));
    Value::Object(j)
}

fn programme(d: &Doc<'_>, p: &Programme) -> Value {
    let mut j = Map::new();
    j.insert("id".into(), d.text(p.id));
    j.insert("name".into(), d.text(p.name));
    d.optional_text(&mut j, "language", p.language);
    j.insert("labels".into(), d.texts(p.labels));
    j.insert("start_sample".into(), Value::from(p.start));
    if p.end_present != 0 {
        j.insert("end_sample".into(), Value::from(p.end));
    }
    j.insert(
        "has_reference_screen".into(),
        Value::from(p.reference_screen != 0),
    );
    if p.reference_screen != 0 {
        let g = &p.screen_geometry;
        let mut screen = Map::new();
        screen.insert(
            "coordinates".into(),
            Value::from(if g.cartesian != 0 {
                "cartesian"
            } else {
                "polar"
            }),
        );
        screen.insert(
            "centre".into(),
            Value::Array(g.centre.iter().map(|&c| Value::from(c)).collect()),
        );
        screen.insert("width".into(), Value::from(g.width));
        screen.insert("aspect_ratio".into(), Value::from(g.aspect_ratio));
        j.insert("reference_screen".into(), Value::Object(screen));
    }
    if p.loudness.present != 0 {
        j.insert("loudness".into(), d.loudness(&p.loudness));
    }
    j.insert("content_ids".into(), d.texts(p.content_ids));
    Value::Object(j)
}

fn content(d: &Doc<'_>, c: &Content) -> Value {
    let mut j = Map::new();
    j.insert("id".into(), d.text(c.id));
    j.insert("name".into(), d.text(c.name));
    d.optional_text(&mut j, "language", c.language);
    j.insert("labels".into(), d.texts(c.labels));
    d.optional_text(&mut j, "dialogue_kind", c.dialogue_kind);
    d.optional_text(&mut j, "content_kind", c.content_kind);
    if c.loudness.present != 0 {
        j.insert("loudness".into(), d.loudness(&c.loudness));
    }
    j.insert("object_ids".into(), d.texts(c.object_ids));
    Value::Object(j)
}

fn hoa(d: &Doc<'_>, h: &Hoa) -> Value {
    let s = d.s;
    let mut j = Map::new();
    j.insert("object_id".into(), d.text(h.object_id));
    j.insert("pack_format_id".into(), d.text(h.pack_id));
    j.insert("normalization".into(), d.text(h.normalization));
    j.insert("nfc_ref_dist".into(), Value::from(h.nfc_ref_dist));
    j.insert("screen_ref".into(), Value::from(h.screen_ref != 0));
    j.insert("gain".into(), Value::from(h.gain));
    j.insert("mute".into(), Value::from(h.mute != 0));
    j.insert("head_locked".into(), Value::from(h.head_locked != 0));
    j.insert("start_sample".into(), Value::from(h.start));
    end_sample(&mut j, h.end);
    let channels = s.hoa_channels[h.channels.offset..h.channels.offset + h.channels.len]
        .iter()
        .map(|c| {
            let mut cj = Map::new();
            if c.channel >= 0 {
                cj.insert("channel_index".into(), Value::from(c.channel as u16 as u32));
            }
            cj.insert("track_uid".into(), d.text(c.uid));
            cj.insert("order".into(), Value::from(c.order));
            cj.insert("degree".into(), Value::from(c.degree));
            let blocks = s.hoa_blocks[c.blocks.offset..c.blocks.offset + c.blocks.len]
                .iter()
                .map(|b| {
                    let mut bj = Map::new();
                    bj.insert("gain".into(), Value::from(b.gain));
                    bj.insert("head_locked".into(), Value::from(b.head_locked != 0));
                    bj.insert("start_sample".into(), Value::from(b.start));
                    end_sample(&mut bj, b.end);
                    Value::Object(bj)
                })
                .collect();
            cj.insert("blocks".into(), Value::Array(blocks));
            Value::Object(cj)
        })
        .collect();
    j.insert("channels".into(), Value::Array(channels));
    Value::Object(j)
}

/// The inspect document of an imported (policy-free) scene; `path` is the input path as given.
pub fn inspect_json(scene: &RenderScene, path: &str) -> String {
    let s = &scene.snapshot;
    let d = Doc { s };
    let info = &scene.info;
    let mut root = Map::new();
    root.insert("schema".into(), Value::from("mradm.scene-inspect"));
    root.insert("schema_version".into(), Value::from(1u32));
    let bed = info.source_kind == SourceKind::ChannelBed;
    root.insert(
        "source_kind".into(),
        Value::from(if bed { "channel_bed" } else { "adm" }),
    );
    if bed {
        root.insert(
            "input_layout".into(),
            Value::from(info.input_layout.as_str()),
        );
        root.insert(
            "input_channel_labels".into(),
            Value::Array(
                info.input_channel_labels
                    .iter()
                    .map(|l| Value::from(l.as_str()))
                    .collect(),
            ),
        );
    }
    let mut file = Map::new();
    file.insert("path".into(), Value::from(path));
    file.insert("sample_rate".into(), Value::from(info.sample_rate));
    file.insert("num_channels".into(), Value::from(u32::from(info.channels)));
    file.insert("num_frames".into(), Value::from(info.frames));
    if info.sample_rate > 0 && info.frames > 0 {
        file.insert(
            "duration_seconds".into(),
            Value::from(info.frames as f64 / f64::from(info.sample_rate)),
        );
    }
    root.insert("file".into(), Value::Object(file));
    root.insert(
        "programmes".into(),
        Value::Array(s.programmes.iter().map(|p| programme(&d, p)).collect()),
    );
    root.insert(
        "contents".into(),
        Value::Array(s.contents.iter().map(|c| content(&d, c)).collect()),
    );
    root.insert(
        "objects".into(),
        Value::Array((0..s.objects.len()).map(|i| object(&d, scene, i)).collect()),
    );
    root.insert(
        "hoa_tracks".into(),
        Value::Array(s.hoa.iter().map(|h| hoa(&d, h)).collect()),
    );
    root.insert(
        "import_warnings".into(),
        Value::Array(s.warnings.iter().map(|&w| d.text(w)).collect()),
    );
    mradm_json::dump(&Value::Object(root), Some(2))
}
