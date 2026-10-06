use crate::{boundary, checked::*};
use mradm_dsp::scene_transition::{Status, Transitions};
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_transition_create(
    channels: usize,
    rate: u32,
    frames: u64,
    out: *mut *mut Transitions,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        range(out, 1)?;
        let value = Transitions::new(channels, rate, frames)?;
        *out = Box::into_raw(Box::new(value));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_transition_destroy(h: *mut Transitions) {
    if !h.is_null() {
        unsafe {
            drop(Box::from_raw(h));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_transition_control(
    h: *mut Transitions,
    command: u32,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        range(h, 1)?;
        match command {
            0 => (*h).reset(),
            1 => (*h).reset_backend(),
            2 => (*h).begin_generation(),
            _ => return Err(invalid()),
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_transition_status(
    h: *const Transitions,
    out: *mut Status,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(h, 1, out, 1)?;
        *out = (*h).status();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_transition_mix(
    h: *mut Transitions,
    old: *mut f32,
    old_len: usize,
    new: *const f32,
    new_len: usize,
    frames: usize,
    done: *mut u32,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(h, 1, old, old_len)?;
        separate(h, 1, new, new_len)?;
        separate(h, 1, done, 1)?;
        separate(old, old_len, new, new_len)?;
        separate(old, old_len, done, 1)?;
        separate(new, new_len, done, 1)?;
        let complete = (*h).mix(output(old, old_len)?, input(new, new_len)?, frames)?;
        *done = u32::from(complete);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_transition_output(
    h: *mut Transitions,
    pcm: *mut f32,
    length: usize,
    frames: usize,
    silence: u32,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(h, 1, pcm, length)?;
        let silence = flag(silence)?;
        (*h).process_output(output(pcm, length)?, frames, silence)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_transition_snapshot(
    h: *const Transitions,
    last: *mut f32,
    last_len: usize,
    anchor: *mut f32,
    anchor_len: usize,
    status: *mut Status,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(h, 1, last, last_len)?;
        separate(h, 1, anchor, anchor_len)?;
        separate(h, 1, status, 1)?;
        separate(last, last_len, anchor, anchor_len)?;
        separate(last, last_len, status, 1)?;
        separate(anchor, anchor_len, status, 1)?;
        let result = (*h).snapshot(output(last, last_len)?, output(anchor, anchor_len)?)?;
        *status = result;
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;
    #[test]
    fn first_calls_alias_rejection_atomicity_and_reset() {
        unsafe {
            let mut h = std::ptr::null_mut();
            assert_eq!(mradm_dsp_scene_transition_create(2, 400, 4, &mut h), 0);
            let mut pcm = [0.; 8];
            let incoming = [1.; 8];
            let mut done = 13;
            let mut status = Status::default();
            let mut last = [17.; 2];
            let mut anchor = [19.; 2];
            COUNT.with(|c| c.set(Some(0)));
            assert_eq!(mradm_dsp_scene_transition_control(h, 2), 0);
            assert_eq!(
                mradm_dsp_scene_transition_mix(
                    h,
                    pcm.as_mut_ptr(),
                    8,
                    incoming.as_ptr(),
                    8,
                    4,
                    &mut done
                ),
                0
            );
            assert_eq!(done, 1);
            assert_eq!(pcm, [0.25, 0.25, 0.5, 0.5, 0.75, 0.75, 1., 1.]);
            assert_eq!(
                mradm_dsp_scene_transition_output(h, pcm.as_mut_ptr(), 8, 2, 0),
                0
            );
            assert_eq!(
                mradm_dsp_scene_transition_snapshot(
                    h,
                    last.as_mut_ptr(),
                    2,
                    anchor.as_mut_ptr(),
                    2,
                    &mut status
                ),
                0
            );
            let before_pcm = pcm;
            let before_status = status;
            done = 99;
            assert_eq!(
                mradm_dsp_scene_transition_mix(
                    h,
                    pcm.as_mut_ptr(),
                    8,
                    pcm.as_ptr(),
                    8,
                    4,
                    &mut done
                ),
                1
            );
            assert_eq!(done, 99);
            assert_eq!(pcm, before_pcm);
            assert_eq!(
                mradm_dsp_scene_transition_output(h, pcm.as_mut_ptr(), 8, 4, 2),
                1
            );
            assert_eq!(mradm_dsp_scene_transition_control(h, 99), 1);
            assert_eq!(mradm_dsp_scene_transition_status(h, &mut status), 0);
            assert_eq!(status, before_status);
            assert_eq!(mradm_dsp_scene_transition_control(h, 0), 0);
            assert_eq!(
                mradm_dsp_scene_transition_output(h, pcm.as_mut_ptr(), 8, 4, 1),
                0
            );
            assert_eq!(pcm, [0.; 8]);
            assert_eq!(COUNT.with(|c| c.replace(None).unwrap()), 0);
            mradm_dsp_scene_transition_destroy(h);
        }
    }
}
