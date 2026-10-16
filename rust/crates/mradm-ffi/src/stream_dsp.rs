//! Private boundary for file-streaming and monitor arithmetic (PCM consistency 3.1).
use crate::{boundary, checked::*};
use mradm_dsp::{monitor, stream_dsp};

/// Adds one stereo source weighted by `gain` into interleaved `mix`; `left`, `right` and
/// `gain` hold `frames` floats each and must not overlap `mix`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_stream_accumulate(
    mix: *mut f32,
    mix_length: usize,
    left: *const f32,
    right: *const f32,
    gain: *const f32,
    frames: usize,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        for source in [left, right, gain] {
            separate(mix, mix_length, source, frames)?;
        }
        stream_dsp::accumulate_stereo(
            output(mix, mix_length)?,
            input(left, frames)?,
            input(right, frames)?,
            input(gain, frames)?,
            frames,
        )
    })
}

/// Blends `2 * frames` interleaved stereo samples of `outgoing` into `incoming`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_stream_topology_crossfade(
    outgoing: *const f32,
    incoming: *mut f32,
    frames: usize,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        let samples = frames.checked_mul(2).ok_or_else(invalid)?;
        separate(outgoing, samples, incoming, samples)?;
        stream_dsp::topology_crossfade(
            input(outgoing, samples)?,
            output(incoming, samples)?,
            frames,
        )
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn mradm_dsp_extent_spread_degrees(width: f32, height: f32, depth: f32) -> f32 {
    stream_dsp::extent_spread_degrees(width, height, depth)
}

#[unsafe(no_mangle)]
pub extern "C" fn mradm_dsp_live_gain(db: f32) -> f32 {
    stream_dsp::live_gain(db)
}

/// Writes the {L, R} fold gains of a speaker at `azimuth` degrees into `gains[2]`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_monitor_stereo_fold(azimuth: f32, gains: *mut f32) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        output(gains, 2)?.copy_from_slice(&monitor::stereo_fold(azimuth));
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn forwards_and_rejects_overlap() {
        let mut out = [0.0f32; 4];
        let source = [1.0f32, 2.0];
        let gain = [0.5f32, 0.25];
        assert_eq!(
            unsafe {
                mradm_dsp_stream_accumulate(
                    out.as_mut_ptr(),
                    4,
                    source.as_ptr(),
                    source.as_ptr(),
                    gain.as_ptr(),
                    2,
                )
            },
            0
        );
        assert_eq!(out, [0.5, 0.5, 0.5, 0.5]);
        let alias = out.as_ptr();
        assert_ne!(
            unsafe { mradm_dsp_stream_accumulate(out.as_mut_ptr(), 4, alias, alias, alias, 2) },
            0
        );
        let mut block = [0.0f32; 4];
        assert_ne!(
            unsafe { mradm_dsp_stream_topology_crossfade(block.as_ptr(), block.as_mut_ptr(), 2) },
            0
        );
        let mut gains = [0.0f32; 2];
        assert_eq!(
            unsafe { mradm_dsp_monitor_stereo_fold(-90.0, gains.as_mut_ptr()) },
            0
        );
        assert_eq!(gains, [0.0, 1.0]);
        assert_ne!(
            unsafe { mradm_dsp_monitor_stereo_fold(0.0, std::ptr::null_mut()) },
            0
        );
        assert_eq!(mradm_dsp_extent_spread_degrees(0.5, 0.25, 0.125), 65.0);
        assert_eq!(mradm_dsp_live_gain(0.0), 1.0);
    }
}
