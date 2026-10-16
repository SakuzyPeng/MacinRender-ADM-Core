//! File-streaming and offline backend arithmetic that used to live in C++ (PCM consistency 3.1).
//!
//! Every product and sum here is rounded separately, as Rust never contracts `a * b + c` into a
//! fused multiply-add. The C++ expressions these replace could be contracted by clang on arm64,
//! so macOS and x86 hosts were not guaranteed to agree.
use crate::{Error, Result, semantic_policy};

fn invalid() -> Error {
    Error::InvalidArgument("Invalid file-stream DSP arguments")
}

/// `output[2f + c] += source_c[f] * gain[f]` for one stereo source of `frames` frames.
pub fn accumulate_stereo(
    output: &mut [f32],
    left: &[f32],
    right: &[f32],
    gain: &[f32],
    frames: usize,
) -> Result<()> {
    if frames > left.len()
        || frames > right.len()
        || frames > gain.len()
        || frames
            .checked_mul(2)
            .is_none_or(|samples| samples > output.len())
    {
        return Err(invalid());
    }
    for (((frame, &l), &r), &g) in output
        .as_chunks_mut::<2>()
        .0
        .iter_mut()
        .zip(left)
        .zip(right)
        .zip(gain)
        .take(frames)
    {
        frame[0] += l * g;
        frame[1] += r * g;
    }
    Ok(())
}

/// Linear blend of one interleaved stereo block from the outgoing into the incoming source graph:
/// frame `f` takes `mix = f / (frames - 1)` (1 for a single frame), and the result
/// `outgoing * (1 - mix) + incoming * mix` replaces `incoming`.
pub fn topology_crossfade(outgoing: &[f32], incoming: &mut [f32], frames: usize) -> Result<()> {
    let samples = frames.checked_mul(2).ok_or_else(invalid)?;
    if samples > outgoing.len() || samples > incoming.len() {
        return Err(invalid());
    }
    for (frame, (old, new)) in outgoing
        .as_chunks::<2>()
        .0
        .iter()
        .zip(incoming.as_chunks_mut::<2>().0)
        .take(frames)
        .enumerate()
    {
        let mix = if frames <= 1 {
            1.0
        } else {
            frame as f32 / (frames - 1) as f32
        };
        for (old, new) in old.iter().zip(new) {
            *new = *old * (1.0 - mix) + *new * mix;
        }
    }
    Ok(())
}

/// C++ `std::max(a, b)`: `b` when `a < b`, otherwise `a` (a NaN in `b` is dropped).
fn cpp_max(a: f32, b: f32) -> f32 {
    if a < b { b } else { a }
}

/// Binaural spreader cone in degrees from an object's extent: width weighs 60, height 45 and
/// depth adds 20 to both before the larger of the two is doubled. Negative extents count as 0.
pub fn extent_spread_degrees(width: f32, height: f32, depth: f32) -> f32 {
    let depth = cpp_max(0.0, depth) * 20.0;
    let width = cpp_max(0.0, width) * 60.0 + depth;
    let height = cpp_max(0.0, height) * 45.0 + depth;
    cpp_max(width, height) * 2.0
}

/// Linear gain of a live override in dB, with the same f32 `libm::powf` as the semantic policy.
pub fn live_gain(db: f32) -> f32 {
    semantic_policy::db_to_gain(db)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accumulates_and_validates_lengths() {
        let mut output = [1.0, 2.0, 3.0, 4.0, 9.0, 9.0];
        accumulate_stereo(&mut output, &[0.5, 0.25], &[2.0, 4.0], &[2.0, 0.5], 2).unwrap();
        assert_eq!(output, [2.0, 6.0, 3.125, 6.0, 9.0, 9.0]);
        assert!(accumulate_stereo(&mut output, &[0.5], &[2.0, 4.0], &[2.0, 0.5], 2).is_err());
        assert!(accumulate_stereo(&mut [0.0; 3], &[0.5; 2], &[2.0; 2], &[2.0; 2], 2).is_err());
        accumulate_stereo(&mut [], &[], &[], &[], 0).unwrap();
    }

    #[test]
    fn crossfade_runs_from_outgoing_to_incoming() {
        let outgoing = [1.0, -1.0, 1.0, -1.0, 1.0, -1.0];
        let mut incoming = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
        topology_crossfade(&outgoing, &mut incoming, 3).unwrap();
        assert_eq!(incoming, [1.0, -1.0, 0.5, -0.5, 0.0, 0.0]);
        let mut single = [3.0, 4.0];
        topology_crossfade(&[1.0, 2.0], &mut single, 1).unwrap();
        assert_eq!(single, [3.0, 4.0]);
        assert!(topology_crossfade(&outgoing, &mut [0.0; 4], 3).is_err());
    }

    #[test]
    fn spread_follows_the_cpp_mapping() {
        assert_eq!(extent_spread_degrees(0.0, 0.0, 0.0), 0.0);
        assert_eq!(extent_spread_degrees(30.0, 10.0, 0.0), 3600.0);
        assert_eq!(extent_spread_degrees(0.5, 0.25, 0.125), 65.0);
        assert_eq!(extent_spread_degrees(-1.0, 1.0, -1.0), 90.0);
        assert_eq!(extent_spread_degrees(f32::NAN, 0.0, 0.0), 0.0);
        assert_eq!(live_gain(0.0), 1.0);
        assert_eq!(live_gain(-20.0), semantic_policy::db_to_gain(-20.0));
    }
}
