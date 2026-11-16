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
    let (values, fields) = encode(initial, updates);
    if !values.is_empty() {
        let _ = diagnostics::process(&format!("{stage}.f32"), Words::F32(&values));
        let _ = diagnostics::process(&format!("{stage}-fields.i32"), Words::I32(&fields));
    }
}

/// The two word streams of [`states`].
fn encode(initial: &[live::Entry], updates: &[MetaUpdate]) -> (Vec<f32>, Vec<i32>) {
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
    (values, fields)
}

/// The prefix of every checkpoint of one rendered slice.
pub(super) fn slice_key(epoch: u64, generation: u64, start: i64) -> String {
    format!("scene/e{epoch}-g{generation}-s{start}")
}

#[cfg(test)]
mod tests {
    use mradm_render::live;

    /// One row: twelve f32 and fifteen i32 words, the sign of zero kept and the 64-bit element id
    /// split low then high (formerly the C++ `live_scene_trace_test`, R3 D2).
    #[test]
    fn state_rows_keep_signed_zero_and_the_whole_element_id() {
        let entry = live::Entry {
            element_id: 0x1234_5678_0000_0001,
            state: live::State {
                position: [-0., 0., 0.],
                ..live::State::default()
            },
        };
        let (values, fields) = super::encode(&[entry], &[]);
        assert_eq!((values.len(), fields.len()), (12, 15));
        assert_eq!(values[1].to_bits(), 0x8000_0000);
        assert_eq!((fields[0], fields[1], fields[2]), (0, 1, 0x1234_5678));
    }
}
