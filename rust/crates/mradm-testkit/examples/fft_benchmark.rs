//! `mr_adm_fft_benchmark`: Release-only timings of the production real FFT (formerly
//! `tests/tools/fft_benchmark.cpp`, which called the same plan through `mradm_dsp_fft_*`).
//! Planning is untimed; each length reports the mean forward and inverse time and FNV-1a 64
//! fingerprints of the spectrum and inverse bits (schema `mradm.fft.benchmark.v1`, read by
//! `scripts/consistency/benchmark-fft.py`).
use mradm_dsp::fft::RealFft;
use mradm_json::{Map, Value, dump};
use std::process::ExitCode;
use std::time::Instant;

const LENGTHS: [usize; 9] = [128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768];

fn bit_hash(samples: &[f32]) -> Result<u64, String> {
    let mut hash = 14695981039346656037u64;
    for &s in samples {
        if !s.is_finite() {
            return Err("nonfinite FFT result".to_owned());
        }
        for byte in s.to_bits().to_le_bytes() {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(1099511628211);
        }
    }
    Ok(hash)
}

fn measure(length: usize) -> Result<Value, String> {
    let error = |e: mradm_dsp::Error| format!("{e:?}");
    let mut fft = RealFft::new(length).map_err(error)?;
    let mut seed = 73u32;
    let input: Vec<f32> = (0..length)
        .map(|_| {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            ((seed >> 8) as i32 - 8_388_608) as f32 / 8_388_608.0
        })
        .collect();
    let mut spectrum = vec![0f32; length + 2];
    let mut output = vec![0f32; length];
    let iterations = 20_000_000 / length;
    let start = Instant::now();
    for _ in 0..iterations {
        fft.forward_interleaved(&input, &mut spectrum)
            .map_err(error)?;
    }
    let middle = Instant::now();
    for _ in 0..iterations {
        fft.inverse_interleaved(&spectrum, &mut output)
            .map_err(error)?;
    }
    let end = Instant::now();
    let per = |d: std::time::Duration| d.as_secs_f64() * 1e9 / iterations as f64;
    let mut row = Map::new();
    row.insert("length".to_owned(), Value::Unsigned(length as u64));
    row.insert("iterations".to_owned(), Value::Unsigned(iterations as u64));
    row.insert("forward_ns".to_owned(), Value::Float(per(middle - start)));
    row.insert("inverse_ns".to_owned(), Value::Float(per(end - middle)));
    row.insert(
        "spectrum_fnv1a64".to_owned(),
        Value::String(bit_hash(&spectrum)?.to_string()),
    );
    row.insert(
        "inverse_fnv1a64".to_owned(),
        Value::String(bit_hash(&output)?.to_string()),
    );
    Ok(Value::Object(row))
}

fn main() -> ExitCode {
    if cfg!(debug_assertions) {
        eprintln!("FFT performance evidence requires a Release build");
        return ExitCode::from(2);
    }
    let mut measurements = Vec::new();
    for length in LENGTHS {
        match measure(length) {
            Ok(row) => measurements.push(row),
            Err(e) => {
                eprintln!("{e}");
                return ExitCode::from(1);
            }
        }
    }
    let mut report = Map::new();
    report.insert(
        "schema".to_owned(),
        Value::String("mradm.fft.benchmark.v1".to_owned()),
    );
    report.insert("measurements".to_owned(), Value::Array(measurements));
    println!("{}", dump(&Value::Object(report), Some(2)));
    ExitCode::SUCCESS
}
