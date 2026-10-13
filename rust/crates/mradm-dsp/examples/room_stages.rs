//! Release-only numerical stage profiler, with no device and no resource I/O in timings.
use mradm_dsp::{
    Error, Result, geometry,
    room::{
        BLOCK, geometry as room_geometry,
        geometry::Rotation,
        hrir::HrirBank,
        partition::Dynamic,
        prepared::{PreparedRoom, ResponseWork},
        quality::Quality,
        resources::{HrirData, RoomProfile},
    },
};
use std::{fs, path::Path, sync::Arc, time::Instant};
fn asset(root: &Path, name: &str, size: usize) -> Result<Vec<u8>> {
    let b = fs::read(root.join(name)).map_err(|_| Error::Io("asset"))?;
    if b.len() != size {
        return Err(Error::Io("size"));
    }
    Ok(b)
}
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Release required".into());
    }
    let mut args: Vec<_> = std::env::args().collect();
    let quality = if let Some(index) = args.iter().position(|v| v == "--quality") {
        let value = args
            .get(index + 1)
            .ok_or("--quality needs exact or realtime")?
            .parse::<Quality>()?;
        args.drain(index..index + 2);
        value
    } else {
        Quality::Exact
    };
    if args.len() < 2 {
        return Err(
            "usage: room_stages ROOM_JSON [ITERATIONS=2000] [--quality exact|realtime]".into(),
        );
    }
    let count = args
        .get(2)
        .map(|v| v.parse::<usize>())
        .transpose()?
        .unwrap_or(2000);
    if count == 0 || count > 100000 {
        return Err("ITERATIONS must be 1..100000".into());
    }
    let file = Path::new(&args[1]);
    let root = file.parent().unwrap();
    let profile = Arc::new(RoomProfile::parse(&fs::read(file)?, |n, s| {
        asset(root, n, s)
    })?);
    let data = Arc::new(HrirData::parse(
        &fs::read(root.join(&profile.default_hrir))?,
        |n, s| asset(root, n, s),
    )?);
    let (n, _, _) = PreparedRoom::required_fft(&profile, &data)?;
    let bank = Arc::new(HrirBank::with_policy(data, n, quality.fft())?);
    let room = Arc::new(PreparedRoom::with_quality(profile, bank, quality)?);
    let mut work = ResponseWork::new(&room)?;
    let position = room_geometry::position(&room.profile, [0., 1.4, 0.], 1.5, [1., 0., 0.])?;
    let mut face = usize::MAX;
    let mut output = vec![0.; room.hrir.stride() * 4];
    let start = Instant::now();
    for i in 0..count {
        room.hrir.query(
            geometry::portable_direction(i as f32 * 0.05, 7.),
            &mut face,
            &mut output,
        )?;
    }
    let hrir_query_us = start.elapsed().as_secs_f64() * 1e6 / count as f64;
    let start = Instant::now();
    for i in 0..count {
        work.build(&room, position, &Rotation::new([i as f32 * 0.05, 7., 0.])?)?;
    }
    let source_response_us = start.elapsed().as_secs_f64() * 1e6 / count as f64;
    // Exercise source movement across delay buckets, including the direct-only
    // updates used between reflection grid points. Prepare geometry outside timing.
    let moves = (0..64)
        .map(|i| {
            let d = geometry::portable_direction(i as f32 * 5.625, 7.);
            let radius = 0.4 + (i % 13) as f64 * 0.17;
            Ok((
                room_geometry::WeightedPosition {
                    position: room_geometry::position(
                        &room.profile,
                        [
                            (-d[1] * radius) as f32,
                            (d[0] * radius) as f32,
                            (d[2] * radius) as f32,
                        ],
                        1.5,
                        [1., 0., 0.],
                    )?,
                    weight: 1.,
                },
                Rotation::new([i as f32 * 0.5, 7., 0.])?,
            ))
        })
        .collect::<Result<Vec<_>>>()?;
    let start = Instant::now();
    for i in 0..count {
        let (source, rotation) = &moves[i % moves.len()];
        work.build_branches(&room, std::slice::from_ref(source), rotation, true, false)?;
    }
    let moving_direct_response_us = start.elapsed().as_secs_f64() * 1e6 / count as f64;
    let start = Instant::now();
    for i in 0..count {
        let (source, rotation) = &moves[i % moves.len()];
        work.build_branches(&room, std::slice::from_ref(source), rotation, true, true)?;
    }
    let moving_full_response_us = start.elapsed().as_secs_f64() * 1e6 / count as f64;
    let mut dynamic = Dynamic::with_policy(2, room.short_taps, quality.fft())?;
    let start = Instant::now();
    for i in 0..count {
        work.impulse[0] = i as f32 * 1e-8;
        dynamic.set_target(&work.impulse[..room.short_taps * 2])?;
    }
    let stereo_target_us = start.elapsed().as_secs_f64() * 1e6 / count as f64;
    let input = [0.001; BLOCK];
    let mut out = [0.; 2 * BLOCK];
    let start = Instant::now();
    for _ in 0..count {
        dynamic.process(&input, &mut out)?;
    }
    let stereo_convolution_us = start.elapsed().as_secs_f64() * 1e6 / count as f64;
    // Use complete pose-dependent responses and the same structural support as
    // Emitter. Advance the convolution clock between targets so the measurement
    // includes slot reuse and pruning, not only replacement at one sample.
    let mut responses = Vec::with_capacity(8);
    for i in 0..8 {
        work.build(&room, position, &Rotation::new([i as f32 * 0.5, 7., 0.])?)?;
        responses.push((
            work.impulse.clone(),
            work.direct_support,
            work.early_support,
        ));
    }
    let mut direct = Dynamic::with_policy(2, room.short_taps, quality.fft())?;
    let mut early = Dynamic::with_policy(2, room.short_taps, quality.fft())?;
    let mut direct_seconds = 0.;
    let mut early_seconds = 0.;
    for i in 0..count + 32 {
        let (impulse, direct_support, early_support) = &responses[i % responses.len()];
        let start = Instant::now();
        direct.set_target_support(&impulse[..2 * room.short_taps], *direct_support)?;
        let direct_elapsed = start.elapsed().as_secs_f64();
        let start = Instant::now();
        early.set_target_support(&impulse[2 * room.short_taps..], *early_support)?;
        let early_elapsed = start.elapsed().as_secs_f64();
        if i >= 32 {
            direct_seconds += direct_elapsed;
            early_seconds += early_elapsed;
        }
        direct.process(&input, &mut out)?;
        early.process(&input, &mut out)?;
    }
    println!(
        "{}",
        serde_json::json!({
            "schema":"mradm.room-stages", "version":2, "iterations":count,
            "quality":quality.name(), "processing_policy":quality.policy(),
            "profiling":cfg!(feature = "room-profile"), "query_fft_length":n,
            "hrir_query_us":hrir_query_us, "source_response_us":source_response_us,
            "moving_direct_response_us":moving_direct_response_us, "moving_full_response_us":moving_full_response_us,
            "stereo_target_us":stereo_target_us, "stereo_convolution_us":stereo_convolution_us,
            "short_taps":room.short_taps,
            "supported_direct_target_us":direct_seconds * 1e6 / count as f64,
            "supported_early_target_us":early_seconds * 1e6 / count as f64,
            "direct_support_taps":responses.iter().map(|v|v.1).collect::<Vec<_>>(),
            "early_support_taps":responses.iter().map(|v|v.2).collect::<Vec<_>>(),
            "note":"Single-source stage means; not realtime capacity or device latency."
        })
    );
    Ok(())
}
