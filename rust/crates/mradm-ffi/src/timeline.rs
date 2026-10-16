//! Private boundary for seconds-to-frame conversion (PCM consistency 3.6).
use crate::{boundary, checked::*};
use mradm_dsp::timeline;

/// Writes the frame nearest to `seconds * sample_rate` (ties away from zero, saturating) to
/// `frame`. Fails for a non-finite or negative time or a zero rate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_seconds_to_frame(
    seconds: f64,
    sample_rate: u32,
    frame: *mut u64,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        output(frame, 1)?[0] =
            timeline::seconds_to_frame(seconds, sample_rate).ok_or_else(invalid)?;
        Ok(())
    })
}
