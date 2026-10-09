//! Private Triple Balance boundary. All audio buffers are disjoint; handles are exclusive.
use crate::boundary;
use mradm_dsp::{
    Error, Result, pcm_mix,
    triple_balance::{Event, Layout, Position, d_mode, panner, processor, session},
};
use std::{ptr, slice, sync::Arc};
fn invalid() -> Error {
    Error::InvalidArgument("Invalid Triple Balance FFI arguments")
}
fn flag(v: u32) -> Result<bool> {
    match v {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(invalid()),
    }
}
fn range<T>(p: *const T, n: usize) -> Result<(usize, usize)> {
    if n > isize::MAX as usize / size_of::<T>()
        || (n != 0 && (p.is_null() || !(p as usize).is_multiple_of(align_of::<T>())))
    {
        return Err(invalid());
    }
    let a = p as usize;
    Ok((a, a.checked_add(n * size_of::<T>()).ok_or_else(invalid)?))
}
fn separate<A, B>(a: *const A, an: usize, b: *const B, bn: usize) -> Result<()> {
    let (a, z) = range(a, an)?;
    let (b, y) = range(b, bn)?;
    if a < y && b < z && a != z && b != y {
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
    let value = make()?;
    unsafe {
        *out = Box::into_raw(Box::new(value));
    }
    Ok(())
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
pub unsafe extern "C" fn mradm_dsp_tb_d_plan_create(
    inputs: usize,
    layout: u32,
    total: u64,
    rows: *const session::TbRowInput,
    n: usize,
    events: *const Event,
    en: usize,
    bed: *const f32,
    bn: usize,
    out: *mut *mut Arc<d_mode::Plan>,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            Ok(Arc::new(d_mode::Plan::new(
                inputs,
                Layout::from_code(layout)?,
                total,
                input(rows, n)?,
                input(events, en)?,
                input(bed, bn)?,
            )?))
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_d_plan_destroy(p: *mut Arc<d_mode::Plan>) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_d_create(
    plan: *const Arc<d_mode::Plan>,
    out: *mut *mut d_mode::Session,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Ok(d_mode::Session::new(Arc::clone(get(plan)?))))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_d_destroy(p: *mut d_mode::Session) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_d_process(
    state: *mut d_mode::Session,
    src: *const f32,
    src_len: usize,
    out: *mut f32,
    out_len: usize,
    start: u64,
    frames: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        separate(src, src_len, out, out_len)?;
        get_mut(state)?.process(input(src, src_len)?, output(out, out_len)?, start, frames)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_plan_destroy(p: *mut Arc<session::Plan>) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_destroy(p: *mut session::Session) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_snapshot_destroy(p: *mut session::Snapshot) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_destroy(p: *mut processor::Processor) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_snapshot_destroy(p: *mut processor::Snapshot) {
    unsafe { release(p) }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_filter_destroy(p: *mut processor::Decorrelator) {
    unsafe { release(p) }
}
#[repr(C)]
pub struct Query {
    position: Position,
    size: f32,
    gain: f32,
    kind: u32,
}
fn evaluate(q: &Query, l: Layout) -> Result<panner::Gains> {
    match q.kind {
        0 => panner::point(q.position, q.gain, l),
        1 if l == Layout::Room222 => panner::room(q.position, q.size),
        _ => Err(invalid()),
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_gains(
    layout: u32,
    queries: *const Query,
    n: usize,
    out: *mut f32,
    len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let layout = Layout::from_code(layout)?;
        if n.checked_mul(24) != Some(len) {
            return Err(invalid());
        }
        separate(queries, n, out, len)?;
        let q = input(queries, n)?;
        for v in q {
            evaluate(v, layout)?;
        }
        let out = output(out, len)?;
        for (i, v) in q.iter().enumerate() {
            out[i * 24..(i + 1) * 24].copy_from_slice(&evaluate(v, layout)?);
        }
        Ok(())
    })
}
#[repr(C)]
pub struct Node {
    channel: usize,
    position: Position,
    filter: i32,
    sign: f32,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_nodes(
    out: *mut Node,
    len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        if len != 22 {
            return Err(invalid());
        }
        let out = output(out, len)?;
        for (o, n) in out.iter_mut().zip(panner::NODES) {
            *o = Node {
                channel: n.channel,
                position: n.position,
                filter: n.filter,
                sign: n.sign,
            };
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_quantize(
    p: Position,
    size: f32,
    out: *mut i32,
    len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        if len != 4 {
            return Err(invalid());
        }
        let q = panner::quantize(p, size)?;
        output(out, len)?.copy_from_slice(&q);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_raw(
    q: *const i32,
    n: usize,
    out: *mut f32,
    len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        if n != 4 || len != 11 {
            return Err(invalid());
        }
        separate(q, n, out, len)?;
        let q = input(q, n)?.try_into().unwrap();
        let gains = panner::raw(q)?;
        output(out, len)?.copy_from_slice(&gains);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_size_mix(
    raw: *const f32,
    n: usize,
    size: f32,
    out: *mut f32,
    len: usize,
    extended: u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let extended = flag(extended)?;
        let channels = if extended { 24 } else { 11 };
        if n != channels || len != channels + 1 || !size.is_finite() || !(0. ..=1.).contains(&size)
        {
            return Err(invalid());
        }
        separate(raw, n, out, len)?;
        let raw = input(raw, n)?;
        if raw.iter().any(|v| !v.is_finite()) {
            return Err(invalid());
        }
        let mix = if extended {
            panner::room_mix(raw.try_into().unwrap(), size)
        } else {
            panner::size_mix(raw.try_into().unwrap(), size)
        };
        let out = output(out, len)?;
        out[0] = mix.direct;
        out[1..].copy_from_slice(&mix.spread[..channels]);
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_plan_create(
    inputs: usize,
    layout: u32,
    rate: u32,
    total: u64,
    rows: *const session::TbRowInput,
    n: usize,
    events: *const Event,
    en: usize,
    bed: *const f32,
    bn: usize,
    out: *mut *mut Arc<session::Plan>,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            Ok(Arc::new(session::Plan::new(
                inputs,
                Layout::from_code(layout)?,
                rate,
                total,
                input(rows, n)?,
                input(events, en)?,
                input(bed, bn)?,
            )?))
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_plan_mix(
    plan: *const Arc<session::Plan>,
    out: *mut *mut Arc<pcm_mix::Plan>,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Ok(Arc::clone(&get(plan)?.mix)))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_create(
    plan: *const Arc<session::Plan>,
    max: usize,
    interpolation: u64,
    live: u32,
    out: *mut *mut session::Session,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            session::Session::new(Arc::clone(get(plan)?), max, interpolation, flag(live)?)
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_reset(
    p: *mut session::Session,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_scales(
    p: *mut session::Session,
    scales: *const f32,
    len: usize,
    mode: u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let scales = input(scales, len)?;
        let p = get_mut(p)?;
        match mode {
            0 => p.set_scales(scales, false),
            1 => p.set_scales(scales, true),
            2 => p.set_matrix(scales),
            _ => Err(invalid()),
        }
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_prepare_points(
    p: *mut session::Session,
    start: u64,
    frames: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.prepare_points(start, frames)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_process(
    p: *mut session::Session,
    src: *const f32,
    src_len: usize,
    out: *mut f32,
    out_len: usize,
    live: *const f32,
    live_len: usize,
    start: u64,
    frames: usize,
    final_block: u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let final_block = flag(final_block)?;
        separate(src, src_len, out, out_len)?;
        separate(live, live_len, out, out_len)?;
        get_mut(p)?.process(
            input(src, src_len)?,
            output(out, out_len)?,
            input(live, live_len)?,
            start,
            frames,
            final_block,
        )
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_point(
    p: *mut session::Session,
    track: usize,
    src: *const f32,
    src_len: usize,
    out: *mut f32,
    out_len: usize,
    live: *const f32,
    live_len: usize,
    start: u64,
    frames: usize,
    user_gain: u32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let user_gain = flag(user_gain)?;
        separate(src, src_len, out, out_len)?;
        separate(live, live_len, out, out_len)?;
        get_mut(p)?.point(
            track,
            input(src, src_len)?,
            output(out, out_len)?,
            input(live, live_len)?,
            start,
            frames,
            user_gain,
        )
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_snapshot_bytes(
    p: *const session::Session,
    out: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let bytes = get(p)?.snapshot_bytes()?;
        *get_mut(out)? = bytes;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_snapshot_create(
    p: *const session::Session,
    out: *mut *mut session::Snapshot,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Ok(get(p)?.snapshot()))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_snapshot_capture(
    p: *const session::Session,
    out: *mut session::Snapshot,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get(p)?.capture(get_mut(out)?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_snapshot_restore(
    p: *mut session::Session,
    state: *const session::Snapshot,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.restore(get(state)?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_create(
    events: *const Event,
    len: usize,
    layout: u32,
    rate: u32,
    out: *mut *mut processor::Processor,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            Ok(processor::Processor::new(Arc::new(processor::Track::new(
                input(events, len)?.to_vec(),
                Layout::from_code(layout)?,
                rate,
            )?)))
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_reset(
    p: *mut processor::Processor,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_scale(
    p: *mut processor::Processor,
    scale: f32,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.set_scale(scale)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_required(
    p: *const processor::Processor,
    input: usize,
    finish: u32,
    out: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let samples = get(p)?.required(input, flag(finish)?)?;
        *get_mut(out)? = samples;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_process(
    p: *mut processor::Processor,
    src: *const f32,
    n: usize,
    out: *mut f32,
    len: usize,
    finish: u32,
    produced: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        range(produced, 1)?;
        separate(src, n, out, len)?;
        separate(produced, 1, out, len)?;
        let frames = get_mut(p)?.process(input(src, n)?, output(out, len)?, flag(finish)?)?;
        *produced = frames;
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_snapshot_create(
    p: *const processor::Processor,
    out: *mut *mut processor::Snapshot,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Ok(get(p)?.snapshot()))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_snapshot_restore(
    p: *mut processor::Processor,
    state: *const processor::Snapshot,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.restore(get(state)?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_filter_create(
    out: *mut *mut processor::Decorrelator,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || Ok(processor::Decorrelator::default()))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_filter_reset(
    p: *mut processor::Decorrelator,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        get_mut(p)?.reset();
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_filter_process(
    p: *mut processor::Decorrelator,
    src: *const f32,
    n: usize,
    out: *mut processor::FilteredFrame,
    len: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        if n != 32 || len != 32 {
            return Err(invalid());
        }
        separate(src, n, out, len)?;
        get_mut(p)?.process(
            input(src, n)?.try_into().unwrap(),
            output(out, len)?.try_into().unwrap(),
        );
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_tb_object_status(
    p: *const processor::Processor,
    out: *mut processor::TbStatus,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let status = get(p)?.status();
        *get_mut(out)? = status;
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_alloc::COUNT;
    #[test]
    fn ffi_lengths_aliasing_atomicity_lifetime_and_first_call_allocations() {
        unsafe {
            let mut msg = [0u8; 256];
            let m = msg.as_mut_ptr();
            let cap = msg.len();
            let events = [Event {
                start: 0,
                position: Position {
                    x: 0.25,
                    y: 0.4,
                    z: 0.5,
                },
                size: 0.25,
            }];
            let row = session::TbRowInput {
                input: 0,
                event_offset: 0,
                event_count: 1,
                bed_offset: 0,
                size_index: 0,
                kind: 2,
                gain: 1.,
            };
            let mut plan = ptr::null_mut();
            assert_eq!(
                mradm_dsp_tb_plan_create(
                    1,
                    1,
                    48000,
                    10000,
                    &row,
                    1,
                    events.as_ptr(),
                    1,
                    ptr::null(),
                    0,
                    &mut plan,
                    m,
                    cap
                ),
                0
            );
            let mut a = ptr::null_mut();
            let mut b = ptr::null_mut();
            assert_eq!(mradm_dsp_tb_create(plan, 1024, 960, 1, &mut a, m, cap), 0);
            assert_eq!(mradm_dsp_tb_create(plan, 1024, 960, 1, &mut b, m, cap), 0);
            mradm_dsp_tb_plan_destroy(plan);
            let mut snapshot = ptr::null_mut();
            assert_eq!(mradm_dsp_tb_snapshot_create(a, &mut snapshot, m, cap), 0);
            let input = [0.25; 1024];
            let mut out = [0.125; 16384];
            let mut expected = out;
            COUNT.with(|c| c.set(Some(0)));
            assert_eq!(mradm_dsp_tb_prepare_points(a, 0, 1024, m, cap), 0);
            assert_eq!(mradm_dsp_tb_prepare_points(b, 0, 1024, m, cap), 0);
            assert_ne!(
                mradm_dsp_tb_process(
                    a,
                    input.as_ptr(),
                    1023,
                    out.as_mut_ptr(),
                    16384,
                    ptr::null(),
                    0,
                    0,
                    1024,
                    0,
                    m,
                    cap
                ),
                0
            );
            assert_ne!(
                mradm_dsp_tb_process(
                    a,
                    input.as_ptr(),
                    1024,
                    out.as_mut_ptr(),
                    16383,
                    ptr::null(),
                    0,
                    0,
                    1024,
                    0,
                    m,
                    cap
                ),
                0
            );
            assert_ne!(
                mradm_dsp_tb_process(
                    a,
                    ptr::null(),
                    usize::MAX,
                    out.as_mut_ptr(),
                    16384,
                    ptr::null(),
                    0,
                    0,
                    1024,
                    0,
                    m,
                    cap
                ),
                0
            );
            assert_ne!(
                mradm_dsp_tb_process(
                    a,
                    out.as_ptr(),
                    1024,
                    out.as_mut_ptr(),
                    16384,
                    ptr::null(),
                    0,
                    0,
                    1024,
                    0,
                    m,
                    cap
                ),
                0
            );
            assert_ne!(
                mradm_dsp_tb_process(
                    a,
                    input.as_ptr(),
                    1024,
                    out.as_mut_ptr(),
                    16384,
                    out.as_ptr(),
                    1024,
                    0,
                    1024,
                    0,
                    m,
                    cap
                ),
                0
            );
            assert_eq!(out, expected);
            assert_eq!(
                mradm_dsp_tb_process(
                    a,
                    input.as_ptr(),
                    1024,
                    out.as_mut_ptr(),
                    16384,
                    ptr::null(),
                    0,
                    0,
                    1024,
                    0,
                    m,
                    cap
                ),
                0
            );
            assert_eq!(
                mradm_dsp_tb_process(
                    b,
                    input.as_ptr(),
                    1024,
                    expected.as_mut_ptr(),
                    16384,
                    ptr::null(),
                    0,
                    0,
                    1024,
                    0,
                    m,
                    cap
                ),
                0
            );
            assert_eq!(out, expected);
            assert_eq!(mradm_dsp_tb_snapshot_capture(a, snapshot, m, cap), 0);
            assert_eq!(mradm_dsp_tb_snapshot_restore(b, snapshot, m, cap), 0);
            assert_ne!(mradm_dsp_tb_scales(a, [f32::NAN].as_ptr(), 1, 0, m, cap), 0);
            assert_eq!(mradm_dsp_tb_scales(a, [0.5].as_ptr(), 1, 0, m, cap), 0);
            assert_eq!(mradm_dsp_tb_reset(a, m, cap), 0);
            assert_eq!(
                mradm_dsp_tb_process(
                    a,
                    ptr::null(),
                    0,
                    ptr::null_mut(),
                    0,
                    ptr::null(),
                    0,
                    0,
                    0,
                    0,
                    m,
                    cap
                ),
                0
            );
            let allocations = COUNT.with(|c| c.replace(None).unwrap());
            assert_eq!(allocations, 0);
            mradm_dsp_tb_snapshot_destroy(snapshot);
            mradm_dsp_tb_destroy(a);
            mradm_dsp_tb_destroy(b);
            let mut p = ptr::null_mut();
            assert_eq!(
                mradm_dsp_tb_object_create(events.as_ptr(), 1, 1, 48000, &mut p, m, cap),
                0
            );
            let mut produced = 123;
            assert_ne!(
                mradm_dsp_tb_object_process(
                    p,
                    [f32::NAN].as_ptr(),
                    1,
                    out.as_mut_ptr(),
                    16384,
                    0,
                    &mut produced,
                    m,
                    cap
                ),
                0
            );
            assert_eq!(produced, 123);
            assert_eq!(
                mradm_dsp_tb_object_process(
                    p,
                    input.as_ptr(),
                    31,
                    out.as_mut_ptr(),
                    16384,
                    1,
                    &mut produced,
                    m,
                    cap
                ),
                0
            );
            assert_eq!(produced, 31);
            assert_ne!(
                mradm_dsp_tb_object_process(
                    p,
                    ptr::null(),
                    0,
                    out.as_mut_ptr(),
                    16384,
                    1,
                    &mut produced,
                    m,
                    cap
                ),
                0
            );
            mradm_dsp_tb_object_destroy(p);
        }
    }
}
