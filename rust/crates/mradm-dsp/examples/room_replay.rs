//! Release room-scoped numerical checkpoints. Resource bytes and event times
//! are fixed; wall clocks and devices are deliberately absent from the input.
use mradm_dsp::{
    Result, geometry,
    room::{
        control::{RoomCommand, RoomDescription, State},
        geometry::{self as room_geometry, Rotation},
        hrir::HrirBank,
        prepared::{PreparedRoom, ResponseWork},
        quality::Quality,
        resources::sha256,
        screen::{RoomScreenGeometry, RoomScreens},
        session::{Instance, PreparedSet, Session},
    },
};
use std::{fs, io::Write, path::Path, sync::Arc};
#[path = "room_support/assets.rs"]
mod assets;

fn save(root: &Path, name: &str, samples: &[f32]) -> std::io::Result<serde_json::Value> {
    let bytes: Vec<_> = samples.iter().flat_map(|v| v.to_le_bytes()).collect();
    let mut out = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(root.join(name))?;
    out.write_all(&bytes)?;
    Ok(serde_json::json!({"sha256":sha256(&bytes),"samples":samples.len()}))
}
fn render(
    prepared: Arc<PreparedSet>,
    next: Arc<PreparedSet>,
    chunks: &[usize],
    screens: bool,
    dense: bool,
    head_motion: &str,
) -> Result<Vec<f32>> {
    const MATERIAL: usize = 96013;
    let mut session = Session::new(prepared.clone(), 1.5, true, false)?;
    let descriptions: Vec<_> = (0..32)
        .map(|i| {
            let v = geometry::portable_direction(i as f32 * 11.25, (i % 3) as f32 * 20. - 20.);
            RoomDescription {
                id: i as u64 + 1,
                role: if i >= 30 { 2 } else { 0 },
                screens: RoomScreens {
                    enabled: u32::from(screens),
                    reproduction: RoomScreenGeometry {
                        width: 90.,
                        ..RoomScreenGeometry::default()
                    },
                    ..RoomScreens::default()
                },
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
    let initial: Vec<_> = descriptions
        .iter()
        .enumerate()
        .map(|(i, d)| RoomCommand {
            element: i as u32,
            state: State {
                position: d.position,
                head_locked: u32::from(i == 0),
                screen_reference: u32::from(screens && i < 30),
                diffuse: if i == 4 { 0.7 } else { 0. },
                ..State::default()
            },
            ..RoomCommand::default()
        })
        .collect();
    let mut moved = initial[1].state;
    moved.position = [-2., 0.4, -0.2];
    let mut quiet = initial[3].state;
    quiet.gain = 0.23;
    let mut relock = initial[2].state;
    relock.head_locked = 1;
    let mut events = vec![
        (
            731usize,
            RoomCommand {
                element: 1,
                changed: 4,
                duration: 1537,
                state: moved,
                ..RoomCommand::default()
            },
        ),
        (
            1789,
            RoomCommand {
                element: 3,
                changed: 2,
                duration: 913,
                state: quiet,
                ..RoomCommand::default()
            },
        ),
        (
            2901,
            RoomCommand {
                element: 2,
                changed: 256,
                state: relock,
                ..RoomCommand::default()
            },
        ),
    ];
    if dense {
        // Dense replay: 30 non-LFE sources each ramp at a distinct sample offset.
        // Keep the original head-lock, gain, pose, room-switch, removal and EOS schedule.
        for block in 0..MATERIAL.div_ceil(512) {
            for (source, command) in initial.iter().enumerate().take(30) {
                let sample = block * 512 + (source * 131 + 17) % 512;
                if sample >= MATERIAL {
                    continue;
                }
                let direction = geometry::portable_direction(
                    source as f32 * 11.25 + (block + 1) as f32 * 0.5,
                    (source % 3) as f32 * 20. - 20.,
                );
                events.push((
                    sample,
                    RoomCommand {
                        element: source as u32,
                        changed: 4,
                        duration: 512,
                        state: State {
                            position: [
                                (-direction[1] * 1.4) as f32,
                                (direction[0] * 1.4) as f32,
                                (direction[2] * 1.4) as f32,
                            ],
                            ..command.state
                        },
                        ..RoomCommand::default()
                    },
                ));
            }
        }
        events.sort_by_key(|event| event.0);
    }
    let mut candidate = Some(Box::new(Instance::new(next, 32)?));
    let mut input = vec![[0.; 4096]; 32];
    let mut block_output = vec![0.; 8192];
    let mut commands = Vec::with_capacity(events.len());
    let mut output = Vec::new();
    let mut position = 0usize;
    let mut sequence = 0;
    let mut end = None;
    let mut sources = 32;
    loop {
        if position == MATERIAL && end.is_none() {
            session.end_input();
            end = Some(MATERIAL + session.info().tail_frames as usize);
        }
        if end == Some(position) {
            break;
        }
        if position == 32011 {
            sources = 31;
            session.configure(&descriptions[..sources])?;
        }
        if position == 7680 {
            session.queue_room(candidate.take().unwrap(), 1)?;
        }
        if position.is_multiple_of(512) {
            let step = (position / 512) as f32;
            session.set_pose(match head_motion {
                "slow" => [step * 0.25 - 35., step * 0.05 - 10., step * 0.02],
                "fast" => [step * 4. - 170., step * 1., step * 0.3],
                "wrap" => [
                    ((179.85 + step * 0.1 + 180.) % 360.) - 180.,
                    89.8,
                    -30. + step * 0.04,
                ],
                _ => [step * 0.61 - 35., step * 0.12 - 10., step * 0.07],
            })?;
        }
        let boundary = [
            MATERIAL,
            32011,
            7680,
            end.unwrap_or(MATERIAL),
            (position / 512 + 1) * 512,
        ]
        .into_iter()
        .filter(|v| *v > position)
        .min()
        .unwrap();
        let count = chunks[sequence % chunks.len()]
            .min(4096)
            .min(boundary - position);
        sequence += 1;
        for (source, plane) in input.iter_mut().enumerate() {
            for (i, value) in plane[..count].iter_mut().enumerate() {
                let word = ((position + i) * 3943 + source * 7817) % 65521;
                *value = if position < MATERIAL {
                    (word as f32 - 32760.) / 16777216.
                } else {
                    0.
                };
            }
        }
        commands.clear();
        for (sample, command) in events.iter().copied() {
            if sample >= position && sample < position + count {
                let mut c = command;
                c.offset = (sample - position) as u32;
                commands.push(c);
            }
        }
        session.process(
            count,
            input[..sources].iter().map(|p| Some(&p[..count])),
            if position == 0 { &initial } else { &[] },
            &commands,
            &mut block_output[..count * 2],
        )?;
        output.extend_from_slice(&block_output[..count * 2]);
        position += count;
        let _retired = session.take_retired();
    }
    Ok(output)
}
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Use Release for numerical acceptance".into());
    }
    let mut args: Vec<_> = std::env::args().collect();
    let hrir_path = if let Some(index) = args.iter().position(|v| v == "--hrir") {
        let path = args
            .get(index + 1)
            .ok_or("--hrir needs a resource path")?
            .clone();
        args.drain(index..index + 2);
        Some(std::path::PathBuf::from(path))
    } else {
        None
    };
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
    let head_motion = if let Some(index) = args.iter().position(|v| v == "--head-motion") {
        let value = args
            .get(index + 1)
            .ok_or("--head-motion needs standard, slow, fast or wrap")?
            .clone();
        if !["standard", "slow", "fast", "wrap"].contains(&value.as_str()) {
            return Err("--head-motion needs standard, slow, fast or wrap".into());
        }
        args.drain(index..index + 2);
        value
    } else {
        "standard".to_owned()
    };
    let screens = args.iter().any(|v| v == "--screens");
    let dense = args.iter().any(|v| v == "--dense");
    args.retain(|v| v != "--screens" && v != "--dense");
    if args.len() != 3 {
        return Err("usage: room_replay ROOM_JSON NEW_OUTPUT_DIRECTORY [--hrir SOFA_OR_COLLECTION] [--screens] [--dense] [--quality exact|realtime] [--head-motion standard|slow|fast|wrap]".into());
    }
    let file = Path::new(&args[1]);
    let root = file.parent().ok_or("missing parent")?;
    let out = Path::new(&args[2]);
    fs::create_dir(out)?;
    let profile = Arc::new(assets::profile(file)?);
    let selected = hrir_path.unwrap_or_else(|| root.join(&profile.default_hrir));
    let hrir = Arc::new(assets::hrir_at_rate(&selected, profile.sample_rate)?);
    let (length, _, _) = PreparedRoom::required_fft(&profile, &hrir)?;
    let bank = Arc::new(HrirBank::with_policy(hrir, length, quality.fft())?);
    let room = Arc::new(PreparedRoom::with_quality(
        profile.clone(),
        bank.clone(),
        quality,
    )?);
    let prepared = Arc::new(PreparedSet::new(room.clone())?);
    let mut changed = (*profile).clone();
    changed.identity.push_str("-replay");
    changed.absorption = [[0.90; 8]; 6];
    changed.late_energy *= 0.8;
    changed.dimensions[0] += 0.5;
    changed.listener[0] += 0.25;
    changed.dimensions[1] += 0.2;
    changed.listener[1] += 0.1;
    let next = Arc::new(PreparedSet::new(Arc::new(PreparedRoom::with_quality(
        Arc::new(changed),
        bank,
        quality,
    )?))?);
    let mut work = ResponseWork::new(&room)?;
    let mut query = vec![0.; room.hrir.data.output_taps * 2];
    let mut manifest = serde_json::json!({"schema":"mradm.room-replay","version":1,
        "scenario":"controls-distance-geometry-switch-v1",
        "room_hash":profile.identity,"hrir_hash":room.hrir.data.identity,"prepared":room.fingerprint(),
        "lfe":prepared.lfe.fingerprint(),"latency_frames":256,"tail_frames":prepared.tail_frames(),"files":{}});
    if quality != Quality::Exact {
        manifest["processing_policy"] = quality.policy().into();
    }
    for (index, [az, el, r]) in [
        [0., 0., 0.75],
        [0., 0., 1.5],
        [0., 0., 3.],
        [179.999, 0., 2.1],
        [-179.999, 0., 2.1],
        [0., 89.999, 1.85],
        [0., -89.999, 1.05],
        [91.3, -31.1, 2.1],
    ]
    .into_iter()
    .enumerate()
    {
        let direction = geometry::portable_direction(az as f32, el as f32);
        let mut face = usize::MAX;
        work.hrir(&room, direction, &mut face, &mut query)?;
        let name = format!("hrir-{index}.f32");
        manifest["files"][&name] = save(out, &name, &query)?;
        let position = room_geometry::position(
            &profile,
            [
                (-direction[1] * r) as f32,
                (direction[0] * r) as f32,
                (direction[2] * r) as f32,
            ],
            1.,
            [1., 0., 0.],
        )?;
        work.build(&room, position, &Rotation::new([13., -7., 11.])?)?;
        let name = format!("branches-{index}.f32");
        manifest["files"][&name] = save(out, &name, &work.impulse)?;
    }
    if screens {
        manifest["scenario"] = "controls-screen-distance-geometry-switch-v1".into();
    }
    if dense {
        manifest["dense_source_ramps"] = true.into();
    }
    if head_motion != "standard" {
        manifest["head_motion"] = head_motion.clone().into();
    }
    let canonical = render(
        prepared.clone(),
        next.clone(),
        &[256],
        screens,
        dense,
        &head_motion,
    )?;
    let fragmented = render(
        prepared,
        next,
        &[1, 173, 733, 4096, 19, 257],
        screens,
        dense,
        &head_motion,
    )?;
    if canonical
        .iter()
        .map(|v| v.to_bits())
        .ne(fragmented.iter().map(|v| v.to_bits()))
    {
        return Err("Fragmented replay differs from canonical PCM".into());
    }
    manifest["files"]["session.f32"] = save(out, "session.f32", &canonical)?;
    manifest["frames"] = (canonical.len() / 2).into();
    fs::write(
        out.join("manifest.json"),
        serde_json::to_vec_pretty(&manifest)?,
    )?;
    println!(
        "{}",
        serde_json::json!({"output":out,"frames":canonical.len()/2,"fragmented_identical":true,
        "pcm_sha256":manifest["files"]["session.f32"]["sha256"],"os":std::env::consts::OS,"arch":std::env::consts::ARCH})
    );
    Ok(())
}
