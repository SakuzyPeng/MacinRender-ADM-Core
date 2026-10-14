//! Numeric evaluation of resolved semantic policies.
//!
//! C++ resolves identity matching, merges rules, applies the DirectSpeakers filters and every
//! boolean field (mute, headLocked, channelLock, jumpPosition). This module owns the arithmetic
//! those rules imply: dB conversion, scale products, clamps, polar position edits and sample
//! limits. File rendering and Scene streaming share it, so a policy evaluates to the same bits on
//! both paths (ADR 0018). Every expression keeps the evaluation order of the original C++.
use crate::scene_math::{self as math, Vec3};
use std::cmp::Ordering;

/// An optional parameter: `present == 0` means the policy omits it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct PolicyValue {
    pub present: u32,
    pub value: f32,
}

impl PolicyValue {
    pub const fn some(value: f32) -> Self {
        Self { present: 1, value }
    }
    fn get(self) -> Option<f32> {
        (self.present != 0).then_some(self.value)
    }
    fn or(self, default: f32) -> f32 {
        self.get().unwrap_or(default)
    }
}

/// `GainPolicy` without `mute`, which the caller applies.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct PolicyGain {
    pub scale: PolicyValue,
    pub gain_db: PolicyValue,
}

/// `PositionPolicy`; `present == 0` when the rule has no position section.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct PolicyPosition {
    pub present: u32,
    pub azimuth: PolicyValue,
    pub elevation: PolicyValue,
    pub distance: PolicyValue,
    /// An offset section adds every component, absent components as `+0.0`.
    pub has_offset: u32,
    pub offset_azimuth: PolicyValue,
    pub offset_elevation: PolicyValue,
    pub offset_distance: PolicyValue,
    pub lock_azimuth: PolicyValue,
    pub lock_elevation: PolicyValue,
}

/// Section modes of [`PolicyObject`].
pub const SECTION_ABSENT: u32 = 0;
pub const SECTION_DISABLED: u32 = 1;
pub const SECTION_SCALED: u32 = 2;

/// Numeric part of the merged per-object override applied to every Objects block.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct PolicyObject {
    pub diffuse: u32,
    pub diffuse_scale: PolicyValue,
    pub diffuse_max: PolicyValue,
    pub extent: u32,
    pub extent_scale: PolicyValue,
    pub width_scale: PolicyValue,
    pub height_scale: PolicyValue,
    pub depth_scale: PolicyValue,
    pub extent_max: PolicyValue,
    pub divergence: u32,
    pub divergence_scale: PolicyValue,
    pub divergence_range_scale: PolicyValue,
    pub divergence_max_range: PolicyValue,
    pub position: PolicyPosition,
    /// Interpolation limit in milliseconds; zero leaves the length unchanged.
    pub interpolation_max_ms: u32,
}

/// Numeric fields of an Objects block. `position` is x/y/z when `cartesian != 0`, otherwise
/// azimuth/elevation/distance.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct PolicyBlock {
    pub cartesian: u32,
    pub position: [f32; 3],
    pub diffuse: f32,
    pub width: f32,
    pub height: f32,
    pub depth: f32,
    pub divergence: f32,
    pub divergence_azimuth_range: f32,
    pub divergence_position_range: f32,
    pub has_interpolation: u32,
    pub interpolation_samples: u64,
}

/// A DirectSpeakers rule whose label/LFE filters already matched.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct PolicyDirect {
    pub gain: PolicyGain,
    /// `gain.mute == true`: silence the block instead of scaling it.
    pub mute: u32,
    pub position: PolicyPosition,
}

/// Numeric fields of a DirectSpeakers block; `polar` is azimuth/elevation/distance.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct PolicyDirectBlock {
    pub gain: f32,
    pub user_gain_multiplier: f32,
    pub has_position: u32,
    pub polar: [f32; 3],
    /// Set when a rule re-aimed the block.
    pub user_position_override: u32,
}

/// The only f32 dB-to-linear conversion of the policy path.
pub fn db_to_gain(db: f32) -> f32 {
    10f32.powf(db / 20.)
}

/// Scale first, then the dB multiplier, each rounded separately.
pub fn apply_gain(rule: &PolicyGain, mut value: f32) -> f32 {
    if let Some(scale) = rule.scale.get() {
        value *= scale;
    }
    if let Some(db) = rule.gain_db.get() {
        value *= db_to_gain(db);
    }
    value
}

/// True when the section would move a position: any absolute or lock field, or a non-zero
/// offset component. A neutral template's zero offsets are inert.
pub fn position_changes(rule: &PolicyPosition) -> bool {
    // NaN is neither above nor below zero, as in the original `> 0 || < 0` test.
    let nonzero = |v: PolicyValue| {
        v.get()
            .is_some_and(|v| matches!(v.partial_cmp(&0.), Some(Ordering::Less | Ordering::Greater)))
    };
    rule.present != 0
        && (rule.azimuth.present != 0
            || rule.elevation.present != 0
            || rule.distance.present != 0
            || rule.lock_azimuth.present != 0
            || rule.lock_elevation.present != 0
            || rule.has_offset != 0
                && (nonzero(rule.offset_azimuth)
                    || nonzero(rule.offset_elevation)
                    || nonzero(rule.offset_distance)))
}

