use crate::{boundary, ptr, slice};
use mradm_dsp::{Error, Result, hptf as dsp};

#[repr(C)]
pub struct Band {
    kind: u32,
    enabled: u32,
    frequency: f64,
    gain_db: f64,
    q: f64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Biquad {
    b0: f32,
    b1: f32,
    b2: f32,
    a1: f32,
    a2: f32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Coefficients {
    sample_rate: u32,
    band_count: u32,
    preamp_gain: f32,
    sections: [Biquad; dsp::MAX_BANDS],
    max_response_db: f32,
    auto_trim_db: f32,
    preamp_db: f32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Snapshot {
    coefficients: Coefficients,
    revision: u64,
}
#[repr(C)]
pub struct Update {
    snapshot: Snapshot,
    blending: u32,
    applied: u32,
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
impl From<dsp::Snapshot> for Snapshot {
    fn from(s: dsp::Snapshot) -> Self {
        Self {
            coefficients: s.coefficients.into(),
            revision: s.revision,
        }
    }
}
impl From<Snapshot> for dsp::Snapshot {
    fn from(s: Snapshot) -> Self {
        Self {
            coefficients: s.coefficients.into(),
            revision: s.revision,
        }
    }
}
impl From<dsp::Update> for Update {
    fn from(s: dsp::Update) -> Self {
        Self {
            snapshot: s.snapshot.into(),
            blending: u32::from(s.blending),
            applied: u32::from(s.applied),
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
unsafe fn target(value: *const Snapshot) -> Option<dsp::Snapshot> {
    unsafe { value.as_ref() }.copied().map(Into::into)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_design(
    bands: *const Band,
    length: usize,
    preamp_db: f64,
    rate: u32,
    mode: u32,
    out: *mut Coefficients,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let out = mutable(out)?;
        let mode = dsp::PreampMode::try_from(mode)?;
        let bands = input(bands, length)?
            .iter()
            .map(|b| {
                if b.enabled > 1 {
                    return Err(Error::InvalidArgument("HpTF: invalid enabled flag"));
                }
                Ok(dsp::Band {
                    kind: dsp::BandKind::try_from(b.kind)?,
                    enabled: b.enabled != 0,
                    frequency: b.frequency,
                    gain_db: b.gain_db,
                    q: b.q,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let result = dsp::design(&bands, preamp_db, rate, mode)?;
        *out = result.into();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_magnitude(
    coefficients: *const Coefficients,
    hz: f64,
    out: *mut f64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let out = mutable(out)?;
        let c: dsp::Coefficients = (*shared(coefficients)?).into();
        let result = c.magnitude_db(hz)?;
        *out = result;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_cascade_create(
    channels: usize,
    out: *mut *mut dsp::Cascade,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || dsp::Cascade::new(channels))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_cascade_clone(
    value: *const dsp::Cascade,
    out: *mut *mut dsp::Cascade,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Ok(shared(value)?.clone()))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_cascade_destroy(value: *mut dsp::Cascade) {
    unsafe {
        destroy(value);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_cascade_set(
    value: *mut dsp::Cascade,
    coefficients: *const Coefficients,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        mutable(value)?.set_coefficients((*shared(coefficients)?).into())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_cascade_reset(
    value: *mut dsp::Cascade,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        mutable(value)?.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_cascade_process(
    value: *mut dsp::Cascade,
    samples: *mut f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        mutable(value)?.process(output(samples, length)?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_processor_create(
    channels: usize,
    rate: u32,
    out: *mut *mut dsp::Processor,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || dsp::Processor::new(channels, rate))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_processor_destroy(value: *mut dsp::Processor) {
    unsafe {
        destroy(value);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_processor_reset(
    value: *mut dsp::Processor,
    latest: *const Snapshot,
    out: *mut Update,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let out = mutable(out)?;
        let result = mutable(value)?.reset(target(latest))?;
        *out = result.into();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hptf_processor_process(
    value: *mut dsp::Processor,
    samples: *mut f32,
    length: usize,
    latest: *const Snapshot,
    out: *mut Update,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let out = mutable(out)?;
        let result = mutable(value)?.process(output(samples, length)?, target(latest))?;
        *out = result.into();
        Ok(())
    })
}

/// error: 0 parsed, 1 Preamp value, 2 Fc value, 3 Gain/Q value, 4 nothing usable.
/// line_offset/line_len index the offending trimmed line of the caller's text.
#[repr(C)]
pub struct ParseResult {
    error: u32,
    preamp_db: f64,
    band_count: usize,
    line_offset: usize,
    line_len: usize,
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
    fn update() -> Update {
        Update {
            snapshot: Snapshot {
                coefficients: coefficients(),
                revision: 42,
            },
            blending: 42,
            applied: 42,
        }
    }

    #[test]
    fn design_and_response_validate_sizes_flags_and_preserve_outputs() {
        unsafe {
            let mut out = coefficients();
            let before: dsp::Coefficients = out.into();
            for (bands, length, rate, mode, preamp) in [
                (ptr::null(), usize::MAX, 48000, 0, 0.),
                (ptr::null(), 1, 48000, 0, 0.),
                (ptr::null(), 0, 48000, 2, 0.),
                (ptr::null(), 0, 0, 0, 0.),
                (ptr::null(), 0, 48000, 0, f64::NAN),
            ] {
                assert_eq!(
                    mradm_dsp_hptf_design(
                        bands,
                        length,
                        preamp,
                        rate,
                        mode,
                        &mut out,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!(dsp::Coefficients::from(out), before);
            }
            let mut band = Band {
                kind: 0,
                enabled: 1,
                frequency: 1000.,
                gain_db: 6.,
                q: 1.,
            };
            for (kind, enabled, q) in [(7, 1, 1.), (0, 2, 1.), (0, 0, f64::NAN)] {
                band.kind = kind;
                band.enabled = enabled;
                band.q = q;
                assert_eq!(
                    mradm_dsp_hptf_design(&band, 1, 0., 48000, 0, &mut out, ptr::null_mut(), 0),
                    1
                );
                assert_eq!(dsp::Coefficients::from(out), before);
            }
            assert_eq!(
                mradm_dsp_hptf_design(ptr::null(), 0, -6., 48000, 0, &mut out, ptr::null_mut(), 0),
                0
            );
            assert!((out.preamp_gain - 10.0f32.powf(-6. / 20.)).abs() < 1e-7);
            let mut db = 42.;
            assert_eq!(
                mradm_dsp_hptf_magnitude(&out, f64::NAN, &mut db, ptr::null_mut(), 0),
                1
            );
            assert_eq!(db, 42.);
            out.band_count = 33;
            assert_eq!(
                mradm_dsp_hptf_magnitude(&out, 1000., &mut db, ptr::null_mut(), 0),
                1
            );
            assert_eq!(db, 42.);
        }
    }

    #[test]
    fn runtime_boundaries_reject_invalid_buffers_without_advancing_audio_or_status() {
        unsafe {
            let mut cascade = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_hptf_cascade_create(usize::MAX, &mut cascade, ptr::null_mut(), 0),
                1
            );
            assert!(cascade.is_null());
            assert_eq!(
                mradm_dsp_hptf_cascade_create(2, &mut cascade, ptr::null_mut(), 0),
                0
            );
            let mut c = coefficients();
            c.preamp_gain = 0.5;
            assert_eq!(
                mradm_dsp_hptf_cascade_set(cascade, &c, ptr::null_mut(), 0),
                0
            );
            let mut cloned = ptr::null_mut();
            assert_eq!(
                mradm_dsp_hptf_cascade_clone(cascade, &mut cloned, ptr::null_mut(), 0),
                0
            );
            c.band_count = 33;
            assert_eq!(
                mradm_dsp_hptf_cascade_set(cascade, &c, ptr::null_mut(), 0),
                1
            );
            let mut audio = [0.25; 16];
            for length in [usize::MAX, 3] {
                assert_eq!(
                    mradm_dsp_hptf_cascade_process(
                        cascade,
                        audio.as_mut_ptr(),
                        length,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!(audio, [0.25; 16]);
            }
            assert_eq!(
                mradm_dsp_hptf_cascade_process(cascade, ptr::null_mut(), 16, ptr::null_mut(), 0),
                1
            );
            assert_eq!(
                mradm_dsp_hptf_cascade_process(cascade, ptr::null_mut(), 0, ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                mradm_dsp_hptf_cascade_process(cascade, audio.as_mut_ptr(), 16, ptr::null_mut(), 0),
                0
            );
            assert_eq!(audio, [0.125; 16]);
            mradm_dsp_hptf_cascade_destroy(cascade);
            audio.fill(0.25);
            assert_eq!(
                mradm_dsp_hptf_cascade_process(cloned, audio.as_mut_ptr(), 16, ptr::null_mut(), 0),
                0
            );
            assert_eq!(audio, [0.125; 16]);
            assert_eq!(mradm_dsp_hptf_cascade_reset(cloned, ptr::null_mut(), 0), 0);
            mradm_dsp_hptf_cascade_destroy(cloned);
            mradm_dsp_hptf_cascade_destroy(ptr::null_mut());
            let mut processor = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_hptf_processor_create(
                    usize::MAX,
                    48000,
                    &mut processor,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert!(processor.is_null());
            assert_eq!(
                mradm_dsp_hptf_processor_create(2, 48000, &mut processor, ptr::null_mut(), 0),
                0
            );
            let mut target = Snapshot {
                coefficients: coefficients(),
                revision: 1,
            };
            target.coefficients.preamp_gain = 0.5;
            let mut status = update();
            audio.fill(42.);
            for length in [usize::MAX, 3] {
                assert_eq!(
                    mradm_dsp_hptf_processor_process(
                        processor,
                        audio.as_mut_ptr(),
                        length,
                        &target,
                        &mut status,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!(audio, [42.; 16]);
                assert_eq!(
                    (status.snapshot.revision, status.blending, status.applied),
                    (42, 42, 42)
                );
            }
            assert_eq!(
                mradm_dsp_hptf_processor_process(
                    processor,
                    audio.as_mut_ptr(),
                    16,
                    &target,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(audio, [42.; 16]);
            target.coefficients.sample_rate = 44100;
            assert_eq!(
                mradm_dsp_hptf_processor_reset(processor, &target, &mut status, ptr::null_mut(), 0),
                1
            );
            assert_eq!(status.snapshot.revision, 42);
            target.coefficients.sample_rate = 48000;
            assert_eq!(
                mradm_dsp_hptf_processor_process(
                    processor,
                    ptr::null_mut(),
                    0,
                    &target,
                    &mut status,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                (status.snapshot.revision, status.blending, status.applied),
                (0, 0, 0)
            );
            assert_eq!(
                mradm_dsp_hptf_processor_process(
                    processor,
                    audio.as_mut_ptr(),
                    16,
                    &target,
                    &mut status,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                (status.snapshot.revision, status.blending, status.applied),
                (0, 1, 0)
            );
            assert_eq!(
                mradm_dsp_hptf_processor_reset(
                    processor,
                    ptr::null(),
                    &mut status,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                (status.snapshot.revision, status.blending, status.applied),
                (1, 0, 1)
            );
            mradm_dsp_hptf_processor_destroy(processor);
            mradm_dsp_hptf_processor_destroy(ptr::null_mut());
        }
    }
}
