//! Release probe of production Rust kernels. Inputs are exact dyadic numbers.
use mradm_dsp::{
    Complex32, fft::RealFft, filterbank, geometry, hptf, mixing, resampler::Resampler, scene_math,
    spreader::Spreader,
};
use std::{
    fs,
    io::{BufWriter, Write},
    path::Path,
};
#[path = "phase2/coverage.rs"]
mod coverage;
#[path = "phase2/sofa.rs"]
mod sofa;
fn floats(path: &Path, name: &str, values: &[f32]) {
    assert!(!values.is_empty() && values.iter().all(|v| v.is_finite()));
    let mut out = BufWriter::new(fs::File::create(path.join(name)).unwrap());
    for v in values {
        out.write_all(&v.to_bits().to_le_bytes()).unwrap();
    }
    out.flush().unwrap();
}
fn doubles(path: &Path, name: &str, values: &[f64]) {
    assert!(!values.is_empty() && values.iter().all(|v| v.is_finite()));
    let mut out = BufWriter::new(fs::File::create(path.join(name)).unwrap());
    for v in values {
        out.write_all(&v.to_bits().to_le_bytes()).unwrap();
    }
    out.flush().unwrap();
}
fn integers(path: &Path, name: &str, values: &[i32]) {
    assert!(!values.is_empty());
    let mut out = BufWriter::new(fs::File::create(path.join(name)).unwrap());
    for value in values {
        out.write_all(&value.to_le_bytes()).unwrap();
    }
    out.flush().unwrap();
}
fn signal(size: usize) -> Vec<f32> {
    let mut state = 0x12345678u32;
    (0..size)
        .map(|_| {
            state = state.wrapping_mul(1664525).wrapping_add(1013904223);
            ((state >> 8) as i32 - 8388608) as f32 / 8388608.0
        })
        .collect()
}
fn run(out: &Path, fixtures: &Path) {
    fs::create_dir_all(out).unwrap();
    // Every twiddle RustFFT/realfft can build for these lengths: compute_twiddle(k, len) for each
    // power-of-two len <= 32768 and 0 <= k < len, with the crates' own formula. The scalar FFT is
    // deterministic given these f32 constants; the f64 values show where libm itself differs.
    let mut twiddle_f64 = Vec::new();
    let mut twiddle_f32 = Vec::new();
    for len in (1..=15).map(|p| 1usize << p) {
        let constant = -2f64 * std::f64::consts::PI / len as f64;
        for k in 0..len {
            let angle = constant * k as f64;
            let (re, im) = (angle.cos(), angle.sin());
            twiddle_f64.extend([re, im]);
            twiddle_f32.extend([re as f32, im as f32]);
        }
    }
    doubles(out, "fft-twiddles.10-libm.f64", &twiddle_f64);
    floats(out, "fft-twiddles.20-table.f32", &twiddle_f32);
    for size in [256, 512, 1024, 2048, 4096, 8192, 16384, 32768] {
        mradm_dsp::diagnostics::scope(&format!("fft-{size}"));
        let input = signal(size);
        let mut fft = RealFft::new(size).unwrap();
        let mut spectrum = vec![Complex32::default(); fft.bins()];
        fft.forward(&input, &mut spectrum).unwrap();
        let flat: Vec<_> = spectrum.iter().flat_map(|v| [v.re, v.im]).collect();
        floats(out, &format!("fft-{size}.10-input.f32"), &input);
        floats(out, &format!("fft-{size}.20-spectrum.c32"), &flat);
        let mut inverse = vec![0.; size];
        fft.inverse(&spectrum, &mut inverse).unwrap();
        floats(out, &format!("fft-{size}.30-inverse.f32"), &inverse);
    }
    // Portable sin/cos behind the resampler's sinc and window tables (ADR 0016): window
    // arguments up to 8π, sinc arguments up to ~380 (wider ranges also sampled) and points next
    // to multiples of π/2.
    let mut trig_input = Vec::new();
    let mut state = 0x2545_f491_4f6c_dd1du64;
    for scale in [1.0, 8.0 * std::f64::consts::PI, 1024.0, 131_072.0] {
        for _ in 0..4096 {
            state = state
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1);
            let unit = (state >> 11) as f64 / (1u64 << 53) as f64;
            trig_input.push((unit * 2.0 - 1.0) * scale);
        }
    }
    for k in -256i32..=256 {
        trig_input.push(f64::from(k) * std::f64::consts::FRAC_PI_2);
    }
    doubles(out, "trig.10-input.f64", &trig_input);
    let (sines, cosines): (Vec<_>, Vec<_>) =
        trig_input.iter().map(|&x| mradm_math::sin_cos(x)).unzip();
    doubles(out, "trig.20-sin.f64", &sines);
    doubles(out, "trig.30-cos.f64", &cosines);
    let directions: [f32; 12] = [
        0., 0., 30., 15., -179.75, 89.5, 179.75, -89.5, 37., 23., -19., 0.25,
    ];
    floats(out, "scene.10-input.f32", &directions);
    let mut values = Vec::new();
    for pair in directions.as_chunks::<2>().0 {
        let v = scene_math::direction(pair[0], pair[1]);
        values.extend(v);
        values.extend(scene_math::cartesian_to_polar(v));
        values.push(scene_math::length(v));
        values.extend(
            scene_math::Rotation::new([37., 23., -19.])
                .unwrap()
                .apply(pair[0], pair[1], false),
        );
    }
    floats(out, "scene.20-output.f32", &values);
    for layout_name in ["0+5+0", "4+7+0", "9+10+3"] {
        mradm_ear::diagnostics::scope(&format!("ear-{layout_name}"));
        let layout = mradm_ear::standard_layout(layout_name).unwrap();
        let geometry: Vec<_> = layout.channels.iter().flat_map(|ch| ch.real).collect();
        doubles(out, &format!("ear-{layout_name}.10-layout.f64"), &geometry);
        floats(
            out,
            &format!("ear-{layout_name}.20-fir.f32"),
            &mradm_ear::decorrelate::filters(&layout),
        );
    }
    for (index, power) in [0., 1.0e-20, 0.125, 1.].into_iter().enumerate() {
        mradm_dsp::diagnostics::scope(&format!("om-{index}"));
        let cx = mixing::RealMatrix::new(power + 0.5, 0.125, 0.125, power + 0.25);
        let cy = mixing::RealMatrix::new(power + 0.75, 0.0625, 0.0625, power + 0.5);
        floats(
            out,
            &format!("om-{index}.10-input.f32"),
            &[cx.as_slice(), cy.as_slice()].concat(),
        );
        let result = mixing::real_mix(cx, cy, 0.125).unwrap();
        floats(out, &format!("om-{index}.20-real.f32"), result.as_slice());
        let cx = cx.map(|v| Complex32::new(v, 0.));
        let cy = cy.map(|v| Complex32::new(v, 0.));
        let (result, residual) = mixing::complex_mix(cx, cy, 0.125).unwrap();
        floats(
            out,
            &format!("om-{index}.30-complex.c32"),
            &result.iter().flat_map(|v| [v.re, v.im]).collect::<Vec<_>>(),
        );
        floats(
            out,
            &format!("om-{index}.40-residual.f32"),
            residual.as_slice(),
        );
    }
    // OM spreader (ADR 0017): a synthetic 26-direction HRIR grid, its Voronoi weights and
    // filterbank coefficients, then two moving, widening sources over several blocks.
    mradm_dsp::diagnostics::scope("spreader");
    let mut dirs = vec![0., 90., 0., -90.];
    for (ring, elevation) in [-45f32, 0., 30., 60.].into_iter().enumerate() {
        for k in 0..6 {
            dirs.extend([k as f32 * 60. + ring as f32 * 7.5 - 180., elevation]);
        }
    }
    let taps = 64;
    let mut ir = signal(dirs.len() * taps);
    for (index, value) in ir.iter_mut().enumerate() {
        *value /= ((index % taps) / 8 + 1) as f32;
    }
    let pcm = signal(2 * filterbank::FRAME * 6);
    floats(
        out,
        "spreader.10-input.f32",
        &[&dirs[..], &ir, &pcm].concat(),
    );
    let vertices = geometry::portable_directions(&dirs).unwrap();
    floats(
        out,
        "spreader.20-voronoi.f32",
        &geometry::voronoi_weights(&vertices).unwrap(),
    );
    let coefficients = filterbank::fir_coefficients(&ir, vertices.len(), taps).unwrap();
    floats(
        out,
        "spreader.30-fir.c32",
        &coefficients
            .iter()
            .flat_map(|v| [v.re, v.im])
            .collect::<Vec<_>>(),
    );
    let mut spreader = Spreader::new(&ir, &dirs, taps, 48000, &[7, 9]).unwrap();
    let mut output = Vec::new();
    let mut block = vec![0.; filterbank::FRAME * 2];
    for (index, input) in pcm
        .as_chunks::<{ filterbank::FRAME * 2 }>()
        .0
        .iter()
        .enumerate()
    {
        let step = index as f32;
        spreader
            .set_source(0, 37.5 + step * 22.5, 12.25 - step * 3.5, 15. + step * 30.)
            .unwrap();
        spreader
            .set_source(
                1,
                -101.25 - step * 11.25,
                -20. + step * 9.,
                120. + step * 45.,
            )
            .unwrap();
        spreader.process(input, &mut block).unwrap();
        output.extend_from_slice(&block);
    }
    floats(out, "spreader.40-output.f32", &output);
    coverage::run(out);
    // Leave the OM/spreader scope: later kernels have no internal checkpoints of their own.
    mradm_dsp::diagnostics::scope("");
    for (input_rate, output_rate) in [
        (48000, 48000),
        (48000, 44100),
        (44100, 48000),
        (96000, 96000),
        (48000, 96000),
        (96000, 48000),
        (48000, 192000),
        (192000, 48000),
    ] {
        let input = signal(2051 * 2);
        let mut resampler = Resampler::new(2, input_rate, output_rate).unwrap();
        let mut pcm = Vec::new();
        let mut buffer = vec![0.; 1024 * 2];
        let mut offset = 0;
        while offset < input.len() / 2 {
            let progress = resampler
                .process(&input[offset * 2..], &mut buffer)
                .unwrap();
            assert!(progress.input_frames + progress.output_frames > 0);
            offset += progress.input_frames;
            pcm.extend_from_slice(&buffer[..progress.output_frames * 2]);
        }
        loop {
            let n = resampler.finish(&mut buffer).unwrap();
            if n == 0 {
                break;
            }
            pcm.extend_from_slice(&buffer[..n * 2]);
        }
        assert_eq!(pcm.len(), resampler.output_length(2051).unwrap() * 2);
        floats(
            out,
            &format!("resampler-{input_rate}-{output_rate}.10-input.f32"),
            &input,
        );
        floats(
            out,
            &format!("resampler-{input_rate}-{output_rate}.20-output.f32"),
            &pcm,
        );
    }
    for rate in [44100, 48000] {
        let band = hptf::Band {
            kind: hptf::BandKind::Peaking,
            enabled: true,
            frequency: 1700.,
            gain_db: 6.,
            q: 0.75,
        };
        doubles(
            out,
            &format!("hptf-{rate}.10-input.f64"),
            &[1700., 6., 0.75, -3., rate as f64],
        );
        let c = hptf::design(&[band], -3., rate, hptf::PreampMode::AutoTrim).unwrap();
        let b = c.sections[0];
        floats(
            out,
            &format!("hptf-{rate}.20-coefficients.f32"),
            &[
                b.b0,
                b.b1,
                b.b2,
                b.a1,
                b.a2,
                c.preamp_gain,
                c.auto_trim_db,
                c.max_response_db,
            ],
        );
    }
    let mut capabilities = vec![format!("arch={}", std::env::consts::ARCH)];
    #[cfg(target_arch = "x86_64")]
    capabilities.extend([
        format!("avx={}", std::is_x86_feature_detected!("avx")),
        format!("avx2={}", std::is_x86_feature_detected!("avx2")),
        format!("fma={}", std::is_x86_feature_detected!("fma")),
        format!("sse4.1={}", std::is_x86_feature_detected!("sse4.1")),
    ]);
    #[cfg(target_arch = "aarch64")]
    capabilities.push(format!(
        "neon={}",
        std::arch::is_aarch64_feature_detected!("neon")
    ));
    capabilities.push(format!("diagnostics={}", cfg!(feature = "diagnostics")));
    capabilities.push("fft_dispatch=scalar (RustFFT SIMD features disabled)".into());
    fs::write(out.join("capabilities.txt"), capabilities.join("\n") + "\n").unwrap();
    sofa::run(out, fixtures);
}

fn main() {
    let out = std::env::args_os().nth(1).expect("output directory");
    let fixtures = std::env::args_os()
        .nth(2)
        .expect("pinned SOFA fixture directory");
    for pass in 1..=2 {
        run(
            &Path::new(&out).join(format!("pass-{pass}")),
            Path::new(&fixtures),
        );
    }
}
