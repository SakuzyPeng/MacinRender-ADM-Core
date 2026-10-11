//! Release-only room CPU deadline probe. No audio device is involved: missed
//! deadlines here are DSP misses, not claimed device underruns/latency.
use mradm_dsp::{
    Error, Result, geometry,
    room::{
        BLOCK,
        control::RoomDescription,
        hrir::HrirBank,
        prepared::PreparedRoom,
        resources::{HrirData, RoomProfile},
        session::{Instance, PreparedSet, Session},
    },
};
use std::{fs, path::Path, sync::Arc, time::Instant};

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
            let stat = fs::read_to_string("/proc/self/stat").ok()?;
            let values: Vec<_> = stat.rsplit_once(')')?.1.split_whitespace().collect();
            let user: u64 = values.get(11)?.parse().ok()?;
            let system: u64 = values.get(12)?.parse().ok()?;
            Some((user + system) as f64 / ticks)
        });
        let rss = fs::read_to_string("/proc/self/status")
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
    #[cfg(not(target_os = "linux"))]
    {
        let _ = ticks;
        (None, None)
    }
}

fn asset(root: &Path, name: &str, expected: usize) -> Result<Vec<u8>> {
    let value = fs::read(root.join(name)).map_err(|_| Error::Io("Missing asset"))?;
    if value.len() != expected {
        return Err(Error::Io("Asset size mismatch"));
    }
    Ok(value)
}
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Use Release for performance evidence".into());
    }
    let mut arguments: Vec<_> = std::env::args().collect();
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
            "usage: room_bench ROOM_JSON [CALLS=384] [FRAMES=256|512] [SOURCES=1|16|32] [SWITCH_PERIOD_CALLS=0] [--trace]".into(),
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
    let profile = Arc::new(RoomProfile::parse(&fs::read(file)?, |n, s| {
        asset(root, n, s)
    })?);
    let data = Arc::new(HrirData::parse(
        &fs::read(root.join(&profile.default_hrir))?,
        |n, s| asset(root, n, s),
    )?);
    let (length, _, _) = PreparedRoom::required_fft(&profile, &data)?;
    let bank = Arc::new(HrirBank::new(data, length)?);
    let prepared = Arc::new(PreparedSet::new(Arc::new(PreparedRoom::new(
        profile.clone(),
        bank.clone(),
    )?))?);
    let mut other = (*profile).clone();
    other.identity.push_str("-benchmark");
    other.absorption = [[0.9; 8]; 6];
    let other = Arc::new(PreparedSet::new(Arc::new(PreparedRoom::new(
        Arc::new(other),
        bank,
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
        for (moving, switching) in [(false, false), (true, false), (true, true)] {
            let mut session = Session::new(prepared.clone(), 1.5, true, false)?;
            let descriptions: Vec<_> = (0..sources)
                .map(|i| {
                    let v = geometry::portable_direction(
                        i as f32 * 360. / sources as f32,
                        (i % 3) as f32 * 15. - 15.,
                    );
                    RoomDescription {
                        id: i as u64 + 1,
                        position: [
                            (-v[1] * 1.4) as f32,
                            (v[0] * 1.4) as f32,
                            (v[2] * 1.4) as f32,
                        ],
                        ..RoomDescription::default()
                    }
                })
                .collect();
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
                    &[],
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
            let deadline = frames as f64 / 48000.;
            let counters = process_counters(ticks);
            rss_peak = rss_peak.max(counters.1.unwrap_or(0));
            let cpu = cpu_start.zip(counters.0).map(|(start, end)| end - start);
            let profile = mradm_dsp::room::profile::snapshot();
            #[cfg(feature = "room-profile")]
            let trace_report = trace.map(|trace| trace.report(deadline));
            #[cfg(not(feature = "room-profile"))]
            let trace_report: Option<serde_json::Value> = None;
            let stages: serde_json::Map<String, serde_json::Value> = mradm_dsp::room::profile::NAMES.iter().zip(profile).map(|(name, (calls, nanos))| ((*name).to_owned(), serde_json::json!({"calls": calls, "worker_summed_ms": nanos as f64 / 1e6}))).collect();
            println!(
                "{}",
                serde_json::json!({"sources":sources,"continuous_pose":moving,"room_transition":switching,
                "room_switches":revision,"switch_period_calls":switch_period,"preparation_outside_dsp_seconds":preparation_seconds,
                "profiling":cfg!(feature = "room-profile"), "stages":stages, "trace":trace_report,
                "blocks":blocks,"call_frames":frames,"dsp_seconds":total,"audio_seconds":blocks as f64*deadline,
                "worker_wall_budget_percent":100.*total/(blocks as f64*deadline),
                "cpu_seconds":cpu,"cpu_audio_budget_percent":cpu.map(|v|100.*v/(blocks as f64*deadline)),
                "rss_peak_bytes":(rss_peak!=0).then_some(rss_peak),"pose_update_hz":if moving{48000./frames as f64}else{0.},
                "p99_ms":elapsed[(blocks*99/100).min(blocks-1)]*1000.,
                "maximum_ms":elapsed[blocks-1]*1000.,"deadline_misses":elapsed.iter().filter(|v|**v>deadline).count(),
                "algorithm_fifo_frames":BLOCK,"final_phase":session.info().phase,"peak":peak})
            );
        }
    }
    Ok(())
}
