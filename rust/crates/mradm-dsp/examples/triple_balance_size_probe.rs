//! Research-only Triple Balance size probe (formerly `tests/unit/triple_balance_size_probe.cpp`,
//! R3 B); comparison PCM must come from a Release build.
//!
//! - `triple_balance_size_probe request.json`: renders `input_f32` (raw little-endian f32, mono)
//!   through one size object (`events`: `start_sample`, `xyz` in room-internal coordinates and
//!   `size`; `layout` 7.1.4 / 9.1.6 / 22.2; 48 kHz) in `block_frames` blocks (default 1024) and
//!   writes the interleaved output to `output_f32`.
//! - without arguments: reads quantized `x y z size` integers from stdin and prints the raw size
//!   gains of each complete group (`%.10g`, space-separated), before waiting for more input.
use mradm_dsp::triple_balance::{
    Event, Layout, Position, panner,
    processor::{Processor, Track},
};
use mradm_json::{Value, at};
use std::io::{BufRead, Write};
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

/// Raw size gains of each complete `x y z size` group; a token that is not an integer fails, an
/// incomplete last group ends the input.
fn gains(input: impl BufRead, mut out: impl Write) -> Result<(), String> {
    let mut token = Vec::new();
    let mut q = [0i32; 4];
    let mut count = 0;
    // The final delimiter also processes a token immediately followed by EOF.
    for byte in input.bytes().chain(std::iter::once(Ok(b' '))) {
        let byte = byte.map_err(|e| e.to_string())?;
        if !byte.is_ascii_whitespace() {
            token.push(byte);
            continue;
        }
        if token.is_empty() {
            continue;
        }
        let text = std::str::from_utf8(&token).map_err(|e| e.to_string())?;
        q[count] = text
            .parse()
            .map_err(|_| format!("invalid integer: {text}"))?;
        token.clear();
        count += 1;
        if count < 4 {
            continue;
        }
        let gains = panner::raw(q).map_err(|e| e.to_string())?;
        for g in gains {
            write!(out, "{} ", mradm_testkit::adm::g(f64::from(g), 10))
                .map_err(|e| e.to_string())?;
        }
        writeln!(out).map_err(|e| e.to_string())?;
        out.flush().map_err(|e| e.to_string())?;
        count = 0;
    }
    Ok(())
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let result = if args.len() == 2 {
        render(&args[1])
    } else {
        gains(std::io::stdin().lock(), std::io::stdout().lock())
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufReader, BufWriter, Cursor};
    use std::sync::mpsc;
    use std::time::Duration;

    const REPLY: &str = "1 0 0 0 0 0 0 0 0 0 0 \n";

    #[test]
    fn complete_groups_reply_before_stdin_eof() {
        let (input, mut requests) = std::io::pipe().unwrap();
        let (replies, output) = std::io::pipe().unwrap();
        let worker =
            std::thread::spawn(move || gains(BufReader::new(input), BufWriter::new(output)));
        let (send, receive) = mpsc::channel();
        let reader = std::thread::spawn(move || {
            for line in BufReader::new(replies).lines() {
                send.send(line.unwrap()).unwrap();
            }
        });
        let responses = [b"0 0 0 0 ".as_slice(), b"0\n0\t0\r\n0\n"].map(|request| {
            requests.write_all(request).unwrap();
            receive.recv_timeout(Duration::from_secs(5))
        });
        // Close the pipe before checking timeouts so a broken reader can still finish.
        drop(requests);
        worker.join().unwrap().unwrap();
        reader.join().unwrap();
        for response in responses {
            assert_eq!(response.unwrap(), REPLY.trim_end_matches('\n'));
        }
    }

    #[test]
    fn whitespace_final_tokens_and_incomplete_groups_keep_their_contract() {
        for input in ["0 0 0 0", "\t0\n0 0\r\n0\t", "0 0 0 0 1 2 3"] {
            let mut output = Vec::new();
            gains(Cursor::new(input), &mut output).unwrap();
            assert_eq!(output, REPLY.as_bytes());
        }
        let mut output = Vec::new();
        assert!(gains(Cursor::new("0 0 0 0 invalid"), &mut output).is_err());
        assert_eq!(output, REPLY.as_bytes());
    }
}