/// Absolute overwrite, offset add, lock (wins), then wrap azimuth, clamp elevation and keep
/// the distance non-negative. Callers check [`position_changes`] first.
pub fn polar_position(
    rule: &PolicyPosition,
    [mut azimuth, mut elevation, mut distance]: Vec3,
) -> Vec3 {
    azimuth = rule.azimuth.or(azimuth);
    elevation = rule.elevation.or(elevation);
    distance = rule.distance.or(distance);
    if rule.has_offset != 0 {
        azimuth += rule.offset_azimuth.or(0.);
        elevation += rule.offset_elevation.or(0.);
        distance += rule.offset_distance.or(0.);
    }
    azimuth = rule.lock_azimuth.or(azimuth);
    elevation = rule.lock_elevation.or(elevation);
    [
        math::wrap(azimuth),
        math::clamp(elevation, -90., 90.),
        math::max(0., distance),
    ]
}

/// Diffuse, extent, divergence, position and the interpolation limit of one Objects block.
pub fn apply_object(rule: &PolicyObject, sample_rate: u32, block: &mut PolicyBlock) {
    match rule.diffuse {
        SECTION_DISABLED => block.diffuse = 0.,
        SECTION_SCALED => {
            block.diffuse *= rule.diffuse_scale.or(1.);
            block.diffuse = math::clamp(block.diffuse, 0., rule.diffuse_max.or(1.));
        }
        _ => {}
    }
    match rule.extent {
        SECTION_DISABLED => {
            block.width = 0.;
            block.height = 0.;
            block.depth = 0.;
        }
        SECTION_SCALED => {
            let scale = rule.extent_scale.or(1.);
            block.width *= scale * rule.width_scale.or(1.);
            block.height *= scale * rule.height_scale.or(1.);
            block.depth *= scale * rule.depth_scale.or(1.);
            let max = rule.extent_max.or(1.);
            block.width = math::clamp(block.width, 0., max);
            block.height = math::clamp(block.height, 0., max);
            block.depth = math::clamp(block.depth, 0., max);
        }
        _ => {}
    }
    match rule.divergence {
        SECTION_DISABLED => block.divergence = 0.,
        SECTION_SCALED => {
            block.divergence = math::clamp(block.divergence * rule.divergence_scale.or(1.), 0., 1.);
            let range_scale = rule.divergence_range_scale.or(1.);
            let max_range = rule.divergence_max_range.or(120.);
            block.divergence_azimuth_range =
                math::clamp(block.divergence_azimuth_range * range_scale, 0., max_range);
            block.divergence_position_range =
                math::max(0., block.divergence_position_range * range_scale);
        }
        _ => {}
    }
    if position_changes(&rule.position) {
        let polar = if block.cartesian != 0 {
            math::cartesian_to_polar(block.position)
        } else {
            block.position
        };
        block.position = polar_position(&rule.position, polar);
        block.cartesian = 0;
    }
    if rule.interpolation_max_ms > 0 && block.has_interpolation != 0 {
        let max_samples = u64::from(sample_rate) * u64::from(rule.interpolation_max_ms) / 1000;
        block.interpolation_samples = block.interpolation_samples.min(max_samples);
    }
}

/// Gain and re-aim of one DirectSpeakers block.
pub fn apply_direct(rule: &PolicyDirect, block: &mut PolicyDirectBlock) {
    block.gain = if rule.mute != 0 {
        0.
    } else {
        apply_gain(&rule.gain, block.gain)
    };
    block.user_gain_multiplier = apply_gain(&rule.gain, block.user_gain_multiplier);
    if position_changes(&rule.position) {
        block.user_position_override = 1;
        block.polar = polar_position(&rule.position, block.polar);
        block.has_position = 1;
    }
}

/// Final Scene state values: the content level times the object level, and a polar position
/// as Cartesian coordinates.
pub fn scene_level(block_gain: f32, object_gain: f32) -> f32 {
    block_gain * object_gain
}

pub fn scene_cartesian([azimuth, elevation, distance]: Vec3) -> Vec3 {
    let [x, y, z] = math::direction(azimuth, elevation);
    [x * distance, y * distance, z * distance]
}

#[cfg(test)]
mod tests {
    use super::*;

    fn position() -> PolicyPosition {
        PolicyPosition {
            present: 1,
            ..PolicyPosition::default()
        }
    }

