//! Seconds to sample frames (PCM consistency 3.6, audit S06).
//!
//! The output trim, the preview window and Monitor seek / loop points all name a time in seconds.
//! The trim used `llround(seconds * rate)` while Monitor truncated toward zero, so the same time
//! could land one frame apart. Every entry now takes the frame nearest to the binary64 product
//! `seconds * rate`, ties away from zero. The product is a single correctly rounded multiplication
//! and the rounding is exact, so the result does not depend on the platform.

/// The frame nearest to `seconds * sample_rate`, halfway cases away from zero. `None` for a
/// non-finite or negative time or a zero rate; a product of 2^64 or more saturates to `u64::MAX`.
pub fn seconds_to_frame(seconds: f64, sample_rate: u32) -> Option<u64> {
    if !seconds.is_finite() || seconds < 0.0 || sample_rate == 0 {
        return None;
    }
    // `as` saturates out-of-range floats; -0.0 becomes 0.
    Some(libm::round(seconds * f64::from(sample_rate)) as u64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rounds_to_the_nearest_frame_with_ties_away_from_zero() {
        assert_eq!(seconds_to_frame(0.0, 48000), Some(0));
        assert_eq!(seconds_to_frame(-0.0, 48000), Some(0));
        assert_eq!(seconds_to_frame(1.0, 48000), Some(48000));
        // 0.1 * 44100 = 4410.000000000001 in binary64.
        assert_eq!(seconds_to_frame(0.1, 44100), Some(4410));
        // Exact ties at a power-of-two rate.
        assert_eq!(seconds_to_frame(0.25, 2), Some(1));
        assert_eq!(seconds_to_frame(0.75, 2), Some(2));
        assert_eq!(seconds_to_frame(1.25, 2), Some(3));
        // Just below and above a tie.
        assert_eq!(seconds_to_frame(0.249_999_999_999_999_97, 2), Some(0));
        assert_eq!(seconds_to_frame(0.250_000_000_000_000_06, 2), Some(1));
        // Truncation would give 47999 here.
        assert_eq!(seconds_to_frame(0.999_99, 48000), Some(48000));
    }

    #[test]
    fn rejects_invalid_times_and_saturates_huge_ones() {
        for bad in [f64::NAN, f64::INFINITY, -f64::INFINITY, -1e-300, -1.0] {
            assert_eq!(seconds_to_frame(bad, 48000), None, "{bad}");
        }
        assert_eq!(seconds_to_frame(1.0, 0), None);
        assert_eq!(seconds_to_frame(1e300, 48000), Some(u64::MAX));
        assert_eq!(seconds_to_frame(f64::MAX, 1), Some(u64::MAX));
        // 2^63 frames is still representable.
        assert_eq!(
            seconds_to_frame(9_223_372_036_854_775_808.0, 1),
            Some(1 << 63)
        );
        assert_eq!(
            seconds_to_frame(18_446_744_073_709_551_616.0, 1),
            Some(u64::MAX)
        );
    }

    #[test]
    fn matches_c_llround_for_in_range_products() {
        // The output trim used llround, which also rounds halfway cases away from zero.
        for (seconds, rate, frame) in [
            (0.2, 48000, 9600),
            (0.7, 48000, 33600),
            (1.5 / 48000.0, 48000, 2),
            (2.5 / 96000.0, 96000, 3),
        ] {
            assert_eq!(
                seconds_to_frame(seconds, rate),
                Some(frame),
                "{seconds} {rate}"
            );
        }
    }
}
