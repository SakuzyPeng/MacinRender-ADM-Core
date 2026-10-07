//! Private Live Triple Balance boundary. Handles are exclusive; error storage is valid and disjoint.
use crate::boundary;
use mradm_dsp::{
    Error, Result,
    triple_balance::{
        Layout,
        live::{Mixer, TbLiveCommand, TbLiveElement},
    },
};
use std::{ptr, slice};
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TbLivePlane {
    samples: *const f32,
    length: usize,
    has_signal: u32,
    reserved: u32,
}
fn invalid() -> Error {
    Error::InvalidArgument("Invalid Live Triple Balance FFI arguments")
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
pub unsafe extern "C" fn mradm_dsp_live_tb_create(
    layout: u32,
    rate: u32,
    elements: *const TbLiveElement,
    element_count: usize,
    out: *mut *mut Mixer,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        range(out, 1)?;
        *out = ptr::null_mut();
        *out = Box::into_raw(Box::new(Mixer::new(
            Layout::from_code(layout)?,
            rate,
            input(elements, element_count)?,
        )?));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_tb_destroy(handle: *mut Mixer) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_tb_reset(
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
pub unsafe extern "C" fn mradm_dsp_live_tb_process(
    handle: *mut Mixer,
    frames: u32,
    planes: *const TbLivePlane,
    plane_count: usize,
    initial: *const TbLiveCommand,
    initial_count: usize,
    events: *const TbLiveCommand,
    event_count: usize,
    pcm: *mut f32,
    pcm_count: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(handle, 1, planes, plane_count)?;
        separate(handle, 1, initial, initial_count)?;
        separate(handle, 1, events, event_count)?;
        separate(handle, 1, pcm, pcm_count)?;
        separate(pcm, pcm_count, planes, plane_count)?;
        separate(pcm, pcm_count, initial, initial_count)?;
        separate(pcm, pcm_count, events, event_count)?;
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
            output(pcm, pcm_count)?,
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;
    use mradm_dsp::triple_balance::Position;

    #[test]
    fn first_process_and_reset_do_not_allocate_and_rejection_is_atomic() {
        unsafe {
            let mut handle = ptr::null_mut();
            let element = TbLiveElement::default();
            assert_eq!(
                mradm_dsp_live_tb_create(1, 48000, &element, 1, &mut handle, ptr::null_mut(), 0),
                0
            );
            let samples = [0.1; 17];
            let mut plane = TbLivePlane {
                samples: samples.as_ptr(),
                length: 17,
                has_signal: 1,
                reserved: 0,
            };
            let initial = TbLiveCommand {
                fields: 7,
                position: Position {
                    x: 0.3,
                    y: 0.4,
                    z: 0.5,
                },
                size: 0.3,
                level: 1.,
                ..Default::default()
            };
            let event = TbLiveCommand {
                offset: 3,
                duration: 7,
                fields: 2,
                size: 0.7,
                ..initial
            };
            let mut out = [19.; 17 * 16];
            let process = |p: &TbLivePlane, event: &TbLiveCommand, out: &mut [f32]| {
                mradm_dsp_live_tb_process(
                    handle,
                    17,
                    p,
                    1,
                    &initial,
                    1,
                    event,
                    1,
                    out.as_mut_ptr(),
                    out.len(),
                    ptr::null_mut(),
                    0,
                )
            };
            COUNT.with(|c| c.set(Some(0)));
            assert_eq!(process(&plane, &event, &mut out), 0);
            assert_eq!(mradm_dsp_live_tb_reset(handle, ptr::null_mut(), 0), 0);
            let expected = out;
            out.fill(19.);
            assert_ne!(
                process(
                    &plane,
                    &TbLiveCommand {
                        element: 10,
                        ..event
                    },
                    &mut out
                ),
                0
            );
            assert_eq!(out, [19.; 17 * 16]);
            plane.samples = out.as_ptr();
            assert_ne!(process(&plane, &event, &mut out), 0);
            assert_eq!(out, [19.; 17 * 16]);
            plane.samples = samples.as_ptr();
            assert_eq!(process(&plane, &event, &mut out), 0);
            assert_eq!(COUNT.with(|c| c.replace(None).unwrap()), 0);
            assert_eq!(out, expected);
            mradm_dsp_live_tb_destroy(handle);
        }
    }
}
