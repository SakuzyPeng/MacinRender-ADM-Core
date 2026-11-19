//! `mr_adm_room_file_device_probe INPUT_ADM ROOM_JSON OTHER_ROOM_JSON DEVICE_ID [--hrir PATH]`:
//! realtime-room file monitoring on a real output device (formerly
//! `tests/manual/room_file_device_probe.cpp`, which drove the C++ SDK's `MonitorSession`). Release
//! builds only.
//!
//! It opens the production monitor session (the import, file streaming, rendering and the device
//! islands) on the device with exactly that id, applies a simulated 100 Hz head pose, requests a
//! room switch every 5 s while more than 3 s remain, and plays to the end of the file and the
//! room's tail. It prints progress once a second and a final `file_streaming_device` report; exit
//! code 0 when it ended without failing or underrunning, presented every frame, applied every
//! switch, produced output and sent the poses on time.
use mradm_dsp::room::quality::Quality;
use mradm_engine::request::Selection;
use mradm_ffi::api::monitor::{EngineMonitorSession, EngineMonitorSessionHost, choice};
use mradm_ffi::api::room::{RoomPreparedHandle, RoomProfileHandle, switch_config};
use mradm_ffi::api::scene::SceneHandle;
use mradm_json::Value;
use mradm_render::room_runtime::state;
use std::path::Path;
use std::process::ExitCode;
use std::time::{Duration, Instant};

#[path = "consistency/request.rs"]
#[allow(dead_code)]
mod consistency_request;
#[path = "probe/islands.rs"]
mod islands;
mod probe;
use islands::{obj, print};

fn message(e: &mradm_render::routing::Error) -> String {
    String::from_utf8_lossy(&e.message).into_owned()
}

/// The working set on Windows, 0 elsewhere (as the C++ probe reported).
fn resident_bytes() -> u64 {
    if cfg!(windows) { probe::rss() } else { 0 }
}

