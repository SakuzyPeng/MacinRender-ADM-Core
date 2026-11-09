//! The Scene stream checkpoints (formerly C++ `live_scene_trace.h`): written only in diagnostic
//! builds, with the C++ names and words. States are two word streams: twelve f32 per row and
//! fifteen i32 per row (kind 0 = initial state / 1 = update, element id low/high, offset, ramp,
//! jump, changed, cleared, stream order, then the six state flags).
use super::MetaUpdate;
use mradm_dsp::diagnostics::{self, Words};
use mradm_render::live;

pub(super) use diagnostics::ENABLED;

pub(super) fn f32(name: &str, values: &[f32]) {
    let _ = diagnostics::process(name, Words::F32(values));
}

fn low(value: u64) -> i32 {
    value as u32 as i32
}

fn high(value: u64) -> i32 {
    (value >> 32) as u32 as i32
}

fn append_state(values: &mut Vec<f32>, fields: &mut Vec<i32>, s: &live::State) {
    values.extend([
        s.linear_gain,
        s.position[0],
        s.position[1],
        s.position[2],
        s.width,
        s.height,
        s.depth,
        s.diffuse,
        s.divergence,
        s.divergence_azimuth_range,
        s.divergence_position_range,
        s.channel_lock_max_distance.unwrap_or(0.),
    ]);
    fields.extend([
        low(s.valid_fields),
        i32::from(s.active),
        i32::from(s.channel_lock),
        i32::from(s.screen_reference),
        i32::from(s.head_locked),
        i32::from(s.channel_lock_max_distance.is_some()),
    ]);
}

/// `<stage>.f32` and `<stage>-fields.i32`; nothing for an empty slice.
pub(super) fn states(stage: &str, initial: &[live::Entry], updates: &[MetaUpdate]) {
    if !ENABLED {
        return;
    }
    let mut values = Vec::new();
    let mut fields = Vec::new();
    for entry in initial {
        fields.extend([
            0,
            low(entry.element_id),
            high(entry.element_id),
            0,
            0,
            0,
            0,
            0,
            0,
        ]);
        append_state(&mut values, &mut fields, &entry.state);
    }
    for update in updates {
        let u = &update.update;
        fields.extend([
            1,
            low(u.element_id),
            high(u.element_id),
            u.offset as i32,
            u.ramp as i32,
            i32::from(u.jump),
            low(u.changed),
            low(u.cleared),
            low(update.order),
        ]);
        append_state(&mut values, &mut fields, &u.state);
    }
    if !values.is_empty() {
        let _ = diagnostics::process(&format!("{stage}.f32"), Words::F32(&values));
        let _ = diagnostics::process(&format!("{stage}-fields.i32"), Words::I32(&fields));
    }
}

/// The prefix of every checkpoint of one rendered slice.
pub(super) fn slice_key(epoch: u64, generation: u64, start: i64) -> String {
    format!("scene/e{epoch}-g{generation}-s{start}")
}
