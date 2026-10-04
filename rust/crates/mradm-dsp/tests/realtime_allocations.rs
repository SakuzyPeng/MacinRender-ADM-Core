//! The allocator instrumentation belongs to the test executable, not the safe DSP crate.
use mradm_dsp::meter::{Meter, MeterMode};
use mradm_dsp::{
    convolution::{LiveConvolver, OlaConvolver},
    diffuse::DiffuseDelay,
};
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
fn prepared_resampler_and_finish_reset_do_not_allocate() {
    use mradm_dsp::resampler::Resampler;
    for (from, to) in [(48000, 44100), (8000, 192000), (192000, 8000)] {
        let mut r = Resampler::new(2, from, to).unwrap();
        let input = [0.01; 1024];
        let mut output = [0.0; 34];
        COUNT.with(|c| c.set(Some(0)));
        for _ in 0..2 {
            let mut offset = 0;
            while offset < input.len() {
                let p = r.process(&input[offset..], &mut output).unwrap();
                offset += p.input_frames * 2;
            }
            while r.finish(&mut output).unwrap() != 0 {}
            r.reset();
        }
        let count = COUNT.with(|c| c.replace(None).unwrap());
        assert_eq!(count, 0, "allocation in prepared resampling / EOS / reset");
    }
}

#[test]
fn prepared_binaural_motion_fades_diffuse_and_resets_do_not_allocate() {
    let mut live = LiveConvolver::new(64, 64, 48000).unwrap();
    let mut state = live.make_state();
    let mut second_state = live.make_state();
    let mut ola = OlaConvolver::new(64, 31, 32).unwrap();
    let mut diffuse = DiffuseDelay::default();
    let mut hrtf = [0.0; 132];
    let mut target = hrtf;
    for band in 0..33 {
        hrtf[band * 4] = 1.0;
        hrtf[band * 4 + 2] = 0.5;
        target[band * 4] = -0.4;
        target[band * 4 + 2] = 0.7;
    }
    let mut samples = [0.01; 32];
    let (mut left, mut right) = ([0.0; 32], [0.0; 32]);
    COUNT.with(|c| c.set(Some(0)));
    // Count from the first prepared call, including filter expansion and every
    // retarget. A warm-up must not hide lazy allocations.
    for i in 0..40 {
        let filter = if i % 2 == 0 { &hrtf } else { &target };
        live.process(
            &mut state,
            filter,
            &samples,
            &mut left,
            &mut right,
            i % 3 == 0,
        )
        .unwrap();
        live.process(
            &mut second_state,
            filter,
            &samples,
            &mut left,
            &mut right,
            false,
        )
        .unwrap();
        ola.process(
            &samples,
            &hrtf,
            0.8,
            Some((&target, 0.4)),
            &mut left,
            &mut right,
        )
        .unwrap();
        ola.advance_silence(&mut left, &mut right).unwrap();
        diffuse.process(&samples, &mut left).unwrap();
        diffuse.mix(&mut samples, [0.5, 0.9], [0.0, 1.0]).unwrap();
        if i % 5 == 0 {
            state.reset();
            ola.reset();
            diffuse.reset();
        }
    }
    let count = COUNT.with(|c| c.replace(None).unwrap());
    assert_eq!(count, 0, "allocation in prepared binaural DSP");
}

#[test]
fn prepared_meter_short_windows_and_seek_do_not_allocate() {
    // Full-history I can grow after ~500 s. This contract covers prepared short
    // windows and resets, not an unbounded no-allocation audio callback promise.
    let mut monitor = Meter::new(2, 48_000, MeterMode::Monitor, &[]).unwrap();
    let mut offline = Meter::new(12, 48_000, MeterMode::IntegratedTruePeak, &[]).unwrap();
    let stereo = [0.01; 1024];
    let surround = [0.01; 6144];
    COUNT.with(|c| c.set(Some(0)));
    for _ in 0..100 {
        monitor.add_frames(&stereo).unwrap();
        monitor.integrated().unwrap();
        monitor.momentary().unwrap();
        monitor.shortterm().unwrap();
        offline.add_frames(&surround).unwrap();
        offline.max_true_peak().unwrap();
    }
    monitor.reset();
    offline.reset();
    let count = COUNT.with(|c| c.replace(None).unwrap());
    assert_eq!(count, 0, "allocation in prepared meter processing or seek");
}

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
