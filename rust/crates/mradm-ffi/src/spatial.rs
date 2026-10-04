use super::boundary;
use mradm_dsp::{Error, data, dataset::Dataset, vbap::Panner};
use std::{ptr, slice};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_panner_create(
    dirs: *const f32,
    length: usize,
    is_3d: i32,
    output: *mut *mut Panner,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::InvalidArgument("Null panner output handle"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        if dirs.is_null() || !(4..=200_000).contains(&length) {
            return Err(Error::InvalidArgument("Invalid speaker array"));
        }
        let panner = Panner::new(unsafe { slice::from_raw_parts(dirs, length) }, is_3d != 0)?;
        unsafe {
            *output = Box::into_raw(Box::new(panner));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_panner_destroy(handle: *mut Panner) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_panner_gains(
    handle: *const Panner,
    az: f32,
    el: f32,
    spread: f32,
    output: *mut f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || output.is_null() {
            return Err(Error::InvalidArgument("Null panner argument"));
        }
        let panner = unsafe { &*handle };
        if length != panner.real_speakers {
            return Err(Error::InvalidArgument("Invalid gain buffer length"));
        }
        panner.gains(az, el, spread, unsafe {
            slice::from_raw_parts_mut(output, length)
        })
    })
}
#[repr(C)]
pub struct DatasetInfo {
    sample_rate: u32,
    num_dirs: usize,
    ir_len: usize,
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_dataset_create(
    bytes: *const u8,
    length: usize,
    builtin: i32,
    output: *mut *mut Dataset,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::InvalidArgument("Null dataset output handle"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        let dataset = if builtin != 0 {
            Dataset::kemar()
        } else {
            if bytes.is_null() || length == 0 || length > isize::MAX as usize {
                return Err(Error::InvalidArgument("Empty SOFA input"));
            }
            Dataset::sofa(unsafe { slice::from_raw_parts(bytes, length) })?
        };
        unsafe {
            *output = Box::into_raw(Box::new(dataset));
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_dataset_destroy(handle: *mut Dataset) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_dataset_info(
    handle: *const Dataset,
    info: *mut DatasetInfo,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || info.is_null() {
            return Err(Error::InvalidArgument("Null dataset info argument"));
        }
        let data = unsafe { &*handle };
        unsafe {
            *info = DatasetInfo {
                sample_rate: data.sample_rate,
                num_dirs: data.num_dirs,
                ir_len: data.ir_len,
            };
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_dataset_copy(
    handle: *const Dataset,
    dirs: *mut f32,
    dir_length: usize,
    impulses: *mut f32,
    ir_length: usize,
    name: *mut u8,
    name_capacity: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || dirs.is_null() || impulses.is_null() {
            return Err(Error::InvalidArgument("Null dataset copy argument"));
        }
        let data = unsafe { &*handle };
        if dir_length != data.directions.len() || ir_length != data.impulses.len() {
            return Err(Error::InvalidArgument("Dataset buffer length mismatch"));
        }
        unsafe {
            ptr::copy_nonoverlapping(data.directions.as_ptr(), dirs, dir_length);
            ptr::copy_nonoverlapping(data.impulses.as_ptr(), impulses, ir_length);
        }
        if !name.is_null() && name_capacity > 0 {
            let mut n = data.name.len().min(name_capacity - 1);
            while !data.name.is_char_boundary(n) {
                n -= 1;
            }
            unsafe {
                ptr::copy_nonoverlapping(data.name.as_ptr(), name, n);
                *name.add(n) = 0;
            }
        }
        Ok(())
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_hoa_matrix(
    output: *mut f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() || length != 176 {
            return Err(Error::InvalidArgument("Invalid HOA matrix buffer"));
        }
        let output = unsafe { slice::from_raw_parts_mut(output, length) };
        for (value, bytes) in output
            .iter_mut()
            .zip(data::HOA_714.as_chunks::<4>().0.iter())
        {
            *value = f32::from_le_bytes(*bytes);
        }
        Ok(())
    })
}