#[allow(clippy::too_many_lines)]
fn run() -> Result<bool, String> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 5 && (args.len() != 7 || args[5] != "--hrir") {
        return Err(
            "usage: room_file_device_probe INPUT_ADM ROOM_JSON OTHER_ROOM_JSON DEVICE_ID [--hrir PATH]"
                .to_owned(),
        );
    }
    let device = islands::outputs()
        .into_iter()
        .find(|o| o.id == args[4])
        .ok_or("Select an exact current device ID; no default fallback is allowed")?;
    let input = &args[1];
    let scene = SceneHandle::import(input.as_bytes(), false, None, &[]).map_err(|e| e.message)?;
    let info = scene.scene().info.clone();
    let objects = scene.scene().snapshot.objects.len();
    let load = |path: &str| RoomProfileHandle::load(path.as_bytes()).map_err(|e| e.to_string());
    let first = load(&args[2])?;
    let next = load(&args[3])?;
    let hrir = if args.len() == 7 {
        args[6].as_str()
    } else {
        ""
    };
    let mut request = consistency_request::request(Path::new(input), Path::new(input));
    request.output = None;
    request.renderer = Selection::RoomBinaural as u32;
    request.output_layout = "binaural".to_owned();
    request.binaural_spread = mradm_engine::backend::BinauralSpreadMode::None as u32;
    request.room = mradm_engine::render::RoomOptions {
        package: Some(first.package()),
        hrir: hrir.as_bytes().to_vec(),
        metres: 1.5,
        quality: Quality::Realtime as u32,
    };
    let host = EngineMonitorSessionHost {
        devices: islands::devices(),
        apple: islands::apple(),
    };
    // SAFETY: the island tables have the process lifetime; the session is dropped before `scene`.
    let monitor = unsafe {
        EngineMonitorSession::new(
            islands::platform(),
            &raw const *scene,
            info.sample_rate,
            info.frames,
            &host,
        )
    }
    .ok_or("invalid monitor session")?;
    let session = monitor.session();
    session
        .start(choice(request, false), device.id.as_bytes())
        .map_err(|e| message(&e))?;
    let prepared = RoomPreparedHandle::prepare(
        &first,
        hrir.as_bytes(),
        b"",
        None,
        Quality::Realtime as u32,
        1.5,
    )
    .map_err(|e| e.to_string())?;
    let prepared_report =
        mradm_json::parse(prepared.description().to_bytes()).map_err(|e| e.to_string())?;
    let control = session
        .room_control()
        .filter(|c| c.status().quality == Quality::Realtime)
        .ok_or("File monitor did not select the realtime room backend")?;
    let switch = |profile: &RoomProfileHandle| {
        switch_config(Some(profile), hrir.as_bytes(), 1.5, Quality::Realtime)
            .map_err(|e| message(&e))
    };
    let (first_switch, next_switch) = (switch(&first)?, switch(&next)?);
    let rate = info.sample_rate;
    let frames = info.frames;
    let requested_seconds = frames as f64 / f64::from(rate);
    let origin = Instant::now();
    let mut memory = Vec::new();
    let mut underruns = Vec::new();
    let mut last_underruns = 0u64;
    let mut peak_rss = resident_bytes();
    let (mut poses, mut switches, mut next_second) = (0u64, 0u64, 0u64);
    let mut max_pose_lateness_ms = 0f64;
    let mut peak = 0f32;
    let mut max_buffered = 0u64;
    print(&obj([
        ("case", "file_probe_ready".into()),
        ("device", device.name.clone().into()),
        ("quality", "realtime".into()),
        ("channels", u32::from(info.channels).into()),
        ("objects", objects.into()),
        ("requested_seconds", requested_seconds.into()),
        ("sample_rate", rate.into()),
    ]));
    session.set_orientation([0., 0., 0.]);
    session.play();
    loop {
        let elapsed = origin.elapsed().as_secs_f64();
        let status = session.status().ok_or("monitor has no engine")?;
        // `ended` marks the producer's EOS; the real device still owns buffered tail samples.
        if (status.ended && status.buffered_frames == 0)
            || status.failed
            || elapsed > requested_seconds + 30.
        {
            break;
        }
        let pose_time = poses as f64 / 100.;
        max_pose_lateness_ms = max_pose_lateness_ms.max((elapsed - pose_time) * 1000.);
        session.set_orientation([
            ((pose_time * 0.25).sin() * 90.) as f32,
            ((pose_time * 0.7).sin() * 20.) as f32,
            ((pose_time * 0.4).sin() * 10.) as f32,
        ]);
        poses += 1;
        if status.playhead_frames + u64::from(rate) * 3 < frames
            && elapsed >= 2. + switches as f64 * 5.
        {
            let room = if switches % 2 == 0 {
                &next_switch
            } else {
                &first_switch
            };
            control.request(room).map_err(|e| message(&e))?;
            switches += 1;
        }
        let room = control.status();
        if room.state == state::FAILED {
            return Err(room.message);
        }
        if status.underruns != last_underruns {
            last_underruns = status.underruns;
            underruns.push(obj([
                ("seconds", elapsed.into()),
                ("count", last_underruns.into()),
                ("room_state", room.state.into()),
            ]));
        }
        max_buffered = max_buffered.max(status.buffered_frames);
        if elapsed >= next_second as f64 {
            let rss = resident_bytes();
            peak_rss = peak_rss.max(rss);
            memory.push(obj([
                ("seconds", elapsed.into()),
                ("rss_bytes", rss.into()),
            ]));
            if let Some(levels) = session.levels() {
                for p in levels.peak.iter().take(levels.channels as usize) {
                    peak = peak.max(*p);
                }
            }
            print(&obj([
                ("case", "file_device_progress".into()),
                ("seconds", elapsed.into()),
                ("playhead_frames", status.playhead_frames.into()),
                ("underruns", status.underruns.into()),
                ("buffered_frames", status.buffered_frames.into()),
                ("rss_bytes", rss.into()),
                ("applied_revision", room.applied_revision.into()),
            ]));
            next_second += 1;
        }
        islands::wait_until(origin + Duration::from_millis(poses * 10));
    }
    let status = session.status().ok_or("monitor has no engine")?;
    let room = control.status();
    let logs: Vec<Value> = (0..session.log_count())
        .filter_map(|i| session.log_entry(i))
        .map(|(level, module, message)| {
            obj([
                ("level", level.into()),
                ("module", module.into()),
                (
                    "message",
                    String::from_utf8_lossy(&message).into_owned().into(),
                ),
            ])
        })
        .collect();
    let wall = origin.elapsed().as_secs_f64();
    let expected = frames + room.tail_frames;
    let passed = status.ended
        && !status.failed
        && status.underruns == 0
        && status.playhead_frames == expected
        && peak > 0.
        && room.applied_revision == switches
        && poses as f64 >= requested_seconds * 99.;
    drop(monitor);
    drop(scene);
    print(&obj([
        ("case", "file_streaming_device".into()),
        ("passed", passed.into()),
        ("device", device.name.into()),
        ("quality", "realtime".into()),
        ("requested_seconds", requested_seconds.into()),
        ("sample_rate", rate.into()),
        ("room_prepared", prepared_report),
        ("input_channels", u32::from(info.channels).into()),
        ("objects", objects.into()),
        ("wall_seconds", wall.into()),
        ("presented_frames", status.playhead_frames.into()),
        ("expected_frames", expected.into()),
        ("underruns", status.underruns.into()),
        ("underrun_events", Value::Array(underruns)),
        ("ended", status.ended.into()),
        ("failed", status.failed.into()),
        ("poses_sent", poses.into()),
        ("pose_sender_max_lateness_ms", max_pose_lateness_ms.into()),
        ("room_switch_requests", switches.into()),
        ("applied_revision", room.applied_revision.into()),
        ("sampled_peak", peak.into()),
        ("maximum_host_buffered_frames", max_buffered.into()),
        ("rss_peak_bytes", peak_rss.into()),
        ("memory_samples", Value::Array(memory)),
        ("logs", Value::Array(logs)),
        (
            "scope",
            "MonitorSession ADM import, file streaming PCM decode, rendering and real audio device; \
             simulated 100 Hz head input"
                .into(),
        ),
    ]));
    Ok(passed)
}

fn main() -> ExitCode {
    if cfg!(debug_assertions) {
        eprintln!("Use a Release build.");
        return ExitCode::from(2);
    }
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(1),
        Err(e) => {
            eprintln!(
                "{}",
                mradm_json::dump(
                    &obj([("case", "file_probe_error".into()), ("error", e.into())]),
                    None
                )
            );
            ExitCode::from(1)
        }
    }
}
