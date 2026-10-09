//! Raw range validation shared by the Scene streaming private boundaries.
use mradm_dsp::{Error, Result};
use std::slice;
pub fn invalid() -> Error {
    Error::InvalidArgument("Invalid Scene DSP buffer or parameter")
}
pub fn range<T>(p: *const T, n: usize) -> Result<(usize, usize)> {
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
pub fn separate<A, B>(a: *const A, an: usize, b: *const B, bn: usize) -> Result<()> {
    let (a, z) = range(a, an)?;
    let (b, y) = range(b, bn)?;
    if a != z && b != y && a < y && b < z {
        return Err(invalid());
    }
    Ok(())
}
pub unsafe fn input<'a, T>(p: *const T, n: usize) -> Result<&'a [T]> {
    range(p, n)?;
    Ok(if n == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(p, n) }
    })
}
pub unsafe fn output<'a, T>(p: *mut T, n: usize) -> Result<&'a mut [T]> {
    range(p, n)?;
    Ok(if n == 0 {
        &mut []
    } else {
        unsafe { slice::from_raw_parts_mut(p, n) }
    })
}
pub fn flag(x: u32) -> Result<bool> {
    match x {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(invalid()),
    }
}
