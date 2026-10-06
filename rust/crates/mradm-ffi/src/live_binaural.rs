use crate::{boundary, checked::*};
use mradm_dsp::{
    hrtf_filters::Filters,
    live_binaural::{Command, Control, Description, Report, Session},
};
use std::{ptr, slice, sync::Arc};
#[derive(Clone, Copy)]
#[repr(C)]
pub struct Plane {
    samples: *const f32,
    length: usize,
    has_signal: u32,
    reserved: u32,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_binaural_create(
    filters: *const Arc<Filters>,
    descriptions: *const Description,
    count: usize,
    rate: u32,
    spread: u32,
    contract: u32,
    out: *mut *mut Session,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(filters, 1, out, 1)?;
        separate(descriptions, count, out, 1)?;
        let contract = flag(contract)?;
        let value = Session::new(
            Arc::clone(&*filters),
            input(descriptions, count)?,
            rate,
            spread,
            contract,
        )?;
        *out = Box::into_raw(Box::new(value));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_binaural_destroy(h: *mut Session) {
    if !h.is_null() {
        unsafe {
            drop(Box::from_raw(h));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_binaural_reset(h: *mut Session) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        range(h, 1)?;
        (*h).reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_binaural_process(
    h: *mut Session,
    frames: u32,
    planes: *const Plane,
    plane_count: usize,
    initial: *const Command,
    initial_count: usize,
    events: *const Command,
    event_count: usize,
    pose: *const f32,
    pose_len: usize,
    warned: u32,
    pcm: *mut f32,
    pcm_len: usize,
    report: *mut Report,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(h, 1, planes, plane_count)?;
        separate(h, 1, initial, initial_count)?;
        separate(h, 1, events, event_count)?;
        separate(h, 1, pose, pose_len)?;
        separate(h, 1, pcm, pcm_len)?;
        separate(h, 1, report, 1)?;
        separate(pcm, pcm_len, planes, plane_count)?;
        separate(pcm, pcm_len, initial, initial_count)?;
        separate(pcm, pcm_len, events, event_count)?;
        separate(pcm, pcm_len, pose, pose_len)?;
        separate(pcm, pcm_len, report, 1)?;
        separate(report, 1, planes, plane_count)?;
        separate(report, 1, initial, initial_count)?;
        separate(report, 1, events, event_count)?;
        separate(report, 1, pose, pose_len)?;
        if pose_len != 3 {
            return Err(invalid());
        }
        let planes = input(planes, plane_count)?;
        for p in planes {
            if p.has_signal > 1 || p.reserved != 0 {
                return Err(invalid());
            }
            if p.has_signal != 0 {
                separate(p.samples, p.length, h, 1)?;
                separate(p.samples, p.length, pcm, pcm_len)?;
                separate(p.samples, p.length, report, 1)?;
            }
        }
        let views = planes.iter().map(|p| {
            if p.has_signal == 0 {
                None
            } else if p.length == 0 {
                Some(&[][..])
            } else {
                Some(slice::from_raw_parts(p.samples, p.length))
            }
        });
        let p = input(pose, pose_len)?;
        let result = (*h).process(
            frames,
            views,
            input(initial, initial_count)?,
            input(events, event_count)?,
            [p[0], p[1], p[2]],
            warned,
            output(pcm, pcm_len)?,
        )?;
        *report = result;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_live_binaural_control(
    h: *const Session,
    index: u32,
    out: *mut Control,
) -> i32 {
    boundary(ptr::null_mut(), 0, || unsafe {
        separate(h, 1, out, 1)?;
        let result = (*h).control(index as usize)?;
        *out = result;
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;
    use mradm_dsp::hrtf::Grid;
    unsafe fn render(
        h: *mut Session,
        n: u32,
        planes: &[Plane],
        initial: &[Command],
        events: &[Command],
        out: &mut [f32],
        report: &mut Report,
    ) -> i32 {
        unsafe {
            mradm_dsp_live_binaural_process(
                h,
                n,
                planes.as_ptr(),
                planes.len(),
                initial.as_ptr(),
                initial.len(),
                events.as_ptr(),
                events.len(),
                [0f32; 3].as_ptr(),
                3,
                0,
                out.as_mut_ptr(),
                out.len(),
                report,
                ptr::null_mut(),
                0,
            )
        }
    }
    #[test]
    fn allocation_atomicity_aliasing_and_shared_bank_lifetime() {
        unsafe {
            let grid = Arc::new(
                Grid::new(&[0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.]).unwrap(),
            );
            let mut ir = [0.; 192];
            for i in 0..12 {
                ir[i * 16] = 1.;
            }
            let bank = Arc::new(Filters::new(grid, &ir, 16, 64, true).unwrap());
            let d = Description::default();
            let mut h = ptr::null_mut();
            assert_eq!(
                mradm_dsp_live_binaural_create(
                    &bank,
                    &d,
                    1,
                    48000,
                    0,
                    0,
                    &mut h,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(Arc::strong_count(&bank), 2);
            drop(bank);
            let samples = [0.125; 1025];
            let planes = [Plane {
                samples: samples.as_ptr(),
                length: samples.len(),
                has_signal: 1,
                reserved: 0,
            }];
            let mut output = [17.; 2050];
            let mut report = Report::default();
            let initial = Command::default();
            let mut status = Control::default();
            COUNT.with(|c| c.set(Some(0)));
            assert_eq!(
                render(h, 1025, &planes, &[initial], &[], &mut output, &mut report),
                0
            );
            assert_eq!(mradm_dsp_live_binaural_control(h, 0, &mut status), 0);
            let before = status;
            let before_pcm = output;
            let before_report = report;
            let invalid = Command {
                element: 1,
                offset: 1024,
                ..Command::default()
            };
            assert_eq!(
                render(h, 1025, &planes, &[], &[invalid], &mut output, &mut report),
                1
            );
            assert_eq!(output, before_pcm);
            assert_eq!(report, before_report);
            assert_eq!(mradm_dsp_live_binaural_control(h, 0, &mut status), 0);
            assert_eq!(status, before);
            let alias = [Plane {
                samples: output.as_ptr(),
                length: output.len(),
                has_signal: 1,
                reserved: 0,
            }];
            assert_eq!(render(h, 1, &alias, &[], &[], &mut output, &mut report), 1);
            assert_eq!(output, before_pcm);
            assert_eq!(
                render(h, 1025, &planes, &[], &[], &mut output[..2049], &mut report),
                1
            );
            assert_eq!(
                render(h, 31, &planes, &[], &[], &mut output, &mut report),
                0
            );
            assert_eq!(mradm_dsp_live_binaural_reset(h), 0);
            assert_eq!(render(h, 31, &[], &[], &[], &mut output, &mut report), 0);
            assert_eq!(COUNT.with(|c| c.replace(None).unwrap()), 0);
            mradm_dsp_live_binaural_destroy(h);
        }
    }
}
