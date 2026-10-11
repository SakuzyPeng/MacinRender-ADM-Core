//! Release-only all-direction room measurement using an ideal omni receiver.
//! Three exported channels are direct, early and late; no personal HRIR is used.
use mradm_dsp::{
    Error, Result,
    room::{
        BLOCK, geometry,
        hrir::HrirBank,
        prepared::{PreparedRoom, ResponseWork},
        resources::{HrirData, RoomProfile, sha256},
        signal::{Emitter, WorldTail},
    },
};
use std::{fs, io::Write, path::Path, sync::Arc};

fn asset(root: &Path, name: &str, size: usize) -> Result<Vec<u8>> {
    let value = fs::read(root.join(name)).map_err(|_| Error::Io("Missing room component"))?;
    if value.len() != size {
        return Err(Error::Io("Room component size mismatch"));
    }
    Ok(value)
}
fn omni() -> HrirData {
    HrirData {
        identity: "ideal-omni-unit-impulse-v1".into(),
        source_sofa_sha256: None,
        impulses: vec![1.; 12],
        taps: 1,
        directions: vec![
            [1., 0., 0.],
            [-1., 0., 0.],
            [0., 1., 0.],
            [0., -1., 0.],
            [0., 0., 1.],
            [0., 0., -1.],
        ],
        faces: vec![
            [0, 2, 4],
            [2, 1, 4],
            [1, 3, 4],
            [3, 0, 4],
            [2, 0, 5],
            [1, 2, 5],
            [3, 1, 5],
            [0, 3, 5],
        ],
        delays: vec![[0.; 2]; 6],
        guard: 40,
        preprocessing_guard: 0,
        output_taps: 81,
    }
}
fn magnitude(impulse: &[f32], hz: f64) -> f64 {
    let mut re = 0.;
    let mut im = 0.;
    for (n, h) in impulse.iter().enumerate() {
        let (s, c) = libm::sincos(std::f64::consts::TAU * hz * n as f64 / 48000.);
        re += f64::from(*h) * c;
        im -= f64::from(*h) * s;
    }
    libm::sqrt(re * re + im * im)
}
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Use Release for acoustic measurements".into());
    }
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 6 {
        return Err("usage: room_omni ROOM_JSON AZIMUTH ELEVATION DISTANCE OUTPUT_F32".into());
    }
    let file = Path::new(&args[1]);
    let root = file.parent().ok_or("Missing room directory")?;
    let manifest = fs::read(file)?;
    let profile = Arc::new(RoomProfile::parse(&manifest, |n, s| asset(root, n, s))?);
    let receiver = Arc::new(omni());
    let (fft, _, _) = PreparedRoom::required_fft(&profile, &receiver)?;
    let bank = Arc::new(HrirBank::new(receiver, fft)?);
    let room = Arc::new(PreparedRoom::new(profile.clone(), bank)?);
    let direction = mradm_dsp::geometry::portable_direction(args[2].parse()?, args[3].parse()?);
    let distance: f64 = args[4].parse()?;
    if !distance.is_finite() || distance <= 0. {
        return Err("Invalid measurement distance".into());
    }
    let position = geometry::position(
        &profile,
        [
            (-direction[1] * distance) as f32,
            (direction[0] * distance) as f32,
            (direction[2] * distance) as f32,
        ],
        1.,
        [1., 0., 0.],
    )?;
    let mut emitter = Emitter::new(room.clone(), position)?;
    let mut work = ResponseWork::new(&room)?;
    emitter.update(position, [0.; 3], &mut work)?;
    let mut field = WorldTail::new(room.clone())?;
    field.set_parallel(false);
    field.update([0.; 3])?;
    let frames = room.history_frames() + 1;
    let mut result = vec![0f32; frames * 3];
    let mut input = [0.; BLOCK];
    let mut direct = [0.; BLOCK * 2];
    let mut early = direct;
    let mut late = direct;
    let mut send = input;
    input[0] = 1.;
    for start in (0..frames).step_by(BLOCK) {
        emitter.process(&input, &[1.; BLOCK], &mut direct, &mut early, &mut send)?;
        field.process(&send, &mut late)?;
        for i in 0..BLOCK.min(frames - start) {
            // The two omni channels must be identical, regardless of direction.
            if direct[i].to_bits() != direct[BLOCK + i].to_bits()
                || early[i].to_bits() != early[BLOCK + i].to_bits()
                || late[i].to_bits() != late[BLOCK + i].to_bits()
            {
                return Err("Omni receiver channels differ".into());
            }
            result[(start + i) * 3..(start + i) * 3 + 3]
                .copy_from_slice(&[direct[i], early[i], late[i]]);
        }
        input.fill(0.);
    }
    let mut reflections = Vec::new();
    let mut direct_magnitude = [0.; 4];
    let mut direct_analysis = serde_json::Value::Null;
    let direct_time = position.distance / profile.speed;
    for image in &room.images {
        let path = geometry::path(&profile, position, *image);
        if path.gain == 0. {
            continue;
        }
        let mut gains = [1f32; 8];
        for (wall, count) in image.walls.iter().enumerate() {
            for (band, gain) in gains.iter_mut().enumerate() {
                for _ in 0..*count {
                    *gain *= (1. - profile.absorption[wall][band]).sqrt() as f32;
                }
            }
        }
        let material: Vec<_> = (0..profile.band_taps)
            .map(|n| {
                let mut value = 0.;
                for (band, gain) in gains.iter().enumerate() {
                    value += profile.bands[band * profile.band_taps + n] * f64::from(*gain);
                }
                value as f32
            })
            .collect();
        let integer = libm::floor(path.samples);
        let mut propagation = [0.; 81];
        room.propagation_kernel(
            f64::from((path.samples - integer) as f32),
            false,
            &mut propagation,
        );
        let response = [1000., 2000., 4000., 8000.]
            .map(|hz| magnitude(&material, hz) * magnitude(&propagation, hz) * path.gain);
        if image.order == 0 {
            direct_magnitude = response;
            direct_analysis = serde_json::json!({"gain":path.gain as f32,"fraction":(path.samples-integer) as f32,"band_gains":gains});
        } else if path.samples / 48000. - direct_time <= 0.015 {
            reflections.push(serde_json::json!({"order":image.order,"walls":image.walls,
                "delay_after_direct_ms":(path.samples/48000.-direct_time)*1000.,
                "gain":path.gain as f32,"fraction":(path.samples-integer) as f32,"band_gains":gains,
                "level_db_1_2_4_8khz":std::array::from_fn::<_,4,_>(|i|20.*libm::log10((response[i]/direct_magnitude[i]).max(1e-30)))}));
        }
    }
    let mut out = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&args[5])?;
    for value in result {
        out.write_all(&value.to_le_bytes())?;
    }
    println!(
        "{}",
        serde_json::json!({"schema":"mradm.room-omni-response","version":1,
        "sample_rate":48000,"frames":frames,"channels":["direct","early","late"],
        "room_id":profile.id,"room_hash":profile.identity,"manifest_sha256":sha256(&manifest),
        "position":position.point,"distance_m":position.distance,"constrained":position.constrained,
        "receiver":"ideal omni unit impulse; normalized interpolation kernel with 40-sample causal guard",
        "receiver_guard_frames":40,"material_guard_frames":profile.band_offset,"alignment_guard_frames":profile.alignment_guard,
        "propagation_guard_frames":40,"direct_analysis":direct_analysis,"early_reflections":reflections})
    );
    Ok(())
}
