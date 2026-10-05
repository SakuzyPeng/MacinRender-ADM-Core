//! Private Monitor boundary. Handles are exclusive; error storage must be valid and disjoint.
use crate::boundary;
use mradm_dsp::{
    Error, Result,
    monitor::{Callback, Crossfade, Output},
};
use std::{ptr, slice};

fn invalid() -> Error {
    Error::InvalidArgument("Invalid monitor FFI arguments")
}
fn range<T>(p: *const T, n: usize) -> Result<(usize, usize)> {
    if n > isize::MAX as usize / size_of::<T>()
        || (n != 0 && (p.is_null() || !(p as usize).is_multiple_of(align_of::<T>())))
    {
        return Err(invalid());
    }
    let start = p as usize;
    Ok((
        start,
        start.checked_add(n * size_of::<T>()).ok_or_else(invalid)?,
    ))
}
fn separate<A, B>(a: *const A, an: usize, b: *const B, bn: usize) -> Result<()> {
    let (a, z) = range(a, an)?;
    let (b, y) = range(b, bn)?;
    if a != z && b != y && a < y && b < z {
        return Err(invalid());
    }
    Ok(())
}
unsafe fn input<'a>(p: *const f32, n: usize) -> Result<&'a [f32]> {
    range(p, n)?;
    Ok(if n == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(p, n) }
    })
}
unsafe fn output<'a>(p: *mut f32, n: usize) -> Result<&'a mut [f32]> {
    range(p, n)?;
    Ok(if n == 0 {
        &mut []
    } else {
        unsafe { slice::from_raw_parts_mut(p, n) }
    })
}
unsafe fn get_mut<'a, T>(p: *mut T) -> Result<&'a mut T> {
    range(p, 1)?;
    Ok(unsafe { &mut *p })
}
unsafe fn create<T>(out: *mut *mut T, make: impl FnOnce() -> Result<T>) -> Result<()> {
    range(out, 1)?;
    unsafe {
        *out = ptr::null_mut();
    }
    let state = make()?;
    unsafe {
        *out = Box::into_raw(Box::new(state));
    }
    Ok(())
}
fn flag(value: u32) -> Result<bool> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(invalid()),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_monitor_crossfade_create(
    channels: usize,
    frames: u64,
    out: *mut *mut Crossfade,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Crossfade::new(channels, frames))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_monitor_output_create(
    channels: usize,
    rate: u32,
    realtime: u32,
    out: *mut *mut Output,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Output::new(channels, rate, flag(realtime)?))
    })
}
macro_rules! lifecycle {
    ($destroy:ident, $reset:ident, $ty:ty) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $destroy(handle: *mut $ty) {
            if !handle.is_null() {
                unsafe {
                    drop(Box::from_raw(handle));
                }
            }
        }
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $reset(
            handle: *mut $ty,
            message: *mut u8,
            capacity: usize,
        ) -> i32 {
            boundary(message, capacity, || unsafe {
                get_mut(handle)?.reset();
                Ok(())
            })
        }
    };
}
lifecycle!(
    mradm_dsp_monitor_crossfade_destroy,
    mradm_dsp_monitor_crossfade_reset,
    Crossfade
);
lifecycle!(
    mradm_dsp_monitor_output_destroy,
    mradm_dsp_monitor_output_reset,
    Output
);

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_monitor_crossfade_process(
    handle: *mut Crossfade,
    old: *mut f32,
    old_len: usize,
    incoming: *const f32,
    incoming_len: usize,
    frames: usize,
    complete: *mut u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(old, old_len, incoming, incoming_len)?;
        separate(complete, 1, old, old_len)?;
        separate(complete, 1, incoming, incoming_len)?;
        separate(handle, 1, old, old_len)?;
        separate(handle, 1, incoming, incoming_len)?;
        separate(handle, 1, complete, 1)?;
        let done = get_mut(handle)?.process(
            output(old, old_len)?,
            input(incoming, incoming_len)?,
            frames,
        )?;
        *complete = u32::from(done);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_monitor_output_process(
    handle: *mut Output,
    pcm: *mut f32,
    pcm_len: usize,
    frames: usize,
    produced: usize,
    active: u32,
    generation: u64,
    peak: *mut f32,
    peak_len: usize,
    rms: *mut f32,
    rms_len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(pcm, pcm_len, peak, peak_len)?;
        separate(pcm, pcm_len, rms, rms_len)?;
        separate(peak, peak_len, rms, rms_len)?;
        separate(handle, 1, pcm, pcm_len)?;
        separate(handle, 1, peak, peak_len)?;
        separate(handle, 1, rms, rms_len)?;
        let call = Callback {
            frames,
            produced_frames: produced,
            active: flag(active)?,
            generation,
        };
        get_mut(handle)?.process(
            output(pcm, pcm_len)?,
            call,
            output(peak, peak_len)?,
            output(rms, rms_len)?,
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;
    #[test]
    fn first_calls_aliases_lengths_flags_and_state_atomicity() {
        unsafe {
            let mut fade = ptr::null_mut();
            let mut state = ptr::null_mut();
            assert_eq!(
                mradm_dsp_monitor_crossfade_create(2, 2048, &mut fade, ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                mradm_dsp_monitor_output_create(2, 48000, 1, &mut state, ptr::null_mut(), 0),
                0
            );
            let mut pcm = [0.25; 74];
            let incoming = [0.75; 74];
            let mut peaks = [17.0; 64];
            let mut rms = [18.0; 64];
            let mut complete = 9;
            COUNT.with(|c| c.set(Some(0)));
            assert_eq!(
                mradm_dsp_monitor_crossfade_process(
                    fade,
                    pcm.as_mut_ptr(),
                    74,
                    incoming.as_ptr(),
                    74,
                    37,
                    &mut complete,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                mradm_dsp_monitor_output_process(
                    state,
                    pcm.as_mut_ptr(),
                    74,
                    37,
                    37,
                    1,
                    1,
                    peaks.as_mut_ptr(),
                    64,
                    rms.as_mut_ptr(),
                    64,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                mradm_dsp_monitor_crossfade_reset(fade, ptr::null_mut(), 0),
                0
            );
            assert_eq!(mradm_dsp_monitor_output_reset(state, ptr::null_mut(), 0), 0);
            assert_eq!(
                mradm_dsp_monitor_output_process(
                    state,
                    pcm.as_mut_ptr(),
                    1,
                    1,
                    1,
                    1,
                    2,
                    peaks.as_mut_ptr(),
                    64,
                    rms.as_mut_ptr(),
                    64,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(COUNT.with(|c| c.replace(None).unwrap()), 0);
            let saved = (*state).clone();
            let saved_fade = (*fade).clone();
            let saved_pcm = pcm;
            let saved_peak = peaks;
            let saved_rms = rms;
            for (length, frames, produced, active, pn, rn) in [
                (73, 1, 1, 1, 64, 64),
                (74, 38, 38, 1, 64, 64),
                (74, 1, 2, 1, 64, 64),
                (74, 1, 1, 2, 64, 64),
                (74, 1, 1, 1, 1, 64),
                (74, 1, 1, 1, 64, 1),
                (74, usize::MAX, 0, 1, 64, 64),
                (usize::MAX, 0, 0, 1, 64, 64),
            ] {
                assert_eq!(
                    mradm_dsp_monitor_output_process(
                        state,
                        pcm.as_mut_ptr(),
                        length,
                        frames,
                        produced,
                        active,
                        2,
                        peaks.as_mut_ptr(),
                        pn,
                        rms.as_mut_ptr(),
                        rn,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!(*state, saved);
                assert_eq!(pcm, saved_pcm);
                assert_eq!(peaks, saved_peak);
                assert_eq!(rms, saved_rms);
            }
            // Partial and exact aliasing, null and misaligned pointers must fail before forming Rust slices.
            for p in [
                pcm.as_mut_ptr(),
                pcm.as_mut_ptr().add(1),
                rms.as_mut_ptr(),
                ptr::null_mut(),
                pcm.as_mut_ptr().cast::<u8>().add(1).cast(),
            ] {
                assert_eq!(
                    mradm_dsp_monitor_output_process(
                        state,
                        pcm.as_mut_ptr(),
                        74,
                        1,
                        1,
                        1,
                        2,
                        p,
                        2,
                        rms.as_mut_ptr(),
                        64,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
            }
            assert_eq!(*state, saved);
            assert_eq!(pcm, saved_pcm);
            assert_eq!(rms, saved_rms);
            for (ip, n, frames, done) in [
                (pcm.as_ptr(), 74, 1, &mut complete as *mut u32),
                (incoming.as_ptr(), 73, 1, &mut complete),
                (incoming.as_ptr(), 74, 38, &mut complete),
                (incoming.as_ptr(), 74, usize::MAX, &mut complete),
                (incoming.as_ptr(), 74, 1, ptr::null_mut()),
                (incoming.as_ptr(), 74, 1, pcm.as_mut_ptr().cast()),
            ] {
                complete = 9;
                assert_eq!(
                    mradm_dsp_monitor_crossfade_process(
                        fade,
                        pcm.as_mut_ptr(),
                        74,
                        ip,
                        n,
                        frames,
                        done,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!(complete, 9);
                assert_eq!(*fade, saved_fade);
                assert_eq!(pcm, saved_pcm);
            }
            assert_eq!(
                mradm_dsp_monitor_output_process(
                    state,
                    ptr::null_mut(),
                    0,
                    0,
                    0,
                    1,
                    2,
                    peaks.as_mut_ptr(),
                    64,
                    rms.as_mut_ptr(),
                    64,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(*state, saved);
            assert_eq!(&peaks[..2], &[0.0; 2]);
            // Rejected calls have not consumed the generation or advanced the blend.
            assert_eq!(
                mradm_dsp_monitor_output_process(
                    state,
                    pcm.as_mut_ptr(),
                    74,
                    1,
                    1,
                    1,
                    2,
                    peaks.as_mut_ptr(),
                    64,
                    rms.as_mut_ptr(),
                    64,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(&pcm[..2], &[0.0; 2]);
            assert_eq!(
                mradm_dsp_monitor_crossfade_process(
                    fade,
                    pcm.as_mut_ptr(),
                    74,
                    incoming.as_ptr(),
                    74,
                    1,
                    &mut complete,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(&pcm[..2], &[0.0; 2]);
            mradm_dsp_monitor_output_destroy(state);
            mradm_dsp_monitor_crossfade_destroy(fade);
            mradm_dsp_monitor_output_destroy(ptr::null_mut());
            state = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_monitor_output_create(2, 48000, 2, &mut state, ptr::null_mut(), 0),
                1
            );
            assert!(state.is_null());
            assert_eq!(
                mradm_dsp_monitor_output_reset(ptr::null_mut(), ptr::null_mut(), 0),
                1
            );
            assert_eq!(
                mradm_dsp_monitor_crossfade_create(2, 0, &mut fade, ptr::null_mut(), 0),
                1
            );
            assert!(fade.is_null());
        }
    }
}
