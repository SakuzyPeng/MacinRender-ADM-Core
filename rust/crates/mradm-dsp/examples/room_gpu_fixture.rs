//! Export real ROOM arithmetic inputs for an independent GPU feasibility probe.
//! This is not a Session benchmark: geometry/control are prepared in advance.
use mradm_dsp::{
    Complex32 as C, Result,
    fft::RealFft,
    geometry,
    room::{
        BLOCK,
        geometry::{self as rg, Rotation},
        hrir::{FractionalBank, HrirBank, PHASES},
        prepared::{PreparedRoom, ResponseWork},
        quality::Quality,
    },
};
use std::{fs, io::Write, path::Path, sync::Arc};
#[path = "room_support/assets.rs"]
mod assets;

#[derive(Clone, Copy, Default)]
struct Term {
    row: u32,
    fraction: u32,
    shift: u32,
    alpha: f32,
    weight: f32,
}
#[derive(Clone, Copy, Default)]
struct Job {
    material: u32,
    fraction: u32,
    shift: u32,
    counts: u32,
    alpha: f32,
    gain: f32,
    terms: [Term; 12],
}
impl Job {
    fn write(&self, out: &mut impl Write) -> std::io::Result<()> {
        for x in [self.material, self.fraction, self.shift, self.counts] {
            out.write_all(&x.to_le_bytes())?;
        }
        for x in [self.alpha, self.gain] {
            out.write_all(&x.to_le_bytes())?;
        }
        for t in self.terms {
            for x in [t.row, t.fraction, t.shift] {
                out.write_all(&x.to_le_bytes())?;
            }
            for x in [t.alpha, t.weight] {
                out.write_all(&x.to_le_bytes())?;
            }
        }
        Ok(())
    }
}
fn floats(out: &mut impl Write, values: &[f32]) -> std::io::Result<()> {
    for x in values {
        out.write_all(&x.to_le_bytes())?;
    }
    Ok(())
}
fn complex(out: &mut impl Write, values: &[C]) -> std::io::Result<()> {
    for v in values {
        floats(out, &[v.re, v.im])?;
    }
    Ok(())
}
fn writer(root: &Path, name: &str) -> std::io::Result<std::io::BufWriter<fs::File>> {
    Ok(std::io::BufWriter::new(fs::File::create(root.join(name))?))
}
fn fraction(value: f64) -> (u32, f32) {
    let x = value.clamp(0., 1.) * PHASES as f64;
    let lo = (x as usize).min(PHASES - 1);
    (lo as u32, (x - lo as f64) as f32)
}
fn signal(voice: usize, sample: i64) -> f32 {
    ((sample + voice as i64 * 61).rem_euclid(179) as f32 - 89.) * 0.00001
}
fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("Use Release for exported numerical references".into());
    }
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 4 {
        return Err("usage: room_gpu_fixture ROOM HRIR NEW_DIRECTORY".into());
    }
    let out = Path::new(&args[3]);
    fs::create_dir(out)?;
    let quality = Quality::Realtime;
    let profile = Arc::new(assets::profile(Path::new(&args[1]))?);
    let data = Arc::new(assets::hrir_at_rate(
        Path::new(&args[2]),
        profile.sample_rate,
    )?);
    let layers = data
        .collection
        .as_ref()
        .ok_or("Use the full distance collection")?
        .layers
        .len();
    let (n, taps, _) = PreparedRoom::required_fft(&profile, &data)?;
    let bins = n / 2 + 1;
    let bank = Arc::new(HrirBank::with_policy(data.clone(), n, quality.fft())?);
    let room = PreparedRoom::with_quality(profile, bank.clone(), quality)?;
    let voices = 32;
    let buckets = taps.div_ceil(room.path_bucket_stride);
    let ranges_count = voices * 2 * buckets;
    let mut fft = RealFft::with_policy(n, quality.fft())?;
    let mut time = vec![0.; n];
    let mut spectrum = vec![C::default(); bins];
    {
        let mut file = writer(out, "bank.bin")?;
        for layer in 0..layers {
            let hrir = data.layer(layer);
            for ir in hrir.impulses.chunks_exact(hrir.taps) {
                time.fill(0.);
                time[..ir.len()].copy_from_slice(ir);
                fft.forward(&time, &mut spectrum)?;
                complex(&mut file, &spectrum)?;
            }
        }
    }
    for (name, table) in [
        (
            "fraction.bin",
            FractionalBank::normalized_with_policy(n, quality.fft())?,
        ),
        (
            "propagation.bin",
            FractionalBank::from_impulses_with_policy(
                n,
                &room.profile.propagation_phases,
                quality.fft(),
            )?,
        ),
    ] {
        let mut file = writer(out, name)?;
        for phase in 0..=PHASES {
            let (a, b, _) = table.rows(phase as f64 / PHASES as f64);
            complex(&mut file, if phase == PHASES { b } else { a })?;
        }
    }
    {
        let mut file = writer(out, "phase.bin")?;
        complex(
            &mut file,
            &(0..n).map(|i| bank.integer_phase(1, i)).collect::<Vec<_>>(),
        )?;
        let mut file = writer(out, "materials.bin")?;
        for image in &room.images {
            let mut gains = [1f32; 8];
            for (wall, count) in image.walls.iter().enumerate() {
                for (band, gain) in gains.iter_mut().enumerate() {
                    let reflection = (1. - room.profile.absorption[wall][band]).sqrt() as f32;
                    for _ in 0..*count {
                        *gain *= reflection;
                    }
                }
            }
            time.fill(0.);
            for (t, value) in time[..room.profile.band_taps].iter_mut().enumerate() {
                let mut sum = 0.;
                for (band, gain) in gains.iter().enumerate() {
                    sum += room.profile.bands[band * room.profile.band_taps + t] * f64::from(*gain);
                }
                *value = sum as f32;
            }
            fft.forward(&time, &mut spectrum)?;
            complex(&mut file, &spectrum)?;
        }
    }
    let mut frames = Vec::new();
    let mut references = writer(out, "reference.bin")?;
    let mut first = Vec::new();
    let mut descriptions = Vec::new();
    let mut response = ResponseWork::new(&room)?;
    for scenario in ["ramps", "distance-ramps"] {
        for tick in [0usize, 7, 19, 31, 47, 63, 89, 127] {
            let mut groups = vec![Vec::new(); ranges_count];
            let rotation =
                Rotation::new([tick as f32 * 0.25, tick as f32 * 0.05, tick as f32 * 0.02])?;
            for voice in 0..voices {
                let direction = if scenario == "ramps" {
                    geometry::portable_direction(
                        voice as f32 * 360. / voices as f32 + tick as f32 * 0.5,
                        (voice % 3) as f32 * 15. - 15.,
                    )
                } else {
                    geometry::portable_direction(-60. + 120. * voice as f32 / voices as f32, 0.)
                };
                let radius = if scenario == "ramps" {
                    1.4
                } else {
                    let phase = ((tick + voice * 11) % 128) as f64 / 64.;
                    (0.2 + 1.4 * (1. - (phase - 1.).abs())) / 1.5
                };
                let scene = [
                    (-direction[1] * radius) as f32,
                    (direction[0] * radius) as f32,
                    (direction[2] * radius) as f32,
                ];
                let position = rg::position(&room.profile, scene, 1.5, [1., 0., 0.])?;
                response.build(&room, position, &rotation)?;
                floats(&mut references, &response.impulse)?;
                if frames.is_empty() {
                    first.extend_from_slice(&response.impulse);
                }
                for (image_index, image) in room.images.iter().enumerate() {
                    let path = rg::path(&room.profile, position, *image);
                    if path.gain == 0. {
                        continue;
                    }
                    let absolute =
                        libm::floor(path.samples) as usize + room.profile.alignment_guard;
                    let (pf, pa) =
                        fraction(f64::from((path.samples - libm::floor(path.samples)) as f32));
                    let mut job = Job {
                        material: image_index as u32,
                        fraction: pf,
                        shift: (absolute % room.path_bucket_stride) as u32,
                        alpha: pa,
                        gain: path.gain as f32,
                        ..Job::default()
                    };
                    let mut face = usize::MAX;
                    let (vertices, weights) =
                        bank.weights(rotation.apply(path.direction), &mut face)?;
                    let radius = if image.order == 0 {
                        position.distance
                    } else {
                        path.distance
                    };
                    let (low, high, alpha) = data.collection.as_ref().unwrap().bracket(radius);
                    let selected = [
                        (low, 1. - alpha),
                        (high, if low == high { 0. } else { alpha }),
                    ];
                    for ear in 0..2 {
                        let mut delay = 0.;
                        for (layer, radial) in selected {
                            if radial == 0. {
                                continue;
                            }
                            for v in 0..3 {
                                delay += data.layer(layer).delays[vertices[v]][ear]
                                    * (weights[v] * radial);
                            }
                        }
                        let mut count = 0;
                        for (layer, radial) in selected {
                            if radial == 0. {
                                continue;
                            }
                            for v in 0..3 {
                                let weight = weights[v] * radial;
                                if weight <= 0. {
                                    continue;
                                }
                                let shift = delay - data.layer(layer).delays[vertices[v]][ear];
                                let integer = libm::floor(shift);
                                let (f, a) = fraction(shift - integer);
                                let start = data.guard as i64 + integer as i64 - 40;
                                job.terms[ear * 6 + count] = Term {
                                    row: ((layer * data.directions.len() + vertices[v]) * 2 + ear)
                                        as u32,
                                    fraction: f,
                                    shift: start.rem_euclid(n as i64) as u32,
                                    alpha: a,
                                    weight: weight as f32,
                                };
                                count += 1;
                            }
                        }
                        job.counts |= (count as u32) << (ear * 8);
                    }
                    let branch = usize::from(image.order != 0);
                    groups[(voice * 2 + branch) * buckets + absolute / room.path_bucket_stride]
                        .push(job);
                }
            }
            let mut jobs = Vec::new();
            let mut ranges = Vec::new();
            for group in groups {
                ranges.push([jobs.len() as u32, group.len() as u32]);
                jobs.extend(group);
            }
            descriptions
                .push(serde_json::json!({"scenario":scenario,"tick":tick,"paths":jobs.len()}));
            frames.push((jobs, ranges));
        }
    }
    let max_jobs = frames.iter().map(|(jobs, _)| jobs.len()).max().unwrap();
    {
        let mut jobs_file = writer(out, "jobs.bin")?;
        let mut ranges_file = writer(out, "ranges.bin")?;
        for (jobs, ranges) in &mut frames {
            jobs.resize(max_jobs, Job::default());
            for job in jobs {
                job.write(&mut jobs_file)?;
            }
            for range in ranges {
                for x in range {
                    ranges_file.write_all(&x.to_le_bytes())?;
                }
            }
        }
    }
    export_history(out, voices, taps, &first, quality)?;
    fs::write(
        out.join("config.txt"),
        format!(
            "1 {} {voices} {n} {taps} {} {buckets} {max_jobs} {} {} {}\n",
            room.profile.sample_rate,
            room.path_bucket_stride,
            frames.len(),
            layers * data.directions.len() * 2,
            room.images.len()
        ),
    )?;
    fs::write(
        out.join("manifest.json"),
        serde_json::to_vec_pretty(&serde_json::json!({
            "schema":"mradm.room-gpu-fixture","version":1,"sample_rate":room.profile.sample_rate,"sources":voices,"fft_length":n,"short_taps":taps,"bucket_stride":room.path_bucket_stride,"buckets":buckets,"distance_layers":layers,"directions":data.directions.len(),"images":room.images.len(),"maximum_active_paths":max_jobs,"cases":descriptions,"room_hash":room.profile.identity,"hrir_hash":data.identity,
            "scope":"Frozen geometry snapshots, all active direct/early image paths and all distance layers. CPU reference is production ResponseWork. Input-history and convolution checks use deterministic synthetic audio; not the Session event schedule, fade implementation, late field or complete tail acceptance."
        }))?,
    )?;
    println!(
        "Exported {} snapshots, {max_jobs} maximum active paths, {bins} bins",
        frames.len()
    );
    Ok(())
}

