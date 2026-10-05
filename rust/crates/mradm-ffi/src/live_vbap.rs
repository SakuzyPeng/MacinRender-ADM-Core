//! Private Live VBAP boundary. Handles are exclusive; error storage is valid and disjoint.
use crate::boundary;
use mradm_dsp::{
    Error, Result,
    live_vbap::{Command, Mixer, Status},
};
use std::{ptr, slice};
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Plane {
    samples: *const f32,
    length: usize,
    has_signal: u32,
    reserved: u32,
}
fn invalid() -> Error {
    Error::InvalidArgument("Invalid Live VBAP FFI arguments")
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
unsafe fn input<'a, T>(p: *const T, n: usize) -> Result<&'a [T]> {
    range(p, n)?;
    Ok(if n == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(p, n) }
    })
}
unsafe fn output<'a, T>(p: *mut T, n: usize) -> Result<&'a mut [T]> {
    range(p, n)?;
    Ok(if n == 0 {
        &mut []
    } else {
        unsafe { slice::from_raw_parts_mut(p, n) }
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_vbap_create(
    elements: u32,
    channels: u32,
    out: *mut *mut Mixer,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        range(out, 1)?;
        *out = ptr::null_mut();
        *out = Box::into_raw(Box::new(Mixer::new(elements, channels)?));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_vbap_destroy(handle: *mut Mixer) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_vbap_reset(
    handle: *mut Mixer,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        range(handle, 1)?;
        (*handle).reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_vbap_process(
    handle: *mut Mixer,
    frames: u32,
    planes: *const Plane,
    plane_count: usize,
    initial: *const Command,
    initial_count: usize,
    events: *const Command,
    event_count: usize,
    coefficients: *const f32,
    coefficient_count: usize,
    pcm: *mut f32,
    pcm_count: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(handle, 1, planes, plane_count)?;
        separate(handle, 1, initial, initial_count)?;
        separate(handle, 1, events, event_count)?;
        separate(handle, 1, coefficients, coefficient_count)?;
        separate(handle, 1, pcm, pcm_count)?;
        separate(pcm, pcm_count, planes, plane_count)?;
        separate(pcm, pcm_count, initial, initial_count)?;
        separate(pcm, pcm_count, events, event_count)?;
        separate(pcm, pcm_count, coefficients, coefficient_count)?;
        let planes = input(planes, plane_count)?;
        for plane in planes {
            if plane.has_signal > 1 || plane.reserved != 0 {
                return Err(invalid());
            }
            if plane.has_signal != 0 {
                separate(pcm, pcm_count, plane.samples, plane.length)?;
                separate(handle, 1, plane.samples, plane.length)?;
            }
        }
        // All raw ranges above are validated before constructing the mutable output or state borrow.
        let views = planes.iter().map(|p| {
            if p.has_signal == 0 {
                None
            } else if p.length == 0 {
                Some(&[][..])
            } else {
                Some(slice::from_raw_parts(p.samples, p.length))
            }
        });
        (*handle).process(
            frames,
            views,
            input(initial, initial_count)?,
            input(events, event_count)?,
            input(coefficients, coefficient_count)?,
            output(pcm, pcm_count)?,
        )
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_vbap_snapshot(
    handle: *const Mixer,
    element: u32,
    gains: *mut f32,
    gain_count: usize,
    status: *mut Status,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(handle, 1, gains, gain_count)?;
        separate(handle, 1, status, 1)?;
        separate(gains, gain_count, status, 1)?;
        let value = (*handle).snapshot(element, output(gains, gain_count)?)?;
        *status = value;
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;
    unsafe fn process(
        h: *mut Mixer,
        frames: u32,
        planes: &[Plane],
        initial: &[Command],
        events: &[Command],
        coefficients: &[f32],
        out: &mut [f32],
    ) -> i32 {
        unsafe {
            mradm_dsp_live_vbap_process(
                h,
                frames,
                planes.as_ptr(),
                planes.len(),
                initial.as_ptr(),
                initial.len(),
                events.as_ptr(),
                events.len(),
                coefficients.as_ptr(),
                coefficients.len(),
                out.as_mut_ptr(),
                out.len(),
                ptr::null_mut(),
                0,
            )
        }
    }
    #[test]
    fn first_calls_validation_aliases_snapshot_and_atomic_recovery() {
        unsafe {
            let mut h = ptr::null_mut();
            assert_eq!(
                mradm_dsp_live_vbap_create(1, 2, &mut h, ptr::null_mut(), 0),
                0
            );
            let data = [1.; 4];
            let planes = [Plane {
                samples: data.as_ptr(),
                length: 4,
                has_signal: 1,
                reserved: 0,
            }];
            let initial = [Command {
                fields: 3,
                level: 1.,
                ..Command::default()
            }];
            let valid = Command {
                fields: 2,
                duration: 4,
                level: 0.,
                ..Command::default()
            };
            let mut out = [17.; 8];
            let mut gains = [19.; 6];
            let mut status = Status::default();
            COUNT.with(|c| c.set(Some(0)));
            assert_eq!(
                process(h, 2, &planes, &initial, &[valid], &[1., 0.], &mut out),
                0
            );
            assert_eq!(
                mradm_dsp_live_vbap_snapshot(
                    h,
                    0,
                    gains.as_mut_ptr(),
                    6,
                    &mut status,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                process(
                    h,
                    2,
                    &planes,
                    &[],
                    &[Command {
                        element: 7,
                        ..valid
                    }],
                    &[],
                    &mut out
                ),
                1
            );
            assert_eq!(mradm_dsp_live_vbap_reset(h, ptr::null_mut(), 0), 0);
            assert_eq!(COUNT.with(|c| c.replace(None).unwrap()), 0);
            assert_eq!(status.current_level, 0.5);
            assert_eq!(status.level_remaining, 2);
            let before = (*h).clone();
            out.fill(17.);
            for events in [
                [
                    valid,
                    Command {
                        element: 1,
                        offset: 1,
                        ..valid
                    },
                ],
                [
                    valid,
                    Command {
                        reserved: 1,
                        ..valid
                    },
                ],
                [valid, Command { fields: 0, ..valid }],
                [
                    valid,
                    Command {
                        level: f32::NAN,
                        ..valid
                    },
                ],
                [valid, Command { offset: 2, ..valid }],
            ] {
                assert_eq!(
                    process(h, 2, &planes, &initial, &events, &[1., 0.], &mut out),
                    1
                );
                assert_eq!(out, [17.; 8]);
                assert_eq!(*h, before);
            }
            for plane in [
                Plane {
                    length: 1,
                    ..planes[0]
                },
                Plane {
                    has_signal: 2,
                    ..planes[0]
                },
                Plane {
                    reserved: 1,
                    ..planes[0]
                },
                Plane {
                    samples: ptr::null(),
                    ..planes[0]
                },
                Plane {
                    samples: out.as_ptr(),
                    ..planes[0]
                },
                Plane {
                    samples: data.as_ptr().cast::<u8>().add(1).cast(),
                    ..planes[0]
                },
                Plane {
                    length: usize::MAX,
                    ..planes[0]
                },
            ] {
                assert_eq!(
                    process(h, 2, &[plane], &initial, &[], &[1., 0.], &mut out),
                    1
                );
                assert_eq!(out, [17.; 8]);
                assert_eq!(*h, before);
            }
            // Descriptor/command storage must not overlap writable PCM either.
            assert_eq!(
                mradm_dsp_live_vbap_process(
                    h,
                    1,
                    planes.as_ptr(),
                    1,
                    initial.as_ptr(),
                    1,
                    ptr::null(),
                    0,
                    data.as_ptr(),
                    4,
                    initial.as_ptr().cast_mut().cast(),
                    4,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_live_vbap_process(
                    h,
                    1,
                    planes.as_ptr(),
                    1,
                    initial.as_ptr(),
                    usize::MAX,
                    ptr::null(),
                    0,
                    data.as_ptr(),
                    4,
                    out.as_mut_ptr(),
                    8,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(process(h, 2, &[], &initial, &[], &[1., 0.], &mut out), 1);
            assert_eq!(
                process(h, 2, &planes, &initial, &[], &[1., 0.], &mut out[..3]),
                1
            );
            let saved_gains = gains;
            let saved_status = status;
            assert_eq!(
                mradm_dsp_live_vbap_snapshot(
                    h,
                    1,
                    gains.as_mut_ptr(),
                    6,
                    &mut status,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_live_vbap_snapshot(
                    h,
                    0,
                    gains.as_mut_ptr(),
                    5,
                    &mut status,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_live_vbap_snapshot(
                    h,
                    0,
                    gains.as_mut_ptr(),
                    6,
                    ptr::null_mut(),
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_live_vbap_snapshot(
                    h,
                    0,
                    gains.as_mut_ptr(),
                    6,
                    gains.as_mut_ptr().cast(),
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(gains, saved_gains);
            assert_eq!(status, saved_status);
            assert_eq!(*h, before);
            assert_eq!(
                process(h, 2, &planes, &initial, &[valid], &[1., 0.], &mut out),
                0
            );
            assert_eq!(&out[..4], &[1., 0., 0.75, 0.]);
            assert_eq!(&out[4..], &[17.; 4]);
            mradm_dsp_live_vbap_destroy(h);
            mradm_dsp_live_vbap_destroy(ptr::null_mut());
            h = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_live_vbap_create(1, 0, &mut h, ptr::null_mut(), 0),
                1
            );
            assert!(h.is_null());
            assert_eq!(mradm_dsp_live_vbap_reset(h, ptr::null_mut(), 0), 1);
        }
    }
}
