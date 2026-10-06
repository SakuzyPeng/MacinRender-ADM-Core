//! Private HOA boundary. Mutable handles are exclusive; buffers are valid, aligned and disjoint.
use crate::boundary;
use mradm_dsp::{
    Error, Result,
    hoa::{BlockInput, Encoder, MeterPreprocessor, Plan, RowInput, Source, Trace},
};
use std::{ptr, slice, sync::Arc};
fn invalid() -> Error {
    Error::InvalidArgument("Invalid HOA FFI arguments")
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
unsafe fn get<'a, T>(p: *const T) -> Result<&'a T> {
    range(p, 1)?;
    Ok(unsafe { &*p })
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
    let result = make()?;
    unsafe {
        *out = Box::into_raw(Box::new(result));
    }
    Ok(())
}
fn flag(v: u32) -> Result<bool> {
    match v {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(invalid()),
    }
}
/// 释放 `Box::into_raw` 交出的句柄；空指针是无操作。
unsafe fn release<T>(p: *mut T) {
    if !p.is_null() {
        unsafe {
            drop(Box::from_raw(p));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_plan_destroy(p: *mut Arc<Plan>) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_encoder_destroy(p: *mut Encoder) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_meter_destroy(p: *mut MeterPreprocessor) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_plan_create(
    inputs: usize,
    rows: *const RowInput,
    n: usize,
    blocks: *const BlockInput,
    bn: usize,
    order: *const usize,
    on: usize,
    sources: *const Source,
    sn: usize,
    out: *mut *mut Arc<Plan>,
    trace: *mut Trace,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        if !trace.is_null() {
            range(trace, 1)?;
            separate(out, 1, trace, 1)?;
            separate(rows, n, trace, 1)?;
            separate(blocks, bn, trace, 1)?;
            separate(order, on, trace, 1)?;
            separate(sources, sn, trace, 1)?;
        }
        create(out, || {
            let (plan, stages) = Plan::new(
                inputs,
                input(rows, n)?,
                input(blocks, bn)?,
                input(order, on)?,
                input(sources, sn)?,
            )?;
            if !trace.is_null() {
                *trace = stages;
            }
            Ok(Arc::new(plan))
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_plan_has_lfe(
    plan: *const Arc<Plan>,
    out: *mut u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let value = get(plan)?.has_lfe();
        *get_mut(out)? = u32::from(value);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_coefficients(
    plan: *const Arc<Plan>,
    row: usize,
    block: usize,
    out: *mut f32,
    len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        if len != 64 {
            return Err(invalid());
        }
        let c = get(plan)?.coefficients(row, block)?;
        let out = output(out, len)?;
        out[..16].copy_from_slice(&c.direct);
        for slot in 0..3 {
            out[(slot + 1) * 16..(slot + 2) * 16].copy_from_slice(&c.diffuse[slot]);
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_encoder_create(
    plan: *const Arc<Plan>,
    max_frames: usize,
    interpolation: u64,
    smoothing: u32,
    out: *mut *mut Encoder,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            Encoder::new(
                Arc::clone(get(plan)?),
                max_frames,
                interpolation,
                flag(smoothing)?,
            )
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_encoder_reset(
    p: *mut Encoder,
    start: u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.reset(start);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_encode(
    p: *mut Encoder,
    src: *const f32,
    n: usize,
    out: *mut f32,
    len: usize,
    start: u64,
    frames: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(src, n, out, len)?;
        get_mut(p)?.process(input(src, n)?, output(out, len)?, start, frames)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_meter_create(
    plan: *const Arc<Plan>,
    max_frames: usize,
    interpolation: u64,
    out: *mut *mut MeterPreprocessor,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            MeterPreprocessor::new(Arc::clone(get(plan)?), max_frames, interpolation)
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_meter_reset(
    p: *mut MeterPreprocessor,
    start: u64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.reset(start);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_meter_process(
    p: *mut MeterPreprocessor,
    src: *const f32,
    n: usize,
    hoa: *const f32,
    hn: usize,
    decoded: *mut f32,
    dn: usize,
    lfe: *mut f32,
    ln: usize,
    start: u64,
    frames: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(src, n, decoded, dn)?;
        separate(src, n, lfe, ln)?;
        separate(hoa, hn, decoded, dn)?;
        separate(hoa, hn, lfe, ln)?;
        separate(decoded, dn, lfe, ln)?;
        get_mut(p)?.process(
            input(src, n)?,
            input(hoa, hn)?,
            output(decoded, dn)?,
            output(lfe, ln)?,
            start,
            frames,
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;
    #[test]
    fn lengths_aliasing_atomicity_lifetime_and_first_call_allocations() {
        unsafe {
            let mut message = [0u8; 256];
            let msg = message.as_mut_ptr();
            let cap = message.len();
            let row = RowInput {
                input: 0,
                block_offset: 0,
                block_count: 1,
            };
            let block = BlockInput {
                end: 10000,
                source_count: 1,
                object_gain: 0.75,
                ..BlockInput::default()
            };
            let source = Source {
                position: [30., 15., 1.],
                gain: 1.,
                diffuse: 0.5,
                ..Source::default()
            };
            let mut p = ptr::null_mut();
            let mut trace = Trace::default();
            assert_eq!(
                mradm_dsp_hoa_plan_create(
                    1,
                    &row,
                    1,
                    &block,
                    1,
                    [0usize].as_ptr(),
                    1,
                    &source,
                    1,
                    &mut p,
                    &mut trace,
                    msg,
                    cap
                ),
                0
            );
            assert_eq!(trace.flags, 3);
            let mut a = ptr::null_mut();
            let mut b = ptr::null_mut();
            let mut meter = ptr::null_mut();
            assert_eq!(
                mradm_dsp_hoa_encoder_create(p, 1024, 240, 0, &mut a, msg, cap),
                0
            );
            assert_eq!(
                mradm_dsp_hoa_encoder_create(p, 1024, 240, 0, &mut b, msg, cap),
                0
            );
            assert_eq!(
                mradm_dsp_hoa_meter_create(p, 1024, 240, &mut meter, msg, cap),
                0
            );
            mradm_dsp_hoa_plan_destroy(p);
            let src = [0.25; 1024];
            let mut x = [0.125; 16384];
            let mut y = x;
            let mut decoded = [0.125; 12288];
            let mut lfe = [0.125; 1024];
            COUNT.with(|c| c.set(Some(0)));
            assert_ne!(
                mradm_dsp_hoa_encode(
                    a,
                    src.as_ptr(),
                    1023,
                    x.as_mut_ptr(),
                    x.len(),
                    0,
                    1024,
                    msg,
                    cap
                ),
                0
            );
            assert_ne!(
                mradm_dsp_hoa_encode(a, src.as_ptr(), 1024, x.as_mut_ptr(), 15, 0, 1, msg, cap),
                0
            );
            assert_ne!(
                mradm_dsp_hoa_encode(
                    a,
                    ptr::null(),
                    usize::MAX,
                    x.as_mut_ptr(),
                    x.len(),
                    0,
                    1024,
                    msg,
                    cap
                ),
                0
            );
            assert_ne!(
                mradm_dsp_hoa_encode(
                    a,
                    x.as_ptr(),
                    1024,
                    x.as_mut_ptr(),
                    x.len(),
                    0,
                    1024,
                    msg,
                    cap
                ),
                0
            );
            assert_ne!(
                mradm_dsp_hoa_encode(
                    a,
                    src.as_ptr(),
                    1024,
                    x.as_mut_ptr(),
                    x.len(),
                    u64::MAX,
                    1,
                    msg,
                    cap
                ),
                0
            );
            assert_eq!(x, y);
            assert_eq!(
                mradm_dsp_hoa_encode(
                    a,
                    src.as_ptr(),
                    1024,
                    x.as_mut_ptr(),
                    x.len(),
                    0,
                    1024,
                    msg,
                    cap
                ),
                0
            );
            assert_eq!(
                mradm_dsp_hoa_encode(
                    b,
                    src.as_ptr(),
                    1024,
                    y.as_mut_ptr(),
                    y.len(),
                    0,
                    1024,
                    msg,
                    cap
                ),
                0
            );
            assert_eq!(x, y);
            assert_ne!(
                mradm_dsp_hoa_meter_process(
                    meter,
                    src.as_ptr(),
                    1024,
                    x.as_ptr(),
                    x.len(),
                    decoded.as_mut_ptr(),
                    decoded.len(),
                    decoded.as_mut_ptr(),
                    1024,
                    0,
                    1024,
                    msg,
                    cap
                ),
                0
            );
            assert!(decoded.iter().all(|v| *v == 0.125));
            assert!(lfe.iter().all(|v| *v == 0.125));
            assert_eq!(
                mradm_dsp_hoa_meter_process(
                    meter,
                    src.as_ptr(),
                    1024,
                    x.as_ptr(),
                    x.len(),
                    decoded.as_mut_ptr(),
                    decoded.len(),
                    lfe.as_mut_ptr(),
                    1024,
                    0,
                    1024,
                    msg,
                    cap
                ),
                0
            );
            assert!(lfe.iter().all(|v| *v == 0.));
            assert!(decoded.chunks(12).all(|f| f[3] == 0.));
            assert_eq!(mradm_dsp_hoa_encoder_reset(a, 37, msg, cap), 0);
            assert_eq!(mradm_dsp_hoa_meter_reset(meter, 37, msg, cap), 0);
            assert_eq!(
                mradm_dsp_hoa_encode(a, ptr::null(), 0, ptr::null_mut(), 0, 37, 0, msg, cap),
                0
            );
            assert_eq!(
                mradm_dsp_hoa_meter_process(
                    meter,
                    ptr::null(),
                    0,
                    ptr::null(),
                    0,
                    ptr::null_mut(),
                    0,
                    ptr::null_mut(),
                    0,
                    37,
                    0,
                    msg,
                    cap
                ),
                0
            );
            let allocations = COUNT.with(|c| c.replace(None).unwrap());
            assert_eq!(allocations, 0);
            mradm_dsp_hoa_encoder_destroy(a);
            mradm_dsp_hoa_encoder_destroy(b);
            mradm_dsp_hoa_meter_destroy(meter);
        }
    }
}
