use crate::{boundary, ptr, slice};
use mradm_dsp::{
    Error,
    meter::{Meter, MeterChannel, MeterMode},
};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_meter_create(
    channels: u32,
    rate: u32,
    mode: u32,
    map: *const u32,
    map_len: usize,
    output: *mut *mut Meter,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if output.is_null() {
            return Err(Error::InvalidArgument("Null meter output handle"));
        }
        unsafe {
            *output = ptr::null_mut();
        }
        if map_len > 64 || (map_len != 0 && (map.is_null() || map_len != channels as usize)) {
            return Err(Error::InvalidArgument("Invalid meter channel map"));
        }
        let map = if map_len == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(map, map_len) }
        };
        let positions: Result<Vec<_>, _> =
            map.iter().copied().map(MeterChannel::try_from).collect();
        let meter = Meter::new(channels, rate, MeterMode::try_from(mode)?, &positions?)?;
        unsafe {
            *output = Box::into_raw(Box::new(meter));
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_meter_destroy(handle: *mut Meter) {
    if !handle.is_null() {
        unsafe {
            drop(Box::from_raw(handle));
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_meter_add(
    handle: *mut Meter,
    input: *const f32,
    length: usize,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null()
            || (input.is_null() && length != 0)
            || length > isize::MAX as usize / size_of::<f32>()
        {
            return Err(Error::InvalidArgument(
                "Invalid meter input buffer or handle",
            ));
        }
        let meter = unsafe { &mut *handle };
        if !length.is_multiple_of(meter.channels()) {
            return Err(Error::InvalidArgument(
                "Meter input must contain complete interleaved frames",
            ));
        }
        let input = if length == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(input, length) }
        };
        meter.add_frames(input)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_meter_reset(
    handle: *mut Meter,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() {
            return Err(Error::InvalidArgument("Null meter handle"));
        }
        unsafe { &mut *handle }.reset();
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_dsp_meter_query(
    handle: *const Meter,
    kind: u32,
    channel: u32,
    output: *mut f64,
    message: *mut u8,
    capacity: usize,
) -> i32 {
    boundary(message, capacity, || {
        if handle.is_null() || output.is_null() {
            return Err(Error::InvalidArgument("Null meter query argument"));
        }
        let meter = unsafe { &*handle };
        let value = match kind {
            0 => meter.integrated(),
            1 => meter.momentary(),
            2 => meter.shortterm(),
            3 => meter.true_peak(channel),
            4 => meter.max_true_peak(),
            _ => Err(Error::InvalidArgument("Invalid meter query")),
        }?;
        unsafe {
            *output = value;
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn lengths_modes_and_failed_outputs_are_checked() {
        unsafe {
            let mut handle = ptr::dangling_mut();
            assert_eq!(
                mradm_dsp_meter_create(
                    0,
                    48_000,
                    2,
                    ptr::null(),
                    0,
                    &mut handle,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert!(handle.is_null());
            assert_eq!(
                mradm_dsp_meter_create(
                    2,
                    48_000,
                    99,
                    ptr::null(),
                    0,
                    &mut handle,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_meter_create(
                    2,
                    48_000,
                    2,
                    [1, 99].as_ptr(),
                    2,
                    &mut handle,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_meter_create(
                    2,
                    48_000,
                    2,
                    ptr::null(),
                    2,
                    &mut handle,
                    ptr::null_mut(),
                    0
                ),
                1
            );
            assert_eq!(
                mradm_dsp_meter_create(
                    2,
                    48_000,
                    2,
                    ptr::null(),
                    0,
                    &mut handle,
                    ptr::null_mut(),
                    0
                ),
                0
            );
            assert_eq!(
                mradm_dsp_meter_add(handle, ptr::null(), 0, ptr::null_mut(), 0),
                0
            );
            assert_eq!(
                mradm_dsp_meter_add(handle, ptr::null(), 2, ptr::null_mut(), 0),
                1
            );
            assert_eq!(
                mradm_dsp_meter_add(handle, [0.0].as_ptr(), 1, ptr::null_mut(), 0),
                1
            );
            assert_eq!(
                mradm_dsp_meter_add(handle, [0.0].as_ptr(), usize::MAX, ptr::null_mut(), 0),
                1
            );
            let mut result = 42.0;
            assert_eq!(
                mradm_dsp_meter_query(handle, 3, 2, &mut result, ptr::null_mut(), 0),
                1
            );
            assert_eq!(result, 42.0);
            assert_eq!(
                mradm_dsp_meter_query(handle, 99, 0, &mut result, ptr::null_mut(), 0),
                1
            );
            assert_eq!(
                mradm_dsp_meter_query(handle, 0, 0, &mut result, ptr::null_mut(), 0),
                0
            );
            assert_eq!(result, f64::NEG_INFINITY);
            assert_eq!(mradm_dsp_meter_reset(handle, ptr::null_mut(), 0), 0);
            mradm_dsp_meter_destroy(handle);
            mradm_dsp_meter_destroy(ptr::null_mut());
        }
    }
}
