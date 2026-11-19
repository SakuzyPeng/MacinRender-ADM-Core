//! Release preparation probe on the Rust room entries (`mradm_ffi::api::room`, the preparation the
//! C ABI calls), with all handles owned here. Cache-cold means no live prepared handle; the OS file
//! cache is retained. Until R3 G it called the same preparation through private C entries.
use mradm_ffi::api::room::{RoomPreparedHandle, RoomProfileHandle};
use std::time::Instant;

/// `RoomQuality::realtime`.
const REALTIME: u32 = 1;

fn prepare(profile: &RoomProfileHandle, path: &str) -> std::io::Result<Box<RoomPreparedHandle>> {
    RoomPreparedHandle::prepare(profile, path.as_bytes(), b"", None, REALTIME, 0.)
        .map_err(|e| std::io::Error::other(e.to_string()))
}

fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Use Release for preparation timing".into());
    }
    let args: Vec<_> = std::env::args().collect();
    if !(3..=4).contains(&args.len()) {
        return Err("usage: room_prepare_bench ROOM HRIR [REPETITIONS=3]".into());
    }
    let repeats = args
        .get(3)
        .map(|v| v.parse::<usize>())
        .transpose()?
        .unwrap_or(3);
    if repeats == 0 {
        return Err("repetitions must be positive".into());
    }
    let start = Instant::now();
    let profile = RoomProfileHandle::load(args[1].as_bytes())
        .map_err(|e| std::io::Error::other(e.to_string()))?;
    let profile_seconds = start.elapsed().as_secs_f64();
    let mut rows = Vec::new();
    let mut metadata = serde_json::Value::Null;
    for repetition in 1..=repeats {
        let start = Instant::now();
        let first = prepare(&profile, &args[2])?;
        let cold = start.elapsed().as_secs_f64();
        let start = Instant::now();
        let second = prepare(&profile, &args[2])?;
        let hit = start.elapsed().as_secs_f64();
        metadata = serde_json::from_str(first.description().to_str()?)?;
        let row = serde_json::json!({"repetition":repetition,"cold_seconds":cold,"active_hit_seconds":hit});
        eprintln!("{row}");
        rows.push(row);
        drop(second);
        drop(first);
    }
    println!(
        "{}",
        serde_json::json!({"schema":"mradm.room-private-prepare-benchmark","version":1,"profile_seconds":profile_seconds,"sample_rate":metadata["sample_rate"],"processing_policy":metadata["processing_policy"],"hrir_hash":metadata["hrir_hash"],"results":rows,"scope":"Release Rust room preparation (no C ABI); profile load separate, HRIR authentication/preparation and PreparedSet included; no live handle before cold, first handle pinned during hit; OS cache retained"})
    );
    Ok(())
}
