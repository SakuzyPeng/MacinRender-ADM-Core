//! Research-only Triple Balance size probe (formerly `tests/unit/triple_balance_size_probe.cpp`,
//! R3 B); comparison PCM must come from a Release build.
//!
//! - `triple_balance_size_probe request.json`: renders `input_f32` (raw little-endian f32, mono)
//!   through one size object (`events`: `start_sample`, `xyz` in room-internal coordinates and
//!   `size`; `layout` 7.1.4 / 9.1.6 / 22.2; 48 kHz) in `block_frames` blocks (default 1024) and
//!   writes the interleaved output to `output_f32`.
//! - without arguments: reads quantized `x y z size` integers from stdin and prints the raw size
//!   gains of each line (`%.10g`, space-separated).
use mradm_dsp::triple_balance::{
    Event, Layout, Position, panner,
    processor::{Processor, Track},
};
use mradm_json::{Value, at};
use std::io::{Read, Write};
use std::process::ExitCode;
use std::sync::Arc;

fn field<'a>(v: &'a Value, key: &str) -> Result<&'a Value, String> {
    at(v, key)
}

fn render(path: &str) -> Result<(), String> {
    let request = mradm_json::parse(&std::fs::read(path).map_err(|e| e.to_string())?)
        .map_err(|e| format!("{e:?}"))?;
    let mut events = Vec::new();
    for e in field(&request, "events")?.as_array().ok_or("events")? {
        let xyz = field(e, "xyz")?.as_array().ok_or("xyz")?;
        let f = |i: usize| xyz.get(i).and_then(Value::as_f32).ok_or("xyz value");
        events.push(Event {
            start: field(e, "start_sample")?.as_f64().ok_or("start_sample")? as u64,
            position: Position {
                x: f(0)?,
                y: f(1)?,
                z: f(2)?,
            },
            size: field(e, "size")?.as_f32().ok_or("size")?,
        });
    }
    let layout = match field(&request, "layout")?.as_str() {
        Some("7.1.4" | "4+7+0") => Layout::Seven,
        Some("9.1.6") => Layout::Nine,
        Some("22.2" | "9+10+3") => Layout::Room222,
        _ => return Err("unsupported layout".to_owned()),
    };
    let mut processor = Processor::new(Arc::new(
        Track::new(events, layout, 48000).map_err(|e| e.to_string())?,
    ));
    let block = match request.as_object().and_then(|o| o.get("block_frames")) {
        Some(v) => v.as_unsigned_u32().ok_or("block_frames")? as usize,
        None => 1024,
    };
    let text = |key| -> Result<String, String> {
        Ok(field(&request, key)?.as_str().ok_or(key)?.to_owned())
    };
    let raw = std::fs::read(text("input_f32")?).map_err(|e| e.to_string())?;
    let input: Vec<f32> = raw
        .as_chunks::<4>()
        .0
        .iter()
        .map(|b| f32::from_le_bytes(*b))
        .collect();
    let mut output = std::io::BufWriter::new(
        std::fs::File::create(text("output_f32")?).map_err(|e| e.to_string())?,
    );
    let mut write = |samples: &[f32]| -> Result<(), String> {
        for s in samples {
            output
                .write_all(&s.to_le_bytes())
                .map_err(|e| e.to_string())?;
        }
        Ok(())
    };
    for chunk in input.chunks(block.max(1)) {
        let mut part = vec![
            0.;
            processor
                .required(chunk.len(), false)
                .map_err(|e| e.to_string())?
        ];
        processor
            .process(chunk, &mut part, false)
            .map_err(|e| e.to_string())?;
        write(&part)?;
    }
    let mut tail = vec![0.; processor.required(0, true).map_err(|e| e.to_string())?];
    processor
        .process(&[], &mut tail, true)
        .map_err(|e| e.to_string())?;
    write(&tail)?;
    output.flush().map_err(|e| e.to_string())
}

/// Raw size gains of each complete `x y z size` line; a token that is not an integer fails, an
/// incomplete last group ends the input.
fn gains() -> Result<(), String> {
    let mut text = String::new();
    std::io::stdin()
        .read_to_string(&mut text)
        .map_err(|e| e.to_string())?;
    let mut out = std::io::stdout().lock();
    let tokens: Vec<&str> = text.split_ascii_whitespace().collect();
    for group in tokens.chunks(4) {
        let mut q = [0i32; 4];
        for (v, t) in q.iter_mut().zip(group) {
            *v = t.parse().map_err(|_| format!("invalid integer: {t}"))?;
        }
        if group.len() < 4 {
            break;
        }
        let gains = panner::raw(q).map_err(|e| e.to_string())?;
        for g in gains {
            write!(out, "{} ", mradm_testkit::adm::g(f64::from(g), 10))
                .map_err(|e| e.to_string())?;
        }
        writeln!(out).map_err(|e| e.to_string())?;
    }
    Ok(())
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let result = if args.len() == 2 {
        render(&args[1])
    } else {
        gains()
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}
