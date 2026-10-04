//! The allocator instrumentation belongs to the test executable, not the safe DSP crate.
use mradm_dsp::{fft::RealFft, spreader::Spreader};
use std::{
    alloc::{GlobalAlloc, Layout, System},
    cell::Cell,
};
thread_local! {static COUNT:Cell<Option<usize>>=const {Cell::new(None)};}
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let _ = COUNT.try_with(|count| {
            if let Some(n) = count.get() {
                count.set(Some(n + 1));
            }
        });
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let _ = COUNT.try_with(|count| {
            if let Some(n) = count.get() {
                count.set(Some(n + 1));
            }
        });
        unsafe { System.realloc(pointer, layout, size) }
    }
}
#[global_allocator]
static ALLOCATOR: Counting = Counting;

#[test]
fn prepared_fft_and_moving_spreader_do_not_allocate() {
    let dirs = [0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.];
    let mut ir = vec![0.; 6 * 2 * 16];
    for dir in 0..6 {
        ir[dir * 32 + 1] = 0.5;
        ir[dir * 32 + 16 + 3] = 0.7;
    }
    let mut spreader = Spreader::new(&ir, &dirs, 16, 48000, &[7, 9]).unwrap();
    let input = [0.01; 1024];
    let mut output = [0.; 1024];
    let mut fft = RealFft::new(512).unwrap();
    let mut spectrum = [0.; 514];
    let mut inverse = [0.; 512];
    COUNT.with(|c| c.set(Some(0)));
    for frame in 0..20 {
        spreader.set_source(0, frame as f32 * 3., 20., 90.).unwrap();
        spreader.process(&input, &mut output).unwrap();
        fft.forward_interleaved(&input[..512], &mut spectrum)
            .unwrap();
        fft.inverse_interleaved(&spectrum, &mut inverse).unwrap();
    }
    let count = COUNT.with(|c| c.replace(None).unwrap());
    assert_eq!(count, 0, "allocation in prepared DSP processing");
}
