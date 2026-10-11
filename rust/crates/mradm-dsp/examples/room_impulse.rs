//! Release-only numerical BRIR export for comparing the frozen model.
use mradm_dsp::{
    Error, Result,
    room::{
        geometry,
        hrir::HrirBank,
        prepared::PreparedRoom,
        resources::{HrirData, RoomProfile},
        signal,
    },
};
use std::{fs, io::Write, path::Path, sync::Arc};

fn asset(root: &Path, name: &str, expected: usize) -> Result<Vec<u8>> {
    let path = root.join(name);
    if fs::metadata(&path)
        .map_err(|_| Error::Io("Missing room asset"))?
        .len()
        != expected as u64
    {
        return Err(Error::Io("Room asset length mismatch"));
    }
    fs::read(path).map_err(|_| Error::Io("Cannot read room asset"))
}

fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Use a Release build for room waveform comparisons".into());
    }
    let arguments: Vec<_> = std::env::args().collect();
    if arguments.len() != 6 {
        return Err("usage: room_impulse ROOM_JSON AZIMUTH ELEVATION DISTANCE OUTPUT_F32".into());
    }
    let file = Path::new(&arguments[1]);
    let root = file.parent().ok_or("missing parent")?;
    let profile = Arc::new(RoomProfile::parse(&fs::read(file)?, |n, s| {
        asset(root, n, s)
    })?);
    let hrir = Arc::new(HrirData::parse(
        &fs::read(root.join(&profile.default_hrir))?,
        |n, s| asset(root, n, s),
    )?);
    let (fft, _, _) = PreparedRoom::required_fft(&profile, &hrir)?;
    let prepare_start = std::time::Instant::now();
    let bank = Arc::new(HrirBank::new(hrir, fft)?);
    let bank_bytes = bank.storage_bytes();
    let room = Arc::new(PreparedRoom::new(profile, bank)?);
    let direction =
        mradm_dsp::geometry::portable_direction(arguments[2].parse()?, arguments[3].parse()?);
    let distance: f64 = arguments[4].parse()?;
    let scene = [
        (-direction[1] * distance) as f32,
        (direction[0] * distance) as f32,
        (direction[2] * distance) as f32,
    ];
    let position = geometry::position(&room.profile, scene, 1., [1., 0., 0.])?;
    let prepare_seconds = prepare_start.elapsed().as_secs_f64();
    let start = std::time::Instant::now();
    let material_guard = room.profile.band_offset;
    let output = signal::impulse(room, position, [0.; 3])?;
    let render_seconds = start.elapsed().as_secs_f64();
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&arguments[5])?;
    for sample in &output {
        file.write_all(&sample.to_le_bytes())?;
    }
    println!(
        "{}",
        serde_json::json!({"frames": output.len()/2, "position": position.point,
        "distance_m":position.distance, "prepare_seconds":prepare_seconds,
        "render_seconds":render_seconds, "hrir_bank_bytes":bank_bytes,"material_filter_guard_frames":material_guard})
    );
    Ok(())
}
