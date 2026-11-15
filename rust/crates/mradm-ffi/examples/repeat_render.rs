//! `mr_adm_repeat_render <input.wav> <output-prefix> [--reset-rng] [--cloud]`: two full binaural
//! renders in one process (formerly `tests/tools/repeat_render.cpp`, which drove the C++
//! `RenderService`). Release-only measurement: `render-matrix.sh` and `collect.py` compare the
//! two outputs; this tool fails only on rendering errors. The binaural backend's log lines go to
//! stdout (`collect.py` reads its pool and grouping reports); `MR_ADM_TRACE_DIR`, when set, is
//! narrowed to `pass-N` for each pass.
//!
//! `--reset-rng` is accepted for the scripts that still pass it: it reseeded the C runtime's RNG,
//! which no renderer uses since the decorrelator's random state moved into Rust instances.
use mradm_engine::backend::BinauralSpreadMode;
use mradm_engine::render::{Event, Render, Reuse};
use mradm_engine::request::{BitDepth, Selection};
use mradm_ffi::api::render::FfiHost;
use std::io::Write;
use std::path::Path;
use std::process::ExitCode;

#[path = "consistency/request.rs"]
mod consistency_request;
use consistency_request::{base_platform, request};

fn render(input: &Path, output: &Path, cloud: bool) -> Result<(), String> {
    let mut r = request(input, output);
    r.renderer = Selection::SafBinaural as u32;
    r.output_layout = "binaural".to_owned();
    r.output_bit_depth = BitDepth::F32 as u32;
    r.peak_limit = false;
    r.binaural_spread = if cloud {
        BinauralSpreadMode::Cloud
    } else {
        BinauralSpreadMode::SafSpreader
    } as u32;
    let mut render = Render::new(r, base_platform(), Reuse::default());
    let mut out = std::io::stdout().lock();
    loop {
        match render.next(false, &FfiHost) {
            Event::Log(l) if l.module == "binaural" => {
                writeln!(out, "{}", l.message).map_err(|e| e.to_string())?;
            }
            Event::Log(_) | Event::Progress(_) | Event::Continue => {}
            Event::Island(_) => return Err("unexpected render island".to_owned()),
            Event::Done => break,
        }
    }
    out.flush().map_err(|e| e.to_string())?;
    match render.outcome().and_then(|o| o.error.as_ref()) {
        Some(error) => Err(error.message.clone()),
        None => Ok(()),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!(
            "usage: mr_adm_repeat_render <input.wav> <output-prefix> [--reset-rng] [--cloud]"
        );
        return ExitCode::from(2);
    }
    let mut cloud = false;
    for option in &args[3..] {
        match option.as_str() {
            "--reset-rng" => {}
            "--cloud" => cloud = true,
            _ => {
                eprintln!("unknown option: {option}");
                return ExitCode::from(2);
            }
        }
    }
    let trace = std::env::var_os("MR_ADM_TRACE_DIR").filter(|t| !t.is_empty());
    let threads = std::thread::available_parallelism().map_or(0, |n| n.get());
    println!("hardware_concurrency={threads}");
    for pass in 1..=2 {
        if let Some(trace) = &trace {
            let mut directory = trace.clone();
            directory.push(format!("/pass-{pass}"));
            // SAFETY: no render is running; the engine reads the variable only while rendering.
            unsafe { std::env::set_var("MR_ADM_TRACE_DIR", directory) };
        }
        let output = format!("{}-{pass}.wav", args[2]);
        println!("same_process_pass={pass}");
        if let Err(message) = render(Path::new(&args[1]), Path::new(&output), cloud) {
            eprintln!("{message}");
            return ExitCode::from(2);
        }
    }
    ExitCode::SUCCESS
}
