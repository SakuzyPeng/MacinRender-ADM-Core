//! The render scene as the loudspeaker backends read it (PCM consistency 4.5): the blocks in
//! effect and the Objects block preparation (position offset, channel lock, divergence), moved
//! from the C++ `render_common` with the same arithmetic. Records are read as the C++ `AdmScene`
//! copied them; the effective object mute and DirectSpeakers position come from the overlay.
use mradm_adm::records::{AdmPosition, Object, ObjectBlock, Span};
use mradm_dsp::scene_math::{self as math, CloudPoint, Speaker};

/// The records of one span.
pub fn span<T>(values: &[T], s: Span) -> &[T] {
    &values[s.offset..s.offset + s.len]
}

/// Indices of the blocks in effect: ordered by start (stable), and of the blocks sharing a start
/// only the last in document order, for its entire interval even when it ends before its peers.
pub fn in_effect<T>(blocks: &[T], start: impl Fn(&T) -> u64) -> Vec<usize> {
    let mut order: Vec<usize> = (0..blocks.len()).collect();
    order.sort_by_key(|&i| start(&blocks[i]));
    let mut result: Vec<usize> = Vec::with_capacity(order.len());
    for i in order {
        match result.last_mut() {
            Some(last) if start(&blocks[*last]) == start(&blocks[i]) => *last = i,
            _ => result.push(i),
        }
    }
    result
}

/// The coordinates the position's own system uses.
pub fn coordinates(p: &AdmPosition) -> [f32; 3] {
    if p.cartesian != 0 {
        [p.x, p.y, p.z]
    } else {
        [p.azimuth, p.elevation, p.distance]
    }
}

/// The position as polar azimuth, elevation and distance.
pub fn polar(p: &AdmPosition) -> [f32; 3] {
    if p.cartesian != 0 {
        math::cartesian_to_polar([p.x, p.y, p.z])
    } else {
        [p.azimuth, p.elevation, p.distance]
    }
}

fn polar_position([azimuth, elevation, distance]: [f32; 3]) -> AdmPosition {
    AdmPosition {
        cartesian: 0,
        azimuth,
        elevation,
        distance,
        ..AdmPosition::default()
    }
}

/// The object's positionOffset added to a block position of the same coordinate system;
/// a mismatched offset is ignored.
pub fn offset(mut p: AdmPosition, object: &Object) -> AdmPosition {
    let o = &object.position_offset;
    if object.offset_present == 0 || p.cartesian != o.cartesian {
        return p;
    }
    if p.cartesian != 0 {
        (p.x, p.y, p.z) = (p.x + o.x, p.y + o.y, p.z + o.z);
    } else {
        p.azimuth += o.azimuth;
        p.elevation = math::clamp(p.elevation + o.elevation, -90., 90.);
        p.distance = math::max(0., p.distance + o.distance);
    }
    p
}

/// channelLock: the block snapped to the nearest non-LFE speaker when it locks.
fn channel_lock(block: &ObjectBlock, speakers: &[Speaker]) -> ObjectBlock {
    let mut result = *block;
    if block.channel_lock == 0 {
        return result;
    }
    let limit = (block.max_distance_present != 0).then_some(block.max_distance);
    let p = &block.position;
    if let Some(locked) = math::channel_lock(coordinates(p), p.cartesian != 0, limit, speakers) {
        result.position = AdmPosition {
            x: p.x,
            y: p.y,
            z: p.z,
            ..polar_position(locked)
        };
    }
    result
}

/// The divergence sources of a block; a negligible divergence keeps the block unchanged.
fn divergence(block: &ObjectBlock) -> Vec<ObjectBlock> {
    let p = &block.position;
    let mut points = [CloudPoint::default(); 3];
    let count = math::divergence(
        coordinates(p),
        p.cartesian != 0,
        block.divergence,
        block.azimuth_range,
        block.position_range,
        block.gain,
        &mut points,
    );
    if count == 1 {
        return vec![*block];
    }
    let base = if p.cartesian != 0 {
        polar_position(polar(p))
    } else {
        *p
    };
    points[..count]
        .iter()
        .map(|point| ObjectBlock {
            position: AdmPosition {
                azimuth: point.azimuth,
                elevation: point.elevation,
                ..base
            },
            gain: point.weight,
            ..*block
        })
        .collect()
}

/// An Objects block ready for a backend's gains: its sources and its interval, clipped to the
/// object's end.
pub struct PreparedBlock {
    pub sources: Vec<ObjectBlock>,
    pub start: u64,
    pub end: u64,
    pub jump: bool,
    pub interpolation: Option<u64>,
}

pub const SCREEN_REF_WARNING: &str =
    "screenRef requires referenceScreen geometry; rendering block as screenRef=false";

/// Position offset, the one screenRef warning per render, channel lock and divergence.
pub fn prepare_object_block(
    raw: &ObjectBlock,
    object: &Object,
    speakers: &[Speaker],
    screen_ref_warned: &mut bool,
    warn: &mut dyn FnMut(&'static str),
) -> PreparedBlock {
    let mut block = *raw;
    block.position = offset(block.position, object);
    if block.screen_ref != 0 && !*screen_ref_warned {
        warn(SCREEN_REF_WARNING);
        *screen_ref_warned = true;
    }
    let block = channel_lock(&block, speakers);
    PreparedBlock {
        sources: divergence(&block),
        start: raw.start,
        end: raw.end.min(object.end),
        jump: raw.jump != 0,
        interpolation: (raw.interpolation_present != 0).then_some(raw.interpolation),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_last_block_at_each_start_is_in_effect() {
        let starts = [5u64, 0, 5, 3, 0];
        assert_eq!(in_effect(&starts, |s| *s), vec![4, 3, 2]);
        assert!(in_effect(&[] as &[u64], |s| *s).is_empty());
    }

    #[test]
    fn offsets_need_the_same_coordinate_system() {
        let mut object = Object {
            offset_present: 1,
            ..Object::default()
        };
        object.position_offset.elevation = 80.;
        object.position_offset.distance = -2.;
        let p = AdmPosition {
            elevation: 30.,
            distance: 1.,
            ..AdmPosition::default()
        };
        let q = offset(p, &object);
        assert_eq!((q.elevation, q.distance), (90., 0.));
        let cartesian = AdmPosition { cartesian: 1, ..p };
        assert_eq!(offset(cartesian, &object), cartesian);
    }
}
