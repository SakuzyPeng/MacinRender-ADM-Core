//! Immutable HRTF handles. Queries can share a handle across workers, provided
//! their outputs are disjoint. Destruction must wait for all active calls.
use crate::{boundary, ptr, slice};
use mradm_dsp::{
    Error, Result,
    hrtf::{GRID_POINTS, Grid},
    hrtf_filters::{Filters, Lookup, Trace, grid_index},
};
use std::sync::Arc;

unsafe fn shared<'a, T>(value: *const T) -> Result<&'a T> {
    unsafe { value.as_ref() }.ok_or(Error::InvalidArgument("Null HRTF handle"))
}
unsafe fn input<'a, T>(data: *const T, length: usize) -> Result<&'a [T]> {
    if data.is_null() || length > isize::MAX as usize / size_of::<T>() {
        return Err(Error::InvalidArgument("Invalid HRTF input buffer"));
    }
    Ok(unsafe { slice::from_raw_parts(data, length) })
}
unsafe fn output<'a, T>(data: *mut T, length: usize) -> Result<&'a mut [T]> {
    if data.is_null() || length > isize::MAX as usize / size_of::<T>() {
        return Err(Error::InvalidArgument("Invalid HRTF output buffer"));
    }
    Ok(unsafe { slice::from_raw_parts_mut(data, length) })
}
unsafe fn create<T>(out: *mut *mut T, make: impl FnOnce() -> Result<T>) -> Result<()> {
    if out.is_null() {
        return Err(Error::InvalidArgument("Null HRTF output handle"));
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
unsafe fn destroy<T>(value: *mut T) {
    if !value.is_null() {
        unsafe {
            drop(Box::from_raw(value));
        }
    }
}

#[repr(C)]
pub struct GridInfo {
    directions: usize,
    entries: usize,
    storage_bytes: usize,
}
#[repr(C)]
pub struct FilterInfo {
    output_length: usize,
    spectrum_length: usize,
    storage_bytes: usize,
}
#[repr(C)]
pub struct QueryTrace {
    magnitudes: *mut f32,
    magnitudes_length: usize,
    complex_sum: *mut f32,
    complex_sum_length: usize,
    scales: *mut f32,
    scales_length: usize,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_grid_create(
    directions: *const f32,
    length: usize,
    out: *mut *mut Arc<Grid>,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            if !(8..=200_000).contains(&length) || !length.is_multiple_of(2) {
                return Err(Error::InvalidArgument("Invalid HRTF direction count"));
            }
            Ok(Arc::new(Grid::new(input(directions, length)?)?))
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_grid_destroy(value: *mut Arc<Grid>) {
    unsafe {
        destroy(value);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_grid_info(
    value: *const Arc<Grid>,
    out: *mut GridInfo,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let grid = shared(value)?;
        if out.is_null() {
            return Err(Error::InvalidArgument("Null HRTF grid info"));
        }
        *out = GridInfo {
            directions: grid.direction_count(),
            entries: GRID_POINTS * 3,
            storage_bytes: grid.storage_bytes() + size_of::<Arc<Grid>>(),
        };
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_grid_copy(
    value: *const Arc<Grid>,
    weights: *mut f32,
    indices: *mut i32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let grid = shared(value)?;
        if length != GRID_POINTS * 3 || weights.is_null() || indices.is_null() {
            return Err(Error::InvalidArgument("Invalid HRTF grid snapshot buffers"));
        }
        output(weights, length)?.copy_from_slice(grid.weights());
        output(indices, length)?.copy_from_slice(grid.indices());
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_grid_index(
    azimuth: f32,
    elevation: f32,
    out: *mut usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if out.is_null() {
            return Err(Error::InvalidArgument("Null HRTF grid index"));
        }
        let index = grid_index(azimuth, elevation)?;
        unsafe {
            *out = index;
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_filters_create(
    grid: *const Arc<Grid>,
    impulses: *const f32,
    input_length: usize,
    taps: usize,
    fft_length: usize,
    cache_magnitudes: u32,
    out: *mut *mut Filters,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        create(out, || {
            let grid = shared(grid)?;
            if cache_magnitudes > 1
                || taps == 0
                || taps > fft_length
                || grid
                    .direction_count()
                    .checked_mul(2)
                    .and_then(|n| n.checked_mul(taps))
                    != Some(input_length)
            {
                return Err(Error::InvalidArgument("Invalid HRTF preparation arguments"));
            }
            Filters::new(
                Arc::clone(grid),
                input(impulses, input_length)?,
                taps,
                fft_length,
                cache_magnitudes != 0,
            )
        })
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_filters_destroy(value: *mut Filters) {
    unsafe {
        destroy(value);
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_filters_info(
    value: *const Filters,
    out: *mut FilterInfo,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let filters = shared(value)?;
        if out.is_null() {
            return Err(Error::InvalidArgument("Null HRTF filter info"));
        }
        *out = FilterInfo {
            output_length: filters.output_len(),
            spectrum_length: filters.spectrum_len(),
            storage_bytes: filters.storage_bytes(),
        };
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_spectra_copy(
    value: *const Filters,
    out: *mut f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let filters = shared(value)?;
        if length != filters.spectrum_len() {
            return Err(Error::InvalidArgument("Invalid HRTF spectrum length"));
        }
        filters.copy_spectra(output(out, length)?)
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hrtf_query(
    value: *const Filters,
    azimuth: f32,
    elevation: f32,
    mode: u32,
    out: *mut f32,
    length: usize,
    trace: *const QueryTrace,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || unsafe {
        let filters = shared(value)?;
        if mode > 1
            || length != filters.output_len()
            || !azimuth.is_finite()
            || !elevation.is_finite()
        {
            return Err(Error::InvalidArgument("Invalid HRTF query arguments"));
        }
        let lookup = if mode == 0 {
            Lookup::Quantized
        } else {
            Lookup::Continuous
        };
        let trace = if let Some(t) = trace.as_ref() {
            if mode != 0
                || t.magnitudes_length != length / 2 * 3
                || t.complex_sum_length != length
                || t.scales_length != length
            {
                return Err(Error::InvalidArgument("Invalid HRTF trace lengths"));
            }
            Some(Trace {
                magnitudes: output(t.magnitudes, t.magnitudes_length)?,
                complex_sum: output(t.complex_sum, t.complex_sum_length)?,
                scales: output(t.scales, t.scales_length)?,
            })
        } else {
            None
        };
        filters.query(azimuth, elevation, lookup, output(out, length)?, trace)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn invalid_boundary_arguments_preserve_outputs_and_creation_clears_handles() {
        unsafe {
            let mut grid = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_hrtf_grid_create(ptr::null(), usize::MAX, &mut grid, ptr::null_mut(), 0),
                1
            );
            assert!(grid.is_null());
            let directions = [0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.];
            assert_eq!(
                mradm_dsp_hrtf_grid_create(directions.as_ptr(), 12, &mut grid, ptr::null_mut(), 0),
                0
            );
            let mut filters = ptr::dangling_mut();
            let ir = [0.25; 12];
            for (length, taps, fft, cache) in [
                (usize::MAX, 1, 2, 0),
                (12, usize::MAX, usize::MAX, 0),
                (12, 1, 2, 2),
                (12, 1, 3, 0),
            ] {
                assert_eq!(
                    mradm_dsp_hrtf_filters_create(
                        grid,
                        ir.as_ptr(),
                        length,
                        taps,
                        fft,
                        cache,
                        &mut filters,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
                assert!(filters.is_null());
            }
            assert_eq!(
                mradm_dsp_hrtf_filters_create(
                    grid,
                    ir.as_ptr(),
                    12,
                    1,
                    2,
                    1,
                    &mut filters,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            let mut out = [42.; 8];
            let mut trace_values = [43.; 12];
            let mut sums = [44.; 8];
            let mut scales = [45.; 8];
            let mut trace = QueryTrace {
                magnitudes: trace_values.as_mut_ptr(),
                magnitudes_length: 12,
                complex_sum: sums.as_mut_ptr(),
                complex_sum_length: 8,
                scales: scales.as_mut_ptr(),
                scales_length: 8,
            };
            for (length, mode, az, el) in [
                (usize::MAX, 0, 0., 0.),
                (7, 0, 0., 0.),
                (8, 2, 0., 0.),
                (8, 0, f32::NAN, 0.),
                (8, 0, 0., f32::INFINITY),
            ] {
                assert_eq!(
                    mradm_dsp_hrtf_query(
                        filters,
                        az,
                        el,
                        mode,
                        out.as_mut_ptr(),
                        length,
                        &trace,
                        ptr::null_mut(),
                        0
                    ),
                    1
                );
            }
            assert_eq!(
                mradm_dsp_hrtf_query(
                    filters,
                    0.,
                    0.,
                    1,
                    out.as_mut_ptr(),
                    8,
                    &trace,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            trace.scales_length = usize::MAX;
            assert_eq!(
                mradm_dsp_hrtf_query(
                    filters,
                    0.,
                    0.,
                    0,
                    out.as_mut_ptr(),
                    8,
                    &trace,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            trace.scales_length = 8;
            trace.scales = ptr::null_mut();
            assert_eq!(
                mradm_dsp_hrtf_query(
                    filters,
                    0.,
                    0.,
                    0,
                    out.as_mut_ptr(),
                    8,
                    &trace,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_hrtf_query(
                    ptr::null(),
                    0.,
                    0.,
                    0,
                    out.as_mut_ptr(),
                    8,
                    ptr::null(),
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_hrtf_query(
                    filters,
                    0.,
                    0.,
                    0,
                    ptr::null_mut(),
                    8,
                    ptr::null(),
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_hrtf_spectra_copy(
                    filters,
                    out.as_mut_ptr(),
                    usize::MAX,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(out, [42.; 8]);
            assert_eq!(trace_values, [43.; 12]);
            assert_eq!(sums, [44.; 8]);
            assert_eq!(scales, [45.; 8]);
            let mut weights = [42.; 3];
            let mut indices = [42; 3];
            assert_eq!(
                mradm_dsp_hrtf_grid_copy(
                    grid,
                    weights.as_mut_ptr(),
                    indices.as_mut_ptr(),
                    usize::MAX,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(weights, [42.; 3]);
            assert_eq!(indices, [42; 3]);
            let mut index = 42;
            assert_eq!(
                mradm_dsp_hrtf_grid_index(f32::NAN, 0., &mut index, ptr::null_mut(), 0),
                1
            );
            assert_eq!(index, 42);
            // The bank keeps its own Arc. C++ cache eviction/destruction cannot
            // invalidate its immutable table.
            mradm_dsp_hrtf_grid_destroy(grid);
            assert_eq!(
                mradm_dsp_hrtf_query(
                    filters,
                    0.,
                    0.,
                    1,
                    out.as_mut_ptr(),
                    8,
                    ptr::null(),
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(out, [0.25, 0., 0.25, 0., 0.25, 0., 0.25, 0.]);
            mradm_dsp_hrtf_filters_destroy(filters);
            mradm_dsp_hrtf_filters_destroy(ptr::null_mut());
            mradm_dsp_hrtf_grid_destroy(ptr::null_mut());
        }
    }
}
