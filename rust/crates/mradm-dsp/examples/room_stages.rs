//! Release-only numerical stage profiler, with no device and no resource I/O in timings.
use mradm_dsp::{
    Error, Result, geometry,
    room::{
        BLOCK, geometry as room_geometry,
        geometry::Rotation,
        hrir::HrirBank,
        partition::Dynamic,
        prepared::{PreparedRoom, ResponseWork},
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
    let args: Vec<_> = std::env::args().collect();
    if args.len() < 2 {
        return Err("usage: room_stages ROOM_JSON [ITERATIONS=2000]".into());
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
    let bank = Arc::new(HrirBank::new(data, n)?);
    let room = Arc::new(PreparedRoom::new(profile, bank)?);
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
    let mut dynamic = Dynamic::new(2, room.short_taps)?;
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
    println!(
        "{}",
        serde_json::json!({
            "schema":"mradm.room-stages", "version":1, "iterations":count,
            "profiling":cfg!(feature = "room-profile"), "query_fft_length":n,
            "hrir_query_us":hrir_query_us, "source_response_us":source_response_us,
            "stereo_target_us":stereo_target_us, "stereo_convolution_us":stereo_convolution_us,
            "note":"Single-source stage means; not realtime capacity or device latency."
        })
    );
    Ok(())
}
