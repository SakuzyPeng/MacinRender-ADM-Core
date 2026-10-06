use crate::{boundary, checked::*};
use mradm_dsp::scene_math::{self as math, CloudPoint, Rotation, Speaker};

/// Scalar operations are batches of fixed strides. These are arithmetic helpers;
/// unlike renderer commands, they retain IEEE nonfinite propagation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_math(
    op: u32,
    src: *const f32,
    n: usize,
    dst: *mut f32,
    m: usize,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(src, n, dst, m)?;
        let contract = op & 256 != 0;
        let op = op & !256;
        let (stride, width) = match op {
            0 => (3, 3),
            1 => (2, 3),
            2 => (3, 1),
            3 => (6, 1),
            4 => (1, 1),
            5 => (4, 2),
            6 | 7 => (6, 1),
            8 => (6, 3),
            9 => (6, 3),
            _ => return Err(invalid()),
        };
        if !n.is_multiple_of(stride) || n / stride * width != m {
            return Err(invalid());
        }
        let src = input(src, n)?;
        let dst = output(dst, m)?;
        for (s, d) in src.chunks_exact(stride).zip(dst.chunks_exact_mut(width)) {
            match op {
                0 => d.copy_from_slice(&math::cartesian_to_polar([s[0], s[1], s[2]])),
                1 => d.copy_from_slice(&math::direction(s[0], s[1])),
                2 => d[0] = math::length([s[0], s[1], s[2]]),
                3 => d[0] = math::distance_compat([s[0], s[1], s[2]], [s[3], s[4], s[5]], contract),
                4 => d[0] = math::wrap(s[0]),
                5 => d.copy_from_slice(&math::radii(s[0], s[1], s[2], s[3], contract)),
                6 | 7 => d[0] = math::spread([s[0], s[1], s[2]], s[3], s[4], s[5], op == 7),
                8 => d.copy_from_slice(&[
                    s[0] + s[3],
                    math::clamp(s[1] + s[4], -90., 90.),
                    math::max(0., s[2] + s[5]),
                ]),
                9 => d.copy_from_slice(&[s[0] + s[3], s[1] + s[4], s[2] + s[5]]),
                _ => unreachable!(),
            }
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_cloud(
    src: *const f32,
    n: usize,
    cartesian: u32,
    binaural: u32,
    dst: *mut CloudPoint,
    capacity: usize,
    count: *mut usize,
    trace: *mut f32,
    trace_len: usize,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(src, n, dst, capacity)?;
        separate(src, n, count, 1)?;
        separate(dst, capacity, count, 1)?;
        separate(trace, trace_len, src, n)?;
        separate(trace, trace_len, dst, capacity)?;
        separate(trace, trace_len, count, 1)?;
        if n != 7 || capacity < 17 || !matches!(trace_len, 0 | 238) {
            return Err(invalid());
        }
        let cart = flag(cartesian)?;
        let bin = flag(binaural & !256)?;
        let contract = binaural & 256 != 0;
        let s = input(src, n)?;
        let mut points = [CloudPoint::default(); 17];
        let mut traces = [[0f32; 14]; 17];
        let len = math::cloud(
            [s[0], s[1], s[2]],
            cart,
            [s[3], s[4], s[5]],
            s[6],
            bin,
            contract,
            &mut points,
            if trace_len == 0 {
                None
            } else {
                Some(&mut traces)
            },
        );
        output(dst, capacity)?[..len].copy_from_slice(&points[..len]);
        if trace_len != 0 {
            output(trace, trace_len)?.copy_from_slice(traces.as_flattened());
        }
        *count = len;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_divergence(
    src: *const f32,
    n: usize,
    cartesian: u32,
    dst: *mut CloudPoint,
    capacity: usize,
    count: *mut usize,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(src, n, dst, capacity)?;
        separate(src, n, count, 1)?;
        separate(dst, capacity, count, 1)?;
        if n != 7 || capacity < 3 {
            return Err(invalid());
        }
        let cart = flag(cartesian)?;
        let s = input(src, n)?;
        let mut points = [CloudPoint::default(); 3];
        let len = math::divergence(
            [s[0], s[1], s[2]],
            cart,
            s[3],
            s[4],
            s[5],
            s[6],
            &mut points,
        );
        output(dst, capacity)?[..len].copy_from_slice(&points[..len]);
        *count = len;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_nearest(
    src: *const f32,
    n: usize,
    cartesian: u32,
    speakers: *const Speaker,
    speaker_count: usize,
    index: *mut usize,
    distance: *mut f32,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(src, n, index, 1)?;
        separate(src, n, distance, 1)?;
        separate(speakers, speaker_count, index, 1)?;
        separate(speakers, speaker_count, distance, 1)?;
        separate(index, 1, distance, 1)?;
        if n != 3 {
            return Err(invalid());
        }
        let cart = flag(cartesian & !256)?;
        let contract = cartesian & 256 != 0;
        let s = input(src, n)?;
        let speakers = input(speakers, speaker_count)?;
        if speakers.iter().any(|s| s.is_lfe > 1) {
            return Err(invalid());
        }
        let result = math::nearest_compat([s[0], s[1], s[2]], cart, speakers, contract)
            .unwrap_or((usize::MAX, f32::MAX));
        *index = result.0;
        *distance = result.1;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_rotation_create(
    pose: *const f32,
    n: usize,
    contract: u32,
    out: *mut *mut Rotation,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(pose, n, out, 1)?;
        if n != 3 {
            return Err(invalid());
        }
        let p = input(pose, n)?;
        let r = Rotation::new_compat([p[0], p[1], p[2]], flag(contract)?)?;
        *out = Box::into_raw(Box::new(r));
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_rotation_destroy(h: *mut Rotation) {
    if !h.is_null() {
        unsafe {
            drop(Box::from_raw(h));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_rotation_update(
    h: *mut Rotation,
    pose: *const f32,
    n: usize,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(h, 1, pose, n)?;
        if n != 3 {
            return Err(invalid());
        }
        let p = input(pose, n)?;
        (*h).update([p[0], p[1], p[2]])?;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_rotation_apply(
    h: *const Rotation,
    src: *const f32,
    n: usize,
    dst: *mut f32,
    m: usize,
    apple: u32,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(h, 1, dst, m)?;
        separate(src, n, dst, m)?;
        range(h, 1)?;
        if n != m || !n.is_multiple_of(2) {
            return Err(invalid());
        }
        let apple = flag(apple)?;
        let src = input(src, n)?;
        if src.iter().any(|x| !x.is_finite()) {
            return Err(invalid());
        }
        for (s, d) in src
            .as_chunks::<2>()
            .0
            .iter()
            .zip(output(dst, m)?.as_chunks_mut::<2>().0.iter_mut())
        {
            d.copy_from_slice(&(*h).apply(s[0], s[1], apple));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_pose(
    src: *const f32,
    n: usize,
    dst: *mut f32,
    m: usize,
    quaternion: u32,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(src, n, dst, m)?;
        if m != 7 {
            return Err(invalid());
        }
        let q = flag(quaternion)?;
        let result = math::pose(input(src, n)?, q)?;
        output(dst, m)?.copy_from_slice(&result);
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_scene_rotate_pose(
    src: *const f32,
    n: usize,
    dst: *mut f32,
    m: usize,
    apple: u32,
) -> i32 {
    boundary(std::ptr::null_mut(), 0, || unsafe {
        separate(src, n, dst, m)?;
        if n != 5 || m != 2 {
            return Err(invalid());
        }
        let contract = apple & 256 != 0;
        let apple = flag(apple & !256)?;
        let s = input(src, n)?;
        if s.iter().any(|x| !x.is_finite()) {
            return Err(invalid());
        }
        let rotation = Rotation::new_compat([s[2], s[3], s[4]], contract)?;
        let result = rotation.apply(s[0], s[1], apple);
        output(dst, m)?.copy_from_slice(&result);
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;
    #[test]
    fn first_calls_no_allocation_and_atomic_errors() {
        unsafe {
            let pose = [0f32; 3];
            let mut handle = std::ptr::null_mut();
            assert_eq!(
                mradm_dsp_scene_rotation_create(pose.as_ptr(), 3, 0, &mut handle),
                0
            );
            let input = [0., 0., 1., 0.5, 0.5, 0.5, 1.];
            let mut points = [CloudPoint::default(); 17];
            let mut count = 999;
            let mut result = [17f32; 7];
            COUNT.with(|c| c.set(Some(0)));
            assert_eq!(
                mradm_dsp_scene_cloud(
                    input.as_ptr(),
                    7,
                    0,
                    0,
                    points.as_mut_ptr(),
                    17,
                    &mut count,
                    std::ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(count, 16);
            assert_eq!(
                mradm_dsp_scene_pose(pose.as_ptr(), 3, result.as_mut_ptr(), 7, 0),
                0
            );
            let before = result;
            assert_eq!(
                mradm_dsp_scene_pose(pose.as_ptr(), 3, result.as_mut_ptr(), 6, 0),
                1
            );
            assert_eq!(result, before);
            assert_eq!(
                mradm_dsp_scene_rotation_apply(
                    handle,
                    input.as_ptr(),
                    2,
                    result.as_mut_ptr(),
                    2,
                    0
                ),
                0
            );
            assert_eq!(mradm_dsp_scene_rotation_update(handle, pose.as_ptr(), 3), 0);
            let before = result;
            assert_eq!(
                mradm_dsp_scene_math(0, result.as_ptr(), 3, result.as_mut_ptr(), 3),
                1
            );
            assert_eq!(result, before);
            assert_eq!(
                mradm_dsp_scene_rotation_update(handle, [f32::NAN, 0., 0.].as_ptr(), 3),
                1
            );
            assert_eq!(COUNT.with(|c| c.replace(None).unwrap()), 0);
            mradm_dsp_scene_rotation_destroy(handle);
        }
    }
}