    #[test]
    fn gain_scales_before_the_db_multiplier() {
        let rule = PolicyGain {
            scale: PolicyValue::some(0.3),
            gain_db: PolicyValue::some(-7.5),
        };
        let expected = 0.8f32 * 0.3 * 10f32.powf(-7.5 / 20.);
        assert_eq!(apply_gain(&rule, 0.8).to_bits(), expected.to_bits());
        assert_eq!(
            apply_gain(&PolicyGain::default(), -0.0).to_bits(),
            (-0.0f32).to_bits()
        );
    }

    #[test]
    fn zero_offsets_are_inert_but_an_offset_section_still_adds() {
        let mut rule = position();
        rule.has_offset = 1;
        rule.offset_azimuth = PolicyValue::some(0.);
        assert!(!position_changes(&rule));
        rule.offset_elevation = PolicyValue::some(-0.5);
        assert!(position_changes(&rule));
        // -0 + +0 is +0: absent offset components still participate in the sum.
        let [az, el, dist] = polar_position(&rule, [-0., 10., 2.]);
        assert_eq!((az.to_bits(), el, dist), (0f32.to_bits(), 9.5, 2.));
        assert!(!position_changes(&PolicyPosition {
            present: 0,
            azimuth: PolicyValue::some(5.),
            ..PolicyPosition::default()
        }));
    }

    #[test]
    fn locks_win_and_results_are_wrapped_and_clamped() {
        let mut rule = position();
        rule.azimuth = PolicyValue::some(170.);
        rule.has_offset = 1;
        rule.offset_azimuth = PolicyValue::some(30.);
        rule.offset_distance = PolicyValue::some(-5.);
        rule.lock_elevation = PolicyValue::some(120.);
        assert_eq!(polar_position(&rule, [0., 0., 1.]), [-160., 90., 0.]);
    }

    #[test]
    fn objects_keep_cartesian_positions_unless_a_position_changes() {
        let mut block = PolicyBlock {
            cartesian: 1,
            position: [0.25, 0.5, 0.1],
            diffuse: 0.6,
            width: 0.5,
            divergence: 0.4,
            divergence_azimuth_range: 90.,
            divergence_position_range: 0.3,
            has_interpolation: 1,
            interpolation_samples: 48_000,
            ..PolicyBlock::default()
        };
        let mut rule = PolicyObject {
            diffuse: SECTION_SCALED,
            diffuse_scale: PolicyValue::some(2.),
            diffuse_max: PolicyValue::some(0.9),
            extent: SECTION_SCALED,
            width_scale: PolicyValue::some(3.),
            divergence: SECTION_SCALED,
            divergence_range_scale: PolicyValue::some(2.),
            interpolation_max_ms: 250,
            ..PolicyObject::default()
        };
        apply_object(&rule, 48_000, &mut block);
        assert_eq!(block.cartesian, 1);
        assert_eq!(block.position, [0.25, 0.5, 0.1]);
        assert_eq!(
            (block.diffuse, block.width, block.divergence_azimuth_range),
            (0.9, 1., 120.)
        );
        assert_eq!(block.divergence_position_range, 0.3f32 * 2.);
        assert_eq!(block.interpolation_samples, 12_000);

        rule.position = position();
        rule.position.distance = PolicyValue::some(2.);
        apply_object(&rule, 48_000, &mut block);
        assert_eq!(block.cartesian, 0);
        let polar = math::cartesian_to_polar([0.25, 0.5, 0.1]);
        assert_eq!(block.position, [math::wrap(polar[0]), polar[1], 2.]);

        rule.divergence = SECTION_DISABLED;
        rule.extent = SECTION_DISABLED;
        apply_object(&rule, 48_000, &mut block);
        assert_eq!((block.divergence, block.width), (0., 0.));
    }

    #[test]
    fn muted_direct_rules_still_scale_the_user_multiplier() {
        let mut block = PolicyDirectBlock {
            gain: 0.5,
            user_gain_multiplier: 1.,
            polar: [30., 0., 1.],
            ..PolicyDirectBlock::default()
        };
        let mut rule = PolicyDirect {
            gain: PolicyGain {
                scale: PolicyValue::some(0.5),
                gain_db: PolicyValue::default(),
            },
            mute: 1,
            position: position(),
        };
        apply_direct(&rule, &mut block);
        assert_eq!((block.gain, block.user_gain_multiplier), (0., 0.5));
        assert_eq!((block.has_position, block.user_position_override), (0, 0));
        rule.mute = 0;
        rule.position.lock_azimuth = PolicyValue::some(-110.);
        block.gain = 0.5;
        apply_direct(&rule, &mut block);
        assert_eq!(block.gain, 0.25);
        assert_eq!(block.polar, [-110., 0., 1.]);
        assert_eq!((block.has_position, block.user_position_override), (1, 1));
    }

    #[test]
    fn scene_cartesian_scales_the_unit_direction() {
        let [x, y, z] = math::direction(-30., 15.);
        assert_eq!(
            scene_cartesian([-30., 15., 2.5]),
            [x * 2.5, y * 2.5, z * 2.5]
        );
    }
}
