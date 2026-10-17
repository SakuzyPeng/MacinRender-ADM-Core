//! Version-1 render-scene checkpoint (PCM consistency 4.1, emitted here from 4.3): the canonical
//! word stream of the public `AdmScene` this scene becomes, specified in
//! docs/architecture/RUST_INVERSION_BASELINE.md. It follows the C++ view mapping field by field,
//! including the user state a semantic policy keeps beside the records (user levels, the
//! effective mute and position flags). The input path and warning text are excluded; only the
//! warning count is recorded.
use crate::import::{RenderScene, SourceKind};
use crate::policy::{Overlay, UserLevel};
use mradm_adm::{Snapshot, records::*};

struct Words<'a> {
    s: &'a Snapshot,
    overlay: &'a Overlay,
    words: Vec<i32>,
}

impl Words<'_> {
    fn u32(&mut self, v: u32) {
        self.words.push(v as i32);
    }
    fn u64(&mut self, v: u64) {
        self.u32(v as u32);
        self.u32((v >> 32) as u32);
    }
    fn boolean(&mut self, v: bool) {
        self.u32(u32::from(v));
    }
    fn flag(&mut self, v: u32) {
        self.boolean(v != 0);
    }
    fn f32(&mut self, v: f32) {
        self.u32(v.to_bits());
    }
    fn f64(&mut self, v: f64) {
        self.u64(v.to_bits());
    }
    fn text(&mut self, v: &str) {
        self.u32(v.len() as u32);
        for chunk in v.as_bytes().chunks(4) {
            let mut word = [0; 4];
            word[..chunk.len()].copy_from_slice(chunk);
            self.u32(u32::from_le_bytes(word));
        }
    }
    fn texts<S: AsRef<str>>(&mut self, values: &[S]) {
        self.u32(values.len() as u32);
        for value in values {
            self.text(value.as_ref());
        }
    }
    fn string(&mut self, id: u32) {
        let s = self.s;
        self.text(&s.strings[id as usize]);
    }
    fn optional_string(&mut self, id: u32) {
        self.boolean(id != NONE);
        if id != NONE {
            self.string(id);
        }
    }
    fn strings(&mut self, span: Span) {
        let s = self.s;
        let ids = &s.indices[span.offset..span.offset + span.len];
        self.u32(ids.len() as u32);
        for &id in ids {
            self.string(id);
        }
    }
    fn optional_f32(&mut self, present: bool, v: f32) {
        self.boolean(present);
        if present {
            self.f32(v);
        }
    }
    fn optional_u64(&mut self, present: u32, v: u64) {
        self.flag(present);
        if present != 0 {
            self.u64(v);
        }
    }
    fn position(&mut self, p: &AdmPosition) {
        self.flag(p.cartesian);
        for v in [p.azimuth, p.elevation, p.distance, p.x, p.y, p.z] {
            self.f32(v);
        }
    }
    fn gain_source(&mut self, g: &Gain) {
        self.flag(g.present);
        self.flag(g.decibels);
        self.f64(g.value);
        self.f32(g.linear);
    }
    fn block_source(&mut self, b: &BlockSource) {
        self.gain_source(&b.gain);
        self.flag(b.rtime_present);
        self.u64(b.rtime_samples);
        self.optional_u64(b.duration_present, b.duration_samples);
    }
    fn loudness(&mut self, l: &Loudness) {
        self.flag(l.present);
        if l.present == 0 {
            return;
        }
        for (bit, v) in [
            (1, l.integrated),
            (2, l.true_peak),
            (4, l.range),
            (8, l.momentary),
            (16, l.short_term),
            (32, l.dialogue),
        ] {
            self.optional_f32(l.fields & bit != 0, v);
        }
        self.optional_string(l.method);
    }
    fn user_level(&mut self, user: UserLevel) {
        self.f32(user.gain_multiplier);
        self.boolean(user.mute.is_some());
        if let Some(mute) = user.mute {
            self.boolean(mute);
        }
    }
    fn object_block(&mut self, b: &ObjectBlock) {
        self.position(&b.position);
        for v in [b.gain, b.diffuse, b.width, b.height, b.depth] {
            self.f32(v);
        }
        self.u64(b.start);
        self.u64(b.end);
        self.flag(b.jump);
        self.optional_u64(b.interpolation_present, b.interpolation);
        self.flag(b.channel_lock);
        self.optional_f32(b.max_distance_present != 0, b.max_distance);
        self.f32(b.divergence);
        self.f32(b.azimuth_range);
        self.f32(b.position_range);
        self.flag(b.screen_ref);
        self.flag(b.head_locked);
        // Objects blocks always carry their ADM source.
        self.boolean(true);
        self.block_source(&b.source);
    }
    fn direct_block(&mut self, index: usize, provenance: bool) {
        let s = self.s;
        let b = &s.direct_blocks[index];
        let state = self.overlay.direct_blocks[index];
        self.strings(b.labels);
        self.string(b.pack_id);
        self.f32(b.azimuth);
        self.f32(b.elevation);
        self.f32(b.distance);
        self.boolean(state.has_position);
        for (bit, v) in [
            (1, b.azimuth_min),
            (2, b.azimuth_max),
            (4, b.elevation_min),
            (8, b.elevation_max),
            (16, b.distance_min),
            (32, b.distance_max),
        ] {
            self.optional_f32(b.bounds_present & bit != 0, v);
        }
        self.f32(b.gain);
        self.optional_f32(b.low_pass_present != 0, b.low_pass);
        self.u64(b.start);
        self.u64(b.end);
        self.flag(b.head_locked);
        self.boolean(provenance);
        if provenance {
            self.block_source(&b.source);
        }
        // The ADM source position follows the authored position, not a policy re-aim.
        let source_position = provenance && b.has_position != 0;
        self.boolean(source_position);
        if source_position {
            self.position(&b.position);
        }
        self.user_level(state.user);
        self.boolean(state.position_override);
    }
    fn object(&mut self, index: usize, provenance: bool) {
        let s = self.s;
        let o = &s.objects[index];
        let state = self.overlay.objects[index];
        self.string(o.id);
        self.string(o.name);
        self.f32(o.gain);
        self.boolean(state.mute);
        self.u64(o.end);
        self.flag(o.offset_present);
        if o.offset_present != 0 {
            self.position(&o.position_offset);
        }
        let tracks = &s.tracks[o.tracks.offset..o.tracks.offset + o.tracks.len];
        self.u32(tracks.len() as u32);
        for t in tracks {
            self.boolean(t.channel >= 0);
            if t.channel >= 0 {
                self.u32(t.channel as u32);
            }
            self.string(t.uid);
            let blocks = &s.object_blocks[t.blocks.offset..t.blocks.offset + t.blocks.len];
            self.u32(blocks.len() as u32);
            for b in blocks {
                self.object_block(b);
            }
            self.u32(t.ds_blocks.len as u32);
            for b in t.ds_blocks.offset..t.ds_blocks.offset + t.ds_blocks.len {
                self.direct_block(b, provenance);
            }
        }
        self.strings(o.labels);
        self.boolean(o.importance_present != 0);
        if o.importance_present != 0 {
            self.u32(o.importance as u32);
        }
        self.boolean(o.dialogue_present != 0);
        if o.dialogue_present != 0 {
            self.u32(o.dialogue);
        }
        self.flag(o.head_locked);
        self.boolean(provenance);
        if provenance {
            self.gain_source(&o.source_gain);
            self.flag(o.mute_present);
            self.flag(o.mute);
            self.flag(o.start_present);
            self.u64(o.start);
            self.u64(o.absolute_start);
            self.optional_u64(o.duration_present, o.duration);
            self.strings(o.child_objects);
            self.flag(o.has_parent);
        }
        self.user_level(state.user);
    }
}

