//! Reference-only FFI to the project's C libebur128 1.2.6. No C binding crate.
use ebur128::{Channel, Mode};
use std::{ffi::c_ulong, ptr::NonNull};

#[repr(C)]
struct State {
    _opaque: [u8; 0],
}

unsafe extern "C" {
    fn ebur128_get_version(major: *mut i32, minor: *mut i32, patch: *mut i32);
    fn ebur128_init(channels: u32, rate: c_ulong, mode: i32) -> *mut State;
    fn ebur128_destroy(state: *mut *mut State);
    fn ebur128_set_channel(state: *mut State, index: u32, channel: i32) -> i32;
    fn ebur128_set_max_history(state: *mut State, milliseconds: c_ulong) -> i32;
    fn ebur128_add_frames_float(state: *mut State, samples: *const f32, frames: usize) -> i32;
    fn ebur128_loudness_global(state: *mut State, out: *mut f64) -> i32;
    fn ebur128_loudness_momentary(state: *mut State, out: *mut f64) -> i32;
    fn ebur128_loudness_shortterm(state: *mut State, out: *mut f64) -> i32;
    fn ebur128_true_peak(state: *mut State, channel: u32, out: *mut f64) -> i32;
}

pub struct Meter {
    state: NonNull<State>,
    channels: usize,
}

impl Meter {
    pub fn version() -> [i32; 3] {
        let mut version = [0; 3];
        unsafe { ebur128_get_version(&mut version[0], &mut version[1], &mut version[2]) };
        version
    }

    pub fn new(channels: u32, rate: u32, mode: Mode, map: Option<&[Channel]>) -> Self {
        let state = NonNull::new(unsafe {
            ebur128_init(channels, c_ulong::from(rate), i32::from(mode.bits()))
        })
        .expect("C reference initialization");
        let result = Self {
            state,
            channels: channels as usize,
        };
        if let Some(map) = map {
            assert_eq!(map.len(), channels as usize);
            for (index, channel) in map.iter().enumerate() {
                assert_eq!(
                    unsafe { ebur128_set_channel(state.as_ptr(), index as u32, *channel as i32) },
                    0
                );
            }
        }
        result
    }

    pub fn add(&mut self, samples: &[f32]) {
        assert!(samples.len().is_multiple_of(self.channels));
        assert_eq!(
            unsafe {
                ebur128_add_frames_float(
                    self.state.as_ptr(),
                    samples.as_ptr(),
                    samples.len() / self.channels,
                )
            },
            0
        );
    }

    pub fn set_max_history(&mut self, milliseconds: u32) {
        assert_eq!(
            unsafe { ebur128_set_max_history(self.state.as_ptr(), c_ulong::from(milliseconds)) },
            0
        );
    }

    pub fn loudness(&self, kind: usize) -> f64 {
        let function = match kind {
            0 => ebur128_loudness_global,
            1 => ebur128_loudness_momentary,
            2 => ebur128_loudness_shortterm,
            _ => unreachable!(),
        };
        let mut result = 0.0;
        assert_eq!(unsafe { function(self.state.as_ptr(), &mut result) }, 0);
        result
    }

    pub fn peak(&self, channel: u32) -> f64 {
        let mut result = 0.0;
        assert_eq!(
            unsafe { ebur128_true_peak(self.state.as_ptr(), channel, &mut result) },
            0
        );
        result
    }
}

impl Drop for Meter {
    fn drop(&mut self) {
        let mut state = self.state.as_ptr();
        unsafe { ebur128_destroy(&mut state) };
    }
}
