use crate::{boundary, ptr, slice};
use mradm_dsp::{
    Error, Result,
    pcm_mix::{Block, Coefficients, Matrix, Mixer, Plan, Row},
};
use std::sync::Arc;

#[repr(C)]
pub struct RowInput {
    input_channel: usize,
    block_offset: usize,
    block_count: usize,
    output_gain: f32,
}
#[repr(C)]
pub struct BlockInput {
    start: u64,
    end: u64,
    interpolation: u64,
    flags: u32,
}
impl BlockInput {
    fn block(&self) -> Block {
        Block {
            start: self.start,
            end: self.end,
            interpolation: self.interpolation,
            flags: self.flags,
        }
    }
}
fn invalid() -> Error {
    Error::InvalidArgument("Invalid PCM mix FFI arguments")
}
unsafe fn input<'a, T>(p: *const T, n: usize) -> Result<&'a [T]> {
    if n > isize::MAX as usize / size_of::<T>() || (n != 0 && p.is_null()) {
        return Err(invalid());
    }
    Ok(if n == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(p, n) }
    })
}
unsafe fn output<'a, T>(p: *mut T, n: usize) -> Result<&'a mut [T]> {
    if n > isize::MAX as usize / size_of::<T>() || (n != 0 && p.is_null()) {
        return Err(invalid());
    }
    Ok(if n == 0 {
        &mut []
    } else {
        unsafe { slice::from_raw_parts_mut(p, n) }
    })
}
unsafe fn create<T>(out: *mut *mut T, make: impl FnOnce() -> Result<T>) -> Result<()> {
    if out.is_null() {
        return Err(invalid());
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
unsafe fn destroy<T>(p: *mut T) {
    if !p.is_null() {
        unsafe {
            drop(Box::from_raw(p));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_plan_create(
    inputs: usize,
    outputs: usize,
    rows: *const RowInput,
    row_count: usize,
    blocks: *const BlockInput,
    block_count: usize,
    gains_f32: *const f32,
    len_f32: usize,
    gains_f64: *const f64,
    len_f64: usize,
    kind: u32,
    out: *mut *mut Arc<Plan>,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            let coefficients = match kind {
                0 if len_f64 == 0 => Coefficients::Speaker(input(gains_f32, len_f32)?.to_vec()),
                1 if len_f32 == 0 => Coefficients::Ear(input(gains_f64, len_f64)?.to_vec()),
                _ => return Err(invalid()),
            };
            let rows = input(rows, row_count)?
                .iter()
                .map(|r| Row {
                    input_channel: r.input_channel,
                    block_offset: r.block_offset,
                    block_count: r.block_count,
                    output_gain: r.output_gain,
                })
                .collect();
            let blocks = input(blocks, block_count)?
                .iter()
                .map(BlockInput::block)
                .collect();
            Ok(Arc::new(Plan::new(
                inputs,
                outputs,
                rows,
                blocks,
                coefficients,
            )?))
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_plan_destroy(plan: *mut Arc<Plan>) {
    unsafe {
        destroy(plan);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_create(
    plan: *const Arc<Plan>,
    max_frames: usize,
    interpolation: u64,
    smoothing: u32,
    out: *mut *mut Mixer,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            if smoothing > 1 {
                return Err(invalid());
            }
            let plan = plan.as_ref().ok_or_else(invalid)?;
            Mixer::new(Arc::clone(plan), max_frames, interpolation, smoothing != 0)
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_dynamic_create(
    inputs: usize,
    outputs: usize,
    channels: *const usize,
    rows: usize,
    block_capacity: usize,
    max_frames: usize,
    interpolation: u64,
    out: *mut *mut Mixer,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            Mixer::dynamic(
                inputs,
                outputs,
                input(channels, rows)?,
                block_capacity,
                max_frames,
                interpolation,
            )
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_destroy(mixer: *mut Mixer) {
    unsafe {
        destroy(mixer);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_reset(
    mixer: *mut Mixer,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        let m = unsafe { mixer.as_mut() }.ok_or_else(invalid)?;
        m.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_update(
    mixer: *mut Mixer,
    row: usize,
    blocks: *const BlockInput,
    block_count: usize,
    gains: *const f32,
    gain_count: usize,
    output_gain: f32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let m = mixer.as_mut().ok_or_else(invalid)?;
        m.update(
            row,
            input(blocks, block_count)?.iter().map(BlockInput::block),
            input(gains, gain_count)?,
            output_gain,
        )
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_speaker(
    mixer: *mut Mixer,
    row: usize,
    pcm: *const f32,
    pcm_len: usize,
    out: *mut f32,
    out_len: usize,
    live: *const f32,
    live_len: usize,
    start: u64,
    frames: usize,
    override_gain: u32,
    gain: f32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        if override_gain > 1 {
            return Err(invalid());
        }
        let m = mixer.as_mut().ok_or_else(invalid)?;
        m.speaker(
            input(pcm, pcm_len)?,
            output(out, out_len)?,
            input(live, live_len)?,
            start,
            frames,
            (row != usize::MAX).then_some(row),
            (override_gain != 0).then_some(gain),
        )
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_mix_ear(
    mixer: *mut Mixer,
    pcm: *const f32,
    pcm_len: usize,
    direct: *mut f32,
    direct_len: usize,
    diffuse: *mut f32,
    diffuse_len: usize,
    start: u64,
    frames: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let m = mixer.as_mut().ok_or_else(invalid)?;
        m.ear(
            input(pcm, pcm_len)?,
            output(direct, direct_len)?,
            output(diffuse, diffuse_len)?,
            start,
            frames,
        )
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_matrix_create(
    inputs: usize,
    outputs: usize,
    gains: *const f32,
    length: usize,
    out: *mut *mut Matrix,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Matrix::new(inputs, outputs, input(gains, length)?))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_matrix_destroy(matrix: *mut Matrix) {
    unsafe {
        destroy(matrix);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_matrix_process(
    matrix: *const Matrix,
    pcm: *const f32,
    pcm_len: usize,
    out: *mut f32,
    out_len: usize,
    frames: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        matrix.as_ref().ok_or_else(invalid)?.process(
            input(pcm, pcm_len)?,
            output(out, out_len)?,
            frames,
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn plan_lifetime_and_rejected_processing_preserve_output_and_cursor() {
        unsafe {
            let rows = [RowInput {
                input_channel: 0,
                block_offset: 0,
                block_count: 2,
                output_gain: 1.,
            }];
            let blocks = [
                BlockInput {
                    start: 0,
                    end: 4,
                    interpolation: 0,
                    flags: 1,
                },
                BlockInput {
                    start: 4,
                    end: 8,
                    interpolation: 0,
                    flags: 1,
                },
            ];
            let gains = [0.5, 1.];
            let mut plan = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_mix_plan_create(
                    1,
                    1,
                    ptr::null(),
                    1,
                    blocks.as_ptr(),
                    2,
                    gains.as_ptr(),
                    2,
                    ptr::null(),
                    0,
                    0,
                    &mut plan,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert!(plan.is_null());
            assert_eq!(
                mradm_dsp_mix_plan_create(
                    1,
                    1,
                    rows.as_ptr(),
                    1,
                    blocks.as_ptr(),
                    2,
                    gains.as_ptr(),
                    2,
                    ptr::null(),
                    0,
                    0,
                    &mut plan,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            let mut mix = ptr::null_mut();
            assert_eq!(
                mradm_dsp_mix_create(plan, 8, 0, 2, &mut mix, ptr::null_mut(), 0),
                1
            );
            assert!(mix.is_null());
            assert_eq!(
                mradm_dsp_mix_create(plan, 8, 0, 0, &mut mix, ptr::null_mut(), 0),
                0
            );
            mradm_dsp_mix_plan_destroy(plan);
            let input = [1.; 8];
            let mut out = [42.; 8];
            for (row, pcm_len, out_len, start, frames, flag) in [
                (2, 8, 8, 0, 8, 0),
                (usize::MAX, usize::MAX, 8, 0, 8, 0),
                (usize::MAX, 8, 1, 0, 8, 0),
                (usize::MAX, 8, 8, u64::MAX, 8, 0),
                (usize::MAX, 8, 8, 0, 8, 2),
            ] {
                assert_eq!(
                    mradm_dsp_mix_speaker(
                        mix,
                        row,
                        input.as_ptr(),
                        pcm_len,
                        out.as_mut_ptr(),
                        out_len,
                        ptr::null(),
                        0,
                        start,
                        frames,
                        flag,
                        1.,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert_eq!(out, [42.; 8]);
            }
            assert_eq!(
                mradm_dsp_mix_speaker(
                    mix,
                    usize::MAX,
                    ptr::null(),
                    1,
                    out.as_mut_ptr(),
                    8,
                    ptr::null(),
                    0,
                    0,
                    1,
                    0,
                    1.,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_mix_speaker(
                    mix,
                    usize::MAX,
                    input.as_ptr(),
                    8,
                    out.as_mut_ptr(),
                    8,
                    ptr::null(),
                    0,
                    0,
                    8,
                    0,
                    1.,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(out, [42.5, 42.5, 42.5, 42.5, 43., 43., 43., 43.]);
            assert_eq!(mradm_dsp_mix_reset(mix, ptr::null_mut(), 0), 0);
            assert_eq!(
                mradm_dsp_mix_speaker(
                    mix,
                    usize::MAX,
                    ptr::null(),
                    0,
                    ptr::null_mut(),
                    0,
                    ptr::null(),
                    0,
                    u64::MAX,
                    0,
                    0,
                    1.,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            out.fill(0.);
            assert_eq!(
                mradm_dsp_mix_speaker(
                    mix,
                    usize::MAX,
                    input.as_ptr(),
                    1,
                    out.as_mut_ptr(),
                    1,
                    ptr::null(),
                    0,
                    0,
                    1,
                    0,
                    1.,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(out[0], 0.5);
            mradm_dsp_mix_destroy(mix);
            mradm_dsp_mix_destroy(ptr::null_mut());
            mradm_dsp_mix_plan_destroy(ptr::null_mut());
        }
    }
    #[test]
    fn invalid_dynamic_update_keeps_the_previous_curve() {
        unsafe {
            let mut mix = ptr::null_mut();
            let channels = [0];
            assert_eq!(
                mradm_dsp_mix_dynamic_create(
                    1,
                    1,
                    channels.as_ptr(),
                    1,
                    3,
                    16,
                    0,
                    &mut mix,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            let blocks = [BlockInput {
                start: 0,
                end: 16,
                interpolation: 0,
                flags: 1,
            }];
            let gains = [0.25];
            assert_eq!(
                mradm_dsp_mix_update(
                    mix,
                    0,
                    blocks.as_ptr(),
                    1,
                    gains.as_ptr(),
                    1,
                    1.,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                mradm_dsp_mix_update(
                    mix,
                    0,
                    blocks.as_ptr(),
                    1,
                    gains.as_ptr(),
                    0,
                    1.,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_mix_update(
                    mix,
                    0,
                    ptr::null(),
                    1,
                    gains.as_ptr(),
                    1,
                    1.,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            let mut out = [0.; 2];
            let input = [1.; 2];
            assert_eq!(
                mradm_dsp_mix_speaker(
                    mix,
                    0,
                    input.as_ptr(),
                    2,
                    out.as_mut_ptr(),
                    2,
                    ptr::null(),
                    0,
                    0,
                    2,
                    0,
                    1.,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(out, [0.25; 2]);
            mradm_dsp_mix_destroy(mix);
        }
    }
}