pub fn encode(scene: &RenderScene) -> Vec<i32> {
    let s = &scene.snapshot;
    let info = &scene.info;
    let provenance = info.source_kind == SourceKind::Adm;
    let mut w = Words {
        s,
        overlay: &scene.overlay,
        words: Vec::new(),
    };
    w.u32(1); // encoding version
    w.u32(info.sample_rate);
    w.u32(u32::from(info.channels));
    w.u64(info.frames);
    w.u32(match info.source_kind {
        SourceKind::Adm => 0,
        SourceKind::ChannelBed => 1,
    });
    w.text(&info.input_layout);
    w.texts(&info.input_channel_labels);
    w.u32(s.programmes.len() as u32);
    for p in &s.programmes {
        w.string(p.id);
        w.string(p.name);
        w.strings(p.content_ids);
        w.optional_string(p.language);
        w.strings(p.labels);
        w.u64(p.start);
        w.optional_u64(p.end_present, p.end);
        w.loudness(&p.loudness);
        w.flag(p.reference_screen);
        w.flag(p.reference_screen);
        if p.reference_screen != 0 {
            let g = &p.screen_geometry;
            w.flag(g.cartesian);
            w.f64(g.aspect_ratio);
            for v in g.centre {
                w.f64(v);
            }
            w.f64(g.width);
        }
    }
    w.u32(s.contents.len() as u32);
    for c in &s.contents {
        w.string(c.id);
        w.string(c.name);
        w.strings(c.object_ids);
        w.optional_string(c.language);
        w.strings(c.labels);
        w.loudness(&c.loudness);
        w.optional_string(c.dialogue_kind);
        w.optional_string(c.content_kind);
    }
    w.u32(s.objects.len() as u32);
    for o in 0..s.objects.len() {
        w.object(o, provenance);
    }
    w.u32(s.hoa.len() as u32);
    for h in &s.hoa {
        w.string(h.object_id);
        w.string(h.pack_id);
        w.string(h.normalization);
        w.f64(h.nfc_ref_dist);
        w.flag(h.screen_ref);
        w.f32(h.gain);
        w.flag(h.mute);
        w.u64(h.start);
        w.u64(h.end);
        let channels = &s.hoa_channels[h.channels.offset..h.channels.offset + h.channels.len];
        w.u32(channels.len() as u32);
        for c in channels {
            w.boolean(c.channel >= 0);
            if c.channel >= 0 {
                w.u32(c.channel as u32);
            }
            w.string(c.uid);
            w.u32(c.order as u32);
            w.u32(c.degree as u32);
            let blocks = &s.hoa_blocks[c.blocks.offset..c.blocks.offset + c.blocks.len];
            w.u32(blocks.len() as u32);
            for b in blocks {
                w.f32(b.gain);
                w.u64(b.start);
                w.u64(b.end);
                w.flag(b.head_locked);
            }
        }
        w.flag(h.head_locked);
    }
    w.u32(s.warnings.len() as u32);
    w.words
}

/// Writes `render-scene/offline.<stage>.i32` (diagnostic builds only; a no-op otherwise).
pub fn dump(stage: &str, scene: &RenderScene) -> std::io::Result<()> {
    let words = encode(scene);
    mradm_dsp::diagnostics::process(
        &format!("render-scene/offline.{stage}.i32"),
        mradm_dsp::diagnostics::Words::I32(&words),
    )
}
