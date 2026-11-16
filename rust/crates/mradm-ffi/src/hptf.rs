pub mod settings;
use crate::{boundary, ptr, slice};
use mradm_dsp::{Error, Result, hptf as dsp};

#[repr(C)]
pub struct Band {
    pub kind: u32,
    pub enabled: u32,
    pub frequency: f64,
    pub gain_db: f64,
    pub q: f64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Biquad {
    pub b0: f32,
    pub b1: f32,
    pub b2: f32,
    pub a1: f32,
    pub a2: f32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Coefficients {
    pub sample_rate: u32,
    pub band_count: u32,
    pub preamp_gain: f32,
    pub sections: [Biquad; dsp::MAX_BANDS],
    pub max_response_db: f32,
    pub auto_trim_db: f32,
    pub preamp_db: f32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Snapshot {
    pub coefficients: Coefficients,
    pub revision: u64,
    pub right: Coefficients,
    pub independent: u32,
}

impl From<dsp::Coefficients> for Coefficients {
    fn from(c: dsp::Coefficients) -> Self {
        Self {
            sample_rate: c.sample_rate,
            band_count: c.band_count as u32,
            preamp_gain: c.preamp_gain,
            sections: c.sections.map(|s| Biquad {
                b0: s.b0,
                b1: s.b1,
                b2: s.b2,
                a1: s.a1,
                a2: s.a2,
            }),
            max_response_db: c.max_response_db,
            auto_trim_db: c.auto_trim_db,
            preamp_db: c.preamp_db,
        }
    }
}
impl From<Coefficients> for dsp::Coefficients {
    fn from(c: Coefficients) -> Self {
        Self {
            sample_rate: c.sample_rate,
            band_count: c.band_count as usize,
            preamp_gain: c.preamp_gain,
            sections: c.sections.map(|s| dsp::Biquad {
                b0: s.b0,
                b1: s.b1,
                b2: s.b2,
                a1: s.a1,
                a2: s.a2,
            }),
            max_response_db: c.max_response_db,
            auto_trim_db: c.auto_trim_db,
            preamp_db: c.preamp_db,
        }
    }
}
impl From<dsp::StereoSnapshot> for Snapshot {
    fn from(s: dsp::StereoSnapshot) -> Self {
        Self {
            coefficients: s.coefficients[0].into(),
            right: s.coefficients[1].into(),
            revision: s.revision,
            independent: u32::from(s.independent),
        }
    }
}
impl From<Snapshot> for dsp::StereoSnapshot {
    fn from(s: Snapshot) -> Self {
        Self {
            coefficients: [
                s.coefficients.into(),
                if s.independent != 0 {
                    s.right
                } else {
                    s.coefficients
                }
                .into(),
            ],
            revision: s.revision,
            independent: s.independent != 0,
        }
    }
}
unsafe fn shared<'a, T>(value: *const T) -> Result<&'a T> {
    unsafe { value.as_ref() }.ok_or(Error::InvalidArgument("HpTF: null argument"))
}
unsafe fn mutable<'a, T>(value: *mut T) -> Result<&'a mut T> {
    unsafe { value.as_mut() }.ok_or(Error::InvalidArgument("HpTF: null argument"))
}
unsafe fn input<'a, T>(data: *const T, len: usize) -> Result<&'a [T]> {
    if len > isize::MAX as usize / size_of::<T>() || (len != 0 && data.is_null()) {
        return Err(Error::InvalidArgument("HpTF: invalid input buffer"));
    }
    Ok(if len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, len) }
    })
}
unsafe fn output<'a>(data: *mut f32, len: usize) -> Result<&'a mut [f32]> {
    if len > isize::MAX as usize / size_of::<f32>() || (len != 0 && data.is_null()) {
        return Err(Error::InvalidArgument("HpTF: invalid audio buffer"));
    }
    Ok(if len == 0 {
        &mut []
    } else {
        unsafe { slice::from_raw_parts_mut(data, len) }
    })
}
unsafe fn create<T>(out: *mut *mut T, make: impl FnOnce() -> Result<T>) -> Result<()> {
    if out.is_null() {
        return Err(Error::InvalidArgument("HpTF: null output handle"));
    }
    unsafe {
        *out = ptr::null_mut();
    }
    let value = make()?;
    unsafe {
        *out = Box::into_raw(Box::new(value));
    }
    Ok(())
}
unsafe fn destroy<T>(value: *mut T) {
    if !value.is_null() {
        unsafe {
            drop(Box::from_raw(value));
        }
    }
}
unsafe fn target(value: *const Snapshot) -> Result<Option<dsp::StereoSnapshot>> {
    let value = unsafe { value.as_ref() }.copied();
    if value.is_some_and(|v| v.independent > 1) {
        return Err(Error::InvalidArgument("HpTF:无效的双耳配置标志"));
    }
    Ok(value.map(Into::into))
}