fn export_history(
    out: &Path,
    voices: usize,
    taps: usize,
    impulses: &[f32],
    quality: Quality,
) -> Result<()> {
    let io = |_| mradm_dsp::Error::Io("Cannot write GPU fixture");
    let parts = taps.div_ceil(BLOCK);
    let bins = BLOCK + 1;
    let mut fft = RealFft::with_policy(BLOCK * 2, quality.fft())?;
    let mut time = vec![0.; BLOCK * 2];
    let mut ring_file = writer(out, "history.bin").map_err(io)?;
    let mut prev_file = writer(out, "previous.bin").map_err(io)?;
    let mut audio = vec![0f32; 1024];
    for voice in 0..voices {
        let mut history = vec![C::default(); (parts + 2) * bins];
        for block in 0..parts + 2 {
            for (i, value) in time.iter_mut().enumerate() {
                *value = signal(
                    voice,
                    (block as i64 - parts as i64 - 1) * BLOCK as i64 + i as i64,
                );
            }
            fft.forward(&time, &mut history[block * bins..(block + 1) * bins])?;
        }
        complex(&mut ring_file, &history[..parts * bins]).map_err(io)?;
        floats(
            &mut prev_file,
            &(0..BLOCK)
                .map(|i| signal(voice, i as i64 - BLOCK as i64))
                .collect::<Vec<_>>(),
        )
        .map_err(io)?;
        for channel in 0..4 {
            let ir = &impulses[(voice * 4 + channel) * taps..(voice * 4 + channel + 1) * taps];
            let mut filters = vec![C::default(); parts * bins];
            for (part, data) in ir.chunks(BLOCK).enumerate() {
                time.fill(0.);
                time[..data.len()].copy_from_slice(data);
                fft.forward(&time, &mut filters[part * bins..(part + 1) * bins])?;
            }
            for step in 0..2 {
                let mut sum = vec![C::default(); bins];
                for part in 0..parts {
                    for bin in 0..bins {
                        sum[bin] += history[(parts + step - part) * bins + bin]
                            * filters[part * bins + bin];
                    }
                }
                fft.inverse(&sum, &mut time)?;
                for i in 0..BLOCK {
                    audio[(channel % 2) * 512 + step * BLOCK + i] += time[BLOCK + i];
                }
            }
        }
    }
    floats(&mut writer(out, "audio-reference.bin").map_err(io)?, &audio).map_err(io)?;
    Ok(())
}
