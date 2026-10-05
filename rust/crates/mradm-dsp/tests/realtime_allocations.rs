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
fn prepared_pcm_tables_cursors_and_dynamic_updates_do_not_allocate() {
    use mradm_dsp::pcm_mix::{Block, Coefficients, Matrix, Mixer, Plan, Row};
    use std::sync::Arc;
    let blocks = vec![
        Block {
            start: 0,
            end: 8,
            interpolation: 0,
            flags: 3,
        },
        Block {
            start: 8,
            end: u64::MAX,
            interpolation: 5,
            flags: 6,
        },
    ];
    let rows = vec![Row {
        input_channel: 0,
        block_offset: 0,
        block_count: 2,
        output_gain: 1.,
    }];
    let common = Arc::new(
        Plan::new(
            1,
            2,
            rows.clone(),
            blocks.clone(),
            Coefficients::Speaker(vec![1., 0., 0., 1.]),
        )
        .unwrap(),
    );
    let ear = Arc::new(
        Plan::new(
            1,
            2,
            rows,
            blocks.clone(),
            Coefficients::Ear(vec![1., 0., 0., 1., 0., 1., 1., 0.]),
        )
        .unwrap(),
    );
    let mut a = Mixer::new(common, 32, 3, true).unwrap();
    let mut b = Mixer::new(ear, 32, 3, true).unwrap();
    let mut dynamic = Mixer::dynamic(1, 2, &[0], 3, 32, 3).unwrap();
    let matrix = Matrix::new(1, 2, &[0.5, 1.]).unwrap();
    let input = [0.5; 32];
    let mut direct = [0.; 64];
    let mut diffuse = [0.; 64];
    COUNT.with(|c| c.set(Some(0)));
    for _ in 0..20 {
        a.speaker(&input, &mut direct, &[], 0, 32, None, None)
            .unwrap();
        b.ear(&input, &mut direct, &mut diffuse, 0, 32).unwrap();
        dynamic
            .update(0, blocks.iter().copied(), &[1., 0., 0., 1.], 1.)
            .unwrap();
        dynamic
            .speaker(&input, &mut direct, &[], 0, 32, Some(0), None)
            .unwrap();
        matrix.process(&input, &mut diffuse, 32).unwrap();
        assert!(
            dynamic
                .update(0, blocks.iter().copied(), &[f32::NAN; 4], 1.)
                .is_err()
        );
        a.reset();
        b.reset();
        dynamic.reset();
    }
    let count = COUNT.with(|c| c.replace(None).unwrap());
    assert_eq!(count, 0);
}

#[test]
fn prepared_output_gains_and_peak_protection_do_not_allocate() {
    use mradm_dsp::{gain::GainBank, peak_guard::StereoPeakGuard};
    let mut gains = GainBank::new(2, 48000, 20).unwrap();
    let mut guard = StereoPeakGuard::new(48000).unwrap();
    let mut block = [0.25; 74];
    COUNT.with(|c| c.set(Some(0)));
    for index in 0..80 {
        gains.set_targets(&[index as f32 * 0.01, 0.5]).unwrap();
        gains.fill(&mut block).unwrap();
        gains.apply(&mut block).unwrap();
        guard.push(&block).unwrap();
        guard.pop(&mut block, 0.8, false).unwrap();
        assert!(gains.set_targets(&[f32::NAN]).is_err());
        assert!(guard.pop(&mut block, f32::NAN, true).is_err());
        if index % 13 == 0 {
            gains.reset();
            guard.reset();
        }
    }
    while guard.pop(&mut block, 0.8, true).unwrap() != 0 {}
    gains.reset();
    guard.reset();
    let count = COUNT.with(|c| c.replace(None).unwrap());
    assert_eq!(
        count, 0,
        "allocation in prepared live gains / peak protection"
    );
}

#[test]
fn prepared_hptf_updates_processing_and_seek_do_not_allocate() {
    use mradm_dsp::hptf::{Band, BandKind, Cascade, PreampMode, Processor, Snapshot, design};
    let coefficients = design(
        &[Band {
            kind: BandKind::Peaking,
            enabled: true,
            frequency: 46.3,
            gain_db: 6.0,
            q: 8.0,
        }],
        -2.,
        48000,
        PreampMode::AutoTrim,
    )
    .unwrap();
    let mut cascade = Cascade::new(2).unwrap();
    let mut processor = Processor::new(2, 48000).unwrap();
    let mut input = [0.01; 274];
    let mut blending = false;
    COUNT.with(|c| c.set(Some(0)));
    cascade.set_coefficients(coefficients).unwrap();
    for revision in 1..100 {
        cascade.process(&mut input).unwrap();
        let target = Snapshot {
            coefficients,
            revision,
        };
        let update = processor
            .process(&mut input, (!blending).then_some(target))
            .unwrap();
        blending = update.blending;
        if revision % 13 == 0 {
            blending = processor.reset(Some(target)).unwrap().blending;
            cascade.reset();
        }
    }
    processor.reset(None).unwrap();
    let count = COUNT.with(|c| c.replace(None).unwrap());
    assert_eq!(count, 0, "allocation in prepared HpTF DSP");
}

#[test]
fn prepared_hrtf_queries_never_allocate_including_first_motion_and_errors() {
    use mradm_dsp::{
        hrtf::Grid,
        hrtf_filters::{Filters, Lookup},
    };
    use std::sync::Arc;
    let directions = [0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.];
    let grid = Arc::new(Grid::new(&directions).unwrap());
    for cache in [false, true] {
        let f = Filters::new(Arc::clone(&grid), &[0.25; 192], 16, 64, cache).unwrap();
        let mut out = vec![0.; f.output_len()];
        COUNT.with(|c| c.set(Some(0)));
        for n in 0..100 {
            for mode in [Lookup::Quantized, Lookup::Continuous] {
                f.query(
                    n as f32 * 7.31 - 180.,
                    n as f32 * 1.29 - 90.,
                    mode,
                    &mut out,
                    None,
                )
                .unwrap();
            }
        }
        assert!(
            f.query(f32::NAN, 0., Lookup::Continuous, &mut out, None)
                .is_err()
        );
        let count = COUNT.with(|c| c.replace(None).unwrap());
        assert_eq!(count, 0, "allocation in HRTF lookup");
    }
}

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
