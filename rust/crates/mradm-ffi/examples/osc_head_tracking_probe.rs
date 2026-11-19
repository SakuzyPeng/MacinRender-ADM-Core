//! `mr_adm_osc_head_tracking_probe [port 0..65535] [seconds 1..60] [source_id]`: a headless OSC
//! loopback receiver for checking the platform UDP bind against a real sender (formerly
//! `tests/manual/osc_head_tracking_probe.cpp`, which used the C++ SDK's `OscHeadTrackingReceiver`).
//!
//! It binds the port exclusively, checks that a second receiver cannot bind it, prints
//! `PORT <bound>` when ready, polls for `seconds`, then stops, checks that the port is released and
//! prints the final snapshot JSON. Exit code 0 when a pose arrived, 2 when none did, 1 on an error.
use mradm_ffi::api::osc::receiver as bind;
use mradm_osc::receiver::State;
use std::io::Write;
use std::process::ExitCode;
use std::time::{Duration, Instant};

/// An unsigned decimal argument: digits only, as `std::from_chars` accepted.
fn number(text: &str) -> Result<u32, String> {
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_digit()) {
        return Err("expected an unsigned decimal argument".to_owned());
    }
    text.parse()
        .map_err(|_| "expected an unsigned decimal argument".to_owned())
}

fn run() -> Result<ExitCode, String> {
    let args: Vec<String> = std::env::args().collect();
    let port = args.get(1).map_or(Ok(9000), |a| number(a))?;
    let seconds = args.get(2).map_or(Ok(5), |a| number(a))?;
    if args.len() > 4 || port > 65535 || seconds == 0 || seconds > 60 {
        return Err(
            "usage: osc_head_tracking_probe [port 0..65535] [seconds 1..60] [source_id]".to_owned(),
        );
    }
    let source = args.get(3).map_or(&b""[..], |s| s.as_bytes());
    let receiver = bind(port as u16, source);
    if let Err(e) = receiver.start() {
        eprintln!("{}", e.message);
        return Ok(ExitCode::from(1));
    }
    let bound_port = receiver.snapshot().bound_port;
    let occupied = bind(bound_port, b"");
    if occupied.start().is_ok() {
        return Err("exclusive bind was not enforced".to_owned());
    }
    // Readiness for a subprocess-driven sender.
    let mut out = std::io::stdout().lock();
    writeln!(out, "PORT {bound_port}")
        .and_then(|()| out.flush())
        .map_err(|e| e.to_string())?;
    let end = Instant::now() + Duration::from_secs(u64::from(seconds));
    while Instant::now() < end {
        if receiver.snapshot().state == State::Failed {
            eprintln!("{}", receiver.last_error());
            return Ok(ExitCode::from(1));
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let result = receiver.snapshot();
    let report = receiver.snapshot_json()?;
    receiver.stop();
    if receiver.snapshot().fresh || occupied.start().is_err() {
        return Err("stop did not clear activity and release the port".to_owned());
    }
    occupied.stop();
    writeln!(out, "{report}").map_err(|e| e.to_string())?;
    Ok(ExitCode::from(if result.has_pose { 0 } else { 2 }))
}

fn main() -> ExitCode {
    run().unwrap_or_else(|e| {
        eprintln!("{e}");
        ExitCode::from(1)
    })
}