/// The shared processor behind the C++ `HptfProcessor` (MonitorEngine's HpTF insert).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_live_create(
    channels: usize,
    rate: u32,
    out: *mut *mut dsp::Live,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || dsp::Live::new(channels, rate))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_live_destroy(value: *mut dsp::Live) {
    unsafe {
        destroy(value);
    }
}
/// Control thread: queue a designed target.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_live_publish(
    value: *const dsp::Live,
    target: *const Snapshot,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let target = self::target(target)?.ok_or(Error::InvalidArgument("HpTF: null argument"))?;
        shared(value)?.publish(target)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_live_bypass(
    value: *const dsp::Live,
    revision: u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        shared(value)?.publish_bypass(revision)
    })
}
/// Only while the callback is parked.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_live_reset(value: *const dsp::Live) {
    if let Some(live) = unsafe { value.as_ref() } {
        live.reset_state();
    }
}
/// Audio callback: interleaved PCM in place.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_live_process(
    value: *const dsp::Live,
    samples: *mut f32,
    length: usize,
) {
    if let (Some(live), Ok(samples)) = (unsafe { value.as_ref() }, unsafe {
        output(samples, length)
    }) {
        live.process(samples);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_live_snapshot(value: *const dsp::Live, out: *mut Snapshot) {
    if let (Some(live), Some(out)) = (unsafe { value.as_ref() }, unsafe { out.as_mut() }) {
        *out = live.active_snapshot().into();
    }
}

/// error: 0 parsed, 1 Preamp value, 2 Fc value, 3 Gain/Q value, 4 nothing usable.
/// line_offset/line_len index the offending trimmed line of the caller's text.
#[repr(C)]
pub struct ParseResult {
    pub error: u32,
    pub preamp_db: f64,
    pub band_count: usize,
    pub line_offset: usize,
    pub line_len: usize,
}

/// Parses AutoEq ParametricEQ text. Writes at most `capacity` bands and always reports the
/// full `band_count`; the caller retries with a larger buffer when it exceeds `capacity`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_parse(
    text: *const u8,
    length: usize,
    bands: *mut Band,
    capacity: usize,
    result: *mut ParseResult,
    message: *mut u8,
    message_capacity: usize,
) -> i32 {
    boundary(message, message_capacity, || unsafe {
        let result = mutable(result)?;
        if bands.is_null() && capacity != 0 {
            return Err(Error::InvalidArgument("HpTF: null band buffer"));
        }
        let text = input(text, length)?;
        *result = match dsp::parse_parametric_eq(text) {
            Ok(profile) => {
                for (slot, band) in profile.bands.iter().take(capacity).enumerate() {
                    bands.add(slot).write(Band {
                        kind: band.kind as u32,
                        enabled: u32::from(band.enabled),
                        frequency: band.frequency,
                        gain_db: band.gain_db,
                        q: band.q,
                    });
                }
                ParseResult {
                    error: 0,
                    preamp_db: profile.preamp_db,
                    band_count: profile.bands.len(),
                    line_offset: 0,
                    line_len: 0,
                }
            }
            Err(e) => ParseResult {
                error: match e.kind {
                    dsp::ParseErrorKind::Preamp => 1,
                    dsp::ParseErrorKind::Frequency => 2,
                    dsp::ParseErrorKind::GainOrQ => 3,
                    dsp::ParseErrorKind::Empty => 4,
                },
                preamp_db: 0.,
                band_count: 0,
                line_offset: e.line.start,
                line_len: e.line.len(),
            },
        };
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn coefficients() -> Coefficients {
        dsp::Coefficients {
            sample_rate: 48000,
            ..dsp::Coefficients::default()
        }
        .into()
    }
    #[test]
    fn live_boundaries_reject_invalid_buffers_without_advancing_audio_or_status() {
        unsafe {
            let mut audio = [0.25; 16];
            let mut live = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_hptf_live_create(usize::MAX, 48000, &mut live, ptr::null_mut(), 0),
                1
            );
            assert!(live.is_null());
            assert_eq!(
                mradm_dsp_hptf_live_create(2, 48000, &mut live, ptr::null_mut(), 0),
                0
            );
            let mut target = Snapshot {
                coefficients: coefficients(),
                revision: 1,
                right: coefficients(),
                independent: 0,
            };
            target.coefficients.preamp_gain = 0.5;
            let mut status = Snapshot {
                revision: 42,
                ..target
            };
            target.coefficients.sample_rate = 44100;
            assert_eq!(
                mradm_dsp_hptf_live_publish(live, &target, ptr::null_mut(), 0),
                1,
                "a target the processor would reject fails on publication"
            );
            assert_eq!(
                mradm_dsp_hptf_live_publish(live, ptr::null(), ptr::null_mut(), 0),
                1
            );
            target.coefficients.sample_rate = 48000;
            assert_eq!(
                mradm_dsp_hptf_live_publish(live, &target, ptr::null_mut(), 0),
                0
            );
            audio.fill(42.);
            for length in [usize::MAX, 3] {
                mradm_dsp_hptf_live_process(live, audio.as_mut_ptr(), length);
                assert_eq!(audio, [42.; 16], "a bad buffer leaves the audio");
            }
            mradm_dsp_hptf_live_process(live, audio.as_mut_ptr(), 16);
            mradm_dsp_hptf_live_snapshot(live, &mut status);
            assert_eq!(status.revision, 0, "the fade is still running");
            mradm_dsp_hptf_live_reset(live);
            mradm_dsp_hptf_live_snapshot(live, &mut status);
            assert_eq!(status.revision, 1, "the reset adopted the fading target");
            assert_eq!(mradm_dsp_hptf_live_bypass(live, 2, ptr::null_mut(), 0), 0);
            mradm_dsp_hptf_live_reset(live);
            mradm_dsp_hptf_live_snapshot(live, &mut status);
            assert_eq!(status.revision, 2);
            mradm_dsp_hptf_live_snapshot(ptr::null(), &mut status);
            mradm_dsp_hptf_live_destroy(live);
            mradm_dsp_hptf_live_destroy(ptr::null_mut());
        }
    }
}
