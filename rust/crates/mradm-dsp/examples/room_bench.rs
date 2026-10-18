//! Release-only room CPU deadline probe. No audio device is involved: missed
//! deadlines here are DSP misses, not claimed device underruns/latency.
use mradm_dsp::{
    geometry,
    room::{
        BLOCK,
        control::{RoomCommand, RoomDescription, State},
        hrir::HrirBank,
        prepared::PreparedRoom,
        quality::Quality,
        session::{Instance, PreparedSet, Session},
    },
};
use std::{path::Path, sync::Arc, time::Instant};
#[path = "room_support/assets.rs"]
mod assets;

#[cfg(feature = "room-profile")]
#[path = "room_bench/trace.rs"]
mod trace;

// Read counters outside the measured process call. Native host/Scene benchmarks
// provide these counters on macOS and Windows; this lightweight probe also runs
// on a headless Linux validation host without building device dependencies.
fn process_counters(ticks: Option<f64>) -> (Option<f64>, Option<u64>) {
    #[cfg(target_os = "linux")]
    {
        let cpu = ticks.and_then(|ticks| {
            let stat = std::fs::read_to_string("/proc/self/stat").ok()?;
            let values: Vec<_> = stat.rsplit_once(')')?.1.split_whitespace().collect();
            let user: u64 = values.get(11)?.parse().ok()?;
            let system: u64 = values.get(12)?.parse().ok()?;
            Some((user + system) as f64 / ticks)
        });
        let rss = std::fs::read_to_string("/proc/self/status")
            .ok()
            .and_then(|value| {
                value.lines().find_map(|line| {
                    line.strip_prefix("VmRSS:")?
                        .split_whitespace()
                        .next()?
                        .parse::<u64>()
                        .ok()
                        .map(|kb| kb * 1024)
                })
            });
        (cpu, rss)
    }
    #[cfg(target_os = "macos")]
    {
        let _ = ticks;
        let mut usage = std::mem::MaybeUninit::<libc::rusage>::uninit();
        // getrusage initializes the complete platform-defined structure.
        if unsafe { libc::getrusage(libc::RUSAGE_SELF, usage.as_mut_ptr()) } != 0 {
            return (None, None);
        }
        let usage = unsafe { usage.assume_init() };
        let seconds = |t: libc::timeval| t.tv_sec as f64 + t.tv_usec as f64 / 1e6;
        (
            Some(seconds(usage.ru_utime) + seconds(usage.ru_stime)),
            u64::try_from(usage.ru_maxrss).ok(),
        )
    }
    #[cfg(not(any(target_os = "linux", target_os = "macos")))]
    {
        let _ = ticks;
        (None, None)
    }
}

fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Use Release for performance evidence".into());
    }
    let mut arguments: Vec<_> = std::env::args().collect();
    let hrir_path = if let Some(index) = arguments.iter().position(|v| v == "--hrir") {
        let path = arguments
            .get(index + 1)
            .ok_or("--hrir needs a resource path")?
            .clone();
        arguments.drain(index..index + 2);
        Some(path)
    } else {
        None
    };
    let quality = if let Some(index) = arguments.iter().position(|v| v == "--quality") {
        let value = arguments
            .get(index + 1)
            .ok_or("--quality needs exact or realtime")?
            .parse::<Quality>()?;
        arguments.drain(index..index + 2);
        value
    } else {
        Quality::Exact
    };
    let scenario = if let Some(index) = arguments.iter().position(|v| v == "--scenario") {
        let value = arguments
            .get(index + 1)
            .ok_or("--scenario needs all, static, pose, switch, ramps or distance-ramps")?
            .clone();
        if !["all", "static", "pose", "switch", "ramps", "distance-ramps"].contains(&value.as_str())
        {
            return Err(
                "--scenario needs all, static, pose, switch, ramps or distance-ramps".into(),
            );
        }
        arguments.drain(index..index + 2);
        value
    } else {
        "all".to_owned()
    };
    let tracing = arguments.last().is_some_and(|v| v == "--trace");
    if tracing {
        arguments.pop();
        if !cfg!(feature = "room-profile") {
            return Err("--trace requires a Release build with --features room-profile".into());
        }
    }
    #[cfg(feature = "room-profile")]
    let clock_read_ns = tracing.then(trace::enable_cpu_clock).flatten();
    let ticks = if cfg!(target_os = "linux") {
        std::process::Command::new("getconf")
            .arg("CLK_TCK")
            .output()
            .ok()
            .filter(|v| v.status.success())
            .and_then(|v| String::from_utf8(v.stdout).ok()?.trim().parse::<f64>().ok())
            .filter(|v| v.is_finite() && *v > 0.)
    } else {
        None
    };
    if arguments.len() < 2 || arguments.len() > 6 {
        return Err(
            "usage: room_bench ROOM_JSON [CALLS=384] [FRAMES=256|512] [SOURCES=1|16|32] [SWITCH_PERIOD_CALLS=0] [--trace] [--quality exact|realtime] [--scenario all|static|pose|switch|ramps|distance-ramps] [--hrir SOFA_OR_MANIFEST]".into(),
        );
    }
    let file = Path::new(&arguments[1]);
    let root = file.parent().ok_or("missing parent")?;
    let blocks = arguments
        .get(2)
        .map(|s| s.parse())
        .transpose()?
        .unwrap_or(384usize);
    if blocks == 0 || blocks > 112_500 {
        return Err("BLOCKS must be 1..112500".into());
    }
    let frames = arguments
        .get(3)
        .map(|v| v.parse::<usize>())
        .transpose()?
        .unwrap_or(BLOCK);
    if frames != BLOCK && frames != 2 * BLOCK {
        return Err("FRAMES must be 256 or 512".into());
    }
    let profile = Arc::new(assets::profile(file)?);
    let selected = hrir_path
        .as_ref()
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| root.join(&profile.default_hrir));
    let data = Arc::new(assets::hrir_at_rate(&selected, profile.sample_rate)?);
    let (length, _, _) = PreparedRoom::required_fft(&profile, &data)?;
    let bank = Arc::new(HrirBank::with_policy(data, length, quality.fft())?);
    let prepared = Arc::new(PreparedSet::new(Arc::new(PreparedRoom::with_quality(
        profile.clone(),
        bank.clone(),
        quality,
    )?))?);
    let mut other = (*profile).clone();
    other.identity.push_str("-benchmark");
    other.absorption = [[0.9; 8]; 6];
    let other = Arc::new(PreparedSet::new(Arc::new(PreparedRoom::with_quality(
        Arc::new(other),
        bank,
        quality,
    )?))?);
    let only_sources = arguments.get(4).map(|v| v.parse::<usize>()).transpose()?;
    if only_sources.is_some_and(|n| ![1, 16, 32].contains(&n)) {
        return Err("SOURCES must be 1, 16 or 32".into());
    }
    let switch_period = arguments
        .get(5)
        .map(|v| v.parse::<usize>())
        .transpose()?
        .unwrap_or(0);
    for sources in [1, 16, 32] {
        if only_sources.is_some_and(|n| n != sources) {
            continue;
        }
        // The last scenario ramps every source with its own event offset, as
        // dense ADM automation does; per-source update grids bound its cost.
        for (name, moving, switching, ramps) in [
            ("static", false, false, false),
            ("pose", true, false, false),
            ("switch", true, true, false),
            ("ramps", true, false, true),
            ("distance-ramps", true, false, true),
        ] {
            if scenario != "all" && scenario != name {
                continue;
            }
            let mut session = Session::new(prepared.clone(), 1.5, true, false)?;
            let around = |i: usize, turn: f32| {
                let v = geometry::portable_direction(
                    i as f32 * 360. / sources as f32 + turn,
                    (i % 3) as f32 * 15. - 15.,
                );
                [
                    (-v[1] * 1.4) as f32,
                    (v[0] * 1.4) as f32,
                    (v[2] * 1.4) as f32,
                ]
            };
            let radial = |i: usize, block: usize| {
                let v = geometry::portable_direction(
                    -60. + 120. * i as f32 / sources.max(1) as f32,
                    0.,
                );
                let phase = ((block + i * 11) % 128) as f64 / 64.;
                let metres = 0.2 + 1.4 * (1. - (phase - 1.).abs());
                [
                    (-v[1] * metres / 1.5) as f32,
                    (v[0] * metres / 1.5) as f32,
                    0.,
                ]
            };
            let descriptions: Vec<_> = (0..sources)
                .map(|i| RoomDescription {
                    id: i as u64 + 1,
                    position: if name == "distance-ramps" {
                        radial(i, 0)
                    } else {
                        around(i, 0.)
                    },
                    ..RoomDescription::default()
                })
                .collect();
            let offsets: Vec<_> = (0..sources).map(|i| (i * 131 + 17) % frames).collect();
            let mut distinct = offsets.clone();
            distinct.sort_unstable();
            distinct.dedup();
            let mut commands = Vec::with_capacity(sources);
            session.configure(&descriptions)?;
            let mut candidate = if switching {
                Some(Box::new(Instance::new(other.clone(), sources)?))
            } else {
                None
            };
            let mut revision = 0u64;
            let mut preparation_seconds = 0.;
            let mut input = vec![0f32; frames];
            let mut output = vec![0f32; 2 * frames];
            let mut elapsed = Vec::with_capacity(blocks);
            #[cfg(feature = "room-profile")]
            let mut trace = tracing.then(|| trace::Trace::new(blocks, clock_read_ns));
            let mut peak = 0f32;
            let mut cpu_start = None;
            let mut rss_peak = 0u64;
            for block in 0..blocks + 32 {
                if block == 32 {
                    mradm_dsp::room::profile::reset();
                    let (cpu, rss) = process_counters(ticks);
                    cpu_start = cpu;
                    rss_peak = rss.unwrap_or(0);
                }
                for (i, v) in input.iter_mut().enumerate() {
                    *v = (((block * frames + i) % 179) as f32 - 89.) * 0.00001;
                }
                if moving {
                    session.set_pose([
                        block as f32 * 0.25,
                        block as f32 * 0.05,
                        block as f32 * 0.02,
                    ])?;
                }
                commands.clear();
                if ramps {
                    // Each call ramps every source to its next position over the call.
                    commands.extend((0..sources).map(|i| RoomCommand {
                        element: i as u32,
                        offset: offsets[i] as u32,
                        duration: frames as u32,
                        changed: 4,
                        state: State {
                            position: if name == "distance-ramps" {
                                radial(i, block + 1)
                            } else {
                                around(i, (block + 1) as f32 * 0.5)
                            },
                            ..State::default()
                        },
                        ..RoomCommand::default()
                    }));
                    commands.sort_by_key(|c| c.offset);
                }
                if switching
                    && (block == 64
                        || (switch_period != 0 && block > 64 && (block - 64) % switch_period == 0))
                {
                    if session.info().phase != 0 {
                        return Err("Switch period shorter than transition and tail".into());
                    }
                    revision += 1;
                    let prepare_start = Instant::now();
                    let next = candidate.take().map_or_else(
                        || {
                            Instance::new(
                                if revision % 2 == 1 {
                                    other.clone()
                                } else {
                                    prepared.clone()
                                },
                                sources,
                            )
                            .map(Box::new)
                        },
                        Ok,
                    )?;
                    session.queue_room(next, revision)?;
                    preparation_seconds += prepare_start.elapsed().as_secs_f64();
                }
                #[cfg(feature = "room-profile")]
                let trace_start = (block >= 32)
                    .then(|| trace.as_ref().map(|_| trace::start(session.info().phase)))
                    .flatten();
                let start = Instant::now();
                session.process(
                    frames,
                    (0..sources).map(|_| Some(input.as_slice())),
                    &[],
                    &commands,
                    &mut output,
                )?;
                let duration = start.elapsed().as_secs_f64();
                if block >= 32 {
                    elapsed.push(duration);
                    #[cfg(feature = "room-profile")]
                    if let (Some(trace), Some(before)) = (&mut trace, trace_start) {
                        trace.finish(block - 32, before, session.info().phase, duration);
                    }
                }
                if block % 64 == 0 {
                    rss_peak = rss_peak.max(process_counters(ticks).1.unwrap_or(0));
                }
                peak = output.iter().fold(peak, |a, b| a.max(b.abs()));
                let _retired = session.take_retired();
            }
            let total: f64 = elapsed.iter().sum();
            elapsed.sort_by(f64::total_cmp);
            let deadline = frames as f64 / f64::from(prepared.room.profile.sample_rate);
            let counters = process_counters(ticks);
            rss_peak = rss_peak.max(counters.1.unwrap_or(0));
            let cpu = cpu_start.zip(counters.0).map(|(start, end)| end - start);
            let profile = mradm_dsp::room::profile::snapshot();
            #[cfg(feature = "room-profile")]
            let trace_report = trace.map(|trace| trace.report(deadline));
            #[cfg(not(feature = "room-profile"))]
            let trace_report: Option<serde_json::Value> = None;
            let segments = cfg!(feature = "room-profile").then(|| {
                let index = mradm_dsp::room::profile::NAMES
                    .iter()
                    .position(|name| *name == "voice_segment")
                    .unwrap();
                profile[index].0
            });
            let stages: serde_json::Map<String, serde_json::Value> = mradm_dsp::room::profile::NAMES.iter().zip(profile).map(|(name, (calls, nanos))| ((*name).to_owned(), serde_json::json!({"calls": calls, "worker_summed_ms": nanos as f64 / 1e6}))).collect();
            println!(
                "{}",
                serde_json::json!({"scenario":name,"sources":sources,"continuous_pose":moving,"room_transition":switching,
                "quality":quality.name(),"processing_policy":quality.policy(),"reflection_update_frames":quality.reflection_frames_at(prepared.room.profile.sample_rate),
                "source_ramps":ramps,"distinct_event_offsets":if ramps{distinct.len()}else{0},
                "voice_segments":segments,
                "voice_segments_per_source_block":segments.map(|v|v as f64/(sources*blocks*frames/BLOCK) as f64),
                "room_switches":revision,"switch_period_calls":switch_period,"preparation_outside_dsp_seconds":preparation_seconds,
                "profiling":cfg!(feature = "room-profile"), "stages":stages, "trace":trace_report,
                "sample_rate":prepared.room.profile.sample_rate,"blocks":blocks,"call_frames":frames,"dsp_seconds":total,"audio_seconds":blocks as f64*deadline,
                "worker_wall_budget_percent":100.*total/(blocks as f64*deadline),
                "cpu_seconds":cpu,"cpu_audio_budget_percent":cpu.map(|v|100.*v/(blocks as f64*deadline)),
                "rss_peak_bytes":(rss_peak!=0).then_some(rss_peak),"pose_update_hz":if moving{f64::from(prepared.room.profile.sample_rate)/frames as f64}else{0.},
                "mean_ms":total*1000./blocks as f64,"p95_ms":elapsed[(blocks*95/100).min(blocks-1)]*1000.,
                "hrir_hash":prepared.room.hrir.data.identity,"hrir_bank_bytes":prepared.room.hrir.storage_bytes(),
                "memory_measurement":if cfg!(target_os="macos") {"process-peak-rusage"} else {"sampled-process-rss"},
                "p99_ms":elapsed[(blocks*99/100).min(blocks-1)]*1000.,
                "maximum_ms":elapsed[blocks-1]*1000.,"deadline_misses":elapsed.iter().filter(|v|**v>deadline).count(),
                "algorithm_fifo_frames":BLOCK,"final_phase":session.info().phase,"peak":peak})
            );
        }
    }
    Ok(())
}
