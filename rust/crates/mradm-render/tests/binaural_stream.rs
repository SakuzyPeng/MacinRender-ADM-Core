//! The binaural file stream (PCM consistency 4.7 C): offline equality for any pull size, seeks on
//! the offline window's pre-roll grid (including filters longer than a block), abandoned seek
//! steps, the 100 ms topology coalescing, the pending topology a seek adopts, per-label gain keys,
//! head tracking and the open rejections.
mod common;

use common::{ObjectSpec, RATE, Temp, TrackSpec, direct_block, input, object_block, scene, sine};
use mradm_dsp::dataset::Dataset;
use mradm_render::binaural::Spread;
use mradm_render::binaural_hrtf;
use mradm_render::binaural_render::{self, Offline, Prepared, Settings};
use mradm_render::binaural_stream::{FileStream, Settings as StreamSettings};
use mradm_render::offline::{Location, Window};
use mradm_render::routing::{LiveOverride, Override};
use mradm_scene::RenderScene;
use mradm_wav::{Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::sync::Arc;

fn kemar_padded(taps: usize) -> Dataset {
    let mut d = Dataset::kemar();
    let impulses = d
        .impulses
        .chunks(d.ir_len)
        .flat_map(|ir| {
            ir.iter()
                .copied()
                .chain(std::iter::repeat_n(0., taps - ir.len()))
        })
        .collect();
    d.impulses = impulses;
    d.ir_len = taps;
    d
}

fn prepare(scene: &RenderScene, dataset: Dataset, spread: Spread, smoothing: u32) -> Arc<Prepared> {
    let block = u64::from(smoothing).max(1024);
    let hrtf = Arc::new(binaural_hrtf::build(dataset, block, false).unwrap());
    Arc::new(binaural_render::prepare(scene, hrtf, spread, smoothing, &mut |_, _| {}).unwrap())
}

fn offline(
    p: &Arc<Prepared>,
    scene: &RenderScene,
    file: &str,
    smoothing: u32,
    window: Option<Window>,
    pose: Option<[f32; 3]>,
) -> Vec<f32> {
    let mut driver = Offline::new(
        Arc::clone(p),
        Offline::open_reader(file).unwrap(),
        Settings {
            inputs: usize::from(scene.info.channels),
            frames: scene.info.frames,
            sample_rate: scene.info.sample_rate,
            window,
            smoothing_frames: smoothing,
            pose,
        },
    )
    .unwrap();
    let mut out = Vec::new();
    while let Some(step) = driver.next_step().unwrap() {
        out.extend_from_slice(driver.output(step.emit));
    }
    out
}

fn open(p: &Arc<Prepared>, scene: &RenderScene, file: &str, smoothing: u32) -> FileStream {
    FileStream::open(
        Arc::clone(p),
        file,
        StreamSettings {
            inputs: usize::from(scene.info.channels),
            frames: scene.info.frames,
            sample_rate: scene.info.sample_rate,
            smoothing_frames: smoothing,
            spread: p.spread,
        },
    )
    .unwrap()
}

/// Pulls `frames` frames (fewer at the end) cycling through `sizes`.
fn pull(s: &mut FileStream, sizes: &[usize], frames: usize) -> Vec<f32> {
    let mut out = Vec::new();
    let mut buffer = vec![0.; 2 * sizes.iter().max().unwrap()];
    for &size in sizes.iter().cycle() {
        let want = size.min(frames - out.len() / 2);
        if want == 0 {
            break;
        }
        let got = s.process(&mut buffer, want).unwrap();
        out.extend_from_slice(&buffer[..2 * got]);
        if got < want {
            break;
        }
    }
    out
}

fn seek(s: &mut FileStream, frame: u64) {
    let mut pending = s.seek_begin(frame).unwrap();
    while pending {
        pending = s.seek_step().unwrap();
    }
}

fn words(v: &[f32]) -> Vec<u32> {
    v.iter().map(|x| x.to_bits()).collect()
}

fn edit<'a>(object_id: &'a [u8], label: &'a [u8], gain_db: f32, extent: f32) -> LiveOverride<'a> {
    LiveOverride {
        base: Override {
            object_id,
            speaker_label: label,
            mute: false,
            gain_db,
            head_locked: None,
        },
        extent_scale: extent,
        extent_width_scale: 1.,
        extent_height_scale: 1.,
        extent_depth_scale: 1.,
        diffuse_scale: 1.,
        divergence_scale: 1.,
    }
}

/// One wide, partly diffuse object moving across four blocks.
fn moving(frames: u64) -> RenderScene {
    let mut blocks = Vec::new();
    for (i, az) in [0f32, 40., -60., 120.].into_iter().enumerate() {
        let mut b = object_block(i as u64 * 3000, (i as u64 + 1) * 3000, az);
        b.width = 30. + 10. * i as f32;
        b.diffuse = 0.3;
        blocks.push(b);
    }
    scene(
        vec![ObjectSpec {
            id: "AO_1001",
            gain: 1.,
            end: u64::MAX,
            tracks: vec![TrackSpec {
                blocks,
                direct: Vec::new(),
            }],
        }],
        frames,
    )
}

const ID: &[u8] = b"AO_1001";

#[test]
fn a_stream_equals_the_offline_render_for_any_pull_size() {
    let frames = 12000;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving(frames);
    for spread in [Spread::Cloud, Spread::None] {
        for smoothing in [0, 1500] {
            let p = prepare(&s, Dataset::kemar(), spread, smoothing);
            let expected = words(&offline(&p, &s, file.path(), smoothing, None, None));
            for sizes in [&[1usize, 7, 1023][..], &[4096], &[frames as usize + 5]] {
                let mut stream = open(&p, &s, file.path(), smoothing);
                let got = pull(&mut stream, sizes, 2 * frames as usize);
                assert_eq!(words(&got), expected, "{sizes:?}");
            }
        }
    }
}

#[test]
fn seeks_equal_the_offline_window() {
    let frames = 12000u64;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving(frames);
    // 1500 taps make the overlap longer than a block, so the pre-roll rounds up to whole blocks.
    for (dataset, smoothing) in [
        (Dataset::kemar(), 0),
        (kemar_padded(1500), 0),
        (Dataset::kemar(), 1500),
    ] {
        let p = prepare(&s, dataset, Spread::Cloud, smoothing);
        let mut stream = open(&p, &s, file.path(), smoothing);
        for target in [0, 511, 1024, 1500, 2049, 4097, 11999, 12000] {
            let expected = offline(
                &p,
                &s,
                file.path(),
                smoothing,
                Some(Window {
                    start: target,
                    count: frames,
                }),
                None,
            );
            seek(&mut stream, target);
            let got = pull(&mut stream, &[333, 1024], frames as usize);
            assert_eq!(words(&got), words(&expected), "target {target}");
        }
    }
}

#[test]
fn an_abandoned_seek_does_not_change_the_next_one() {
    let frames = 12000u64;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving(frames);
    let p = prepare(&s, Dataset::kemar(), Spread::Cloud, 0);
    let mut fresh = open(&p, &s, file.path(), 0);
    seek(&mut fresh, 3000);
    let expected = pull(&mut fresh, &[512], 4000);
    let mut stream = open(&p, &s, file.path(), 0);
    pull(&mut stream, &[700], 2100);
    assert!(stream.seek_begin(9000).unwrap());
    assert!(stream.seek_step().unwrap());
    seek(&mut stream, 3000);
    assert_eq!(words(&pull(&mut stream, &[512], 4000)), words(&expected));
}

#[test]
fn topology_edits_apply_at_most_every_100_ms() {
    let frames = 16384u64;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving(frames);
    let p = prepare(&s, Dataset::kemar(), Spread::Cloud, 0);
    // Edits land at the block boundaries the pulls stop at.
    let run = |edits: &[(usize, f32)]| {
        let mut stream = open(&p, &s, file.path(), 0);
        let mut out = Vec::new();
        let mut at = 0;
        for &(frame, scale) in edits {
            out.extend(pull(&mut stream, &[1024], frame - at));
            stream.set_overrides(&[edit(ID, b"", 0., scale)]).unwrap();
            at = frame;
        }
        out.extend(pull(&mut stream, &[1024], frames as usize - at));
        words(&out)
    };
    // The first edit applies at its block; the second waits until 2048 + 4800 → block 7168.
    let coalesced = run(&[(2048, 0.5), (3072, 0.25)]);
    assert_eq!(coalesced, run(&[(2048, 0.5), (7168, 0.25)]));
    assert_ne!(coalesced, run(&[(2048, 0.5), (8192, 0.25)]));
    assert_ne!(coalesced, run(&[(2048, 0.5)]));
    // Rapid edits between two applications only keep the newest.
    assert_eq!(
        coalesced,
        run(&[(2048, 0.5), (3072, 4.), (4096, 0.), (5120, 0.25)])
    );
}

#[test]
fn a_seek_adopts_the_pending_topology() {
    let frames = 16384u64;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving(frames);
    let p = prepare(&s, Dataset::kemar(), Spread::Cloud, 0);
    let mut stream = open(&p, &s, file.path(), 0);
    stream.set_overrides(&[edit(ID, b"", 0., 0.5)]).unwrap();
    pull(&mut stream, &[1024], 3072);
    stream.set_overrides(&[edit(ID, b"", 0., 0.25)]).unwrap();
    seek(&mut stream, 6000);
    let got = pull(&mut stream, &[1024], 6000);
    let mut fresh = open(&p, &s, file.path(), 0);
    fresh.set_overrides(&[edit(ID, b"", 0., 0.25)]).unwrap();
    seek(&mut fresh, 6000);
    assert_eq!(words(&got), words(&pull(&mut fresh, &[1024], 6000)));
    // Reverting to unity before a seek restores the prepared graph bit for bit.
    stream.set_overrides(&[edit(ID, b"", 0., 1.)]).unwrap();
    seek(&mut stream, 0);
    let expected = offline(&p, &s, file.path(), 0, None, None);
    assert_eq!(
        words(&pull(&mut stream, &[1024], frames as usize)),
        words(&expected)
    );
}

/// Two DirectSpeakers channels of one bed (M+030 on channel 0, M-030 on channel 1); `silent`
/// zeroes channel 0.
fn bed(frames: u64, silent: bool) -> (RenderScene, Temp) {
    let mut s = scene(
        vec![ObjectSpec {
            id: "AO_bed",
            gain: 1.,
            end: u64::MAX,
            tracks: vec![
                TrackSpec {
                    blocks: Vec::new(),
                    direct: vec![(vec!["M+030"], direct_block(0, frames))],
                },
                TrackSpec {
                    blocks: Vec::new(),
                    direct: vec![(vec!["M-030"], direct_block(0, frames))],
                },
            ],
        }],
        frames,
    );
    s.info.channels = 2;
    let temp = Temp::new("bed");
    let format = Format {
        channels: 2,
        sample_rate: RATE,
        sample_format: SampleFormat::Float32,
        channel_mask: None,
    };
    let mut w = Writer::new(
        File::create(temp.path()).unwrap(),
        format,
        WriterOptions::default(),
    )
    .unwrap();
    let a = sine(frames as usize, 0.5);
    let b = sine(frames as usize, 0.25);
    let samples: Vec<f32> = a
        .iter()
        .zip(&b)
        .flat_map(|(&l, &r)| [if silent { 0. } else { l }, r])
        .collect();
    w.write_frames(&samples).unwrap();
    w.finish().unwrap();
    (s, temp)
}

#[test]
fn channel_overrides_follow_their_speaker_label() {
    let frames = 8192u64;
    let (s, file) = bed(frames, false);
    let (silent, silent_file) = bed(frames, true);
    let p = prepare(&s, Dataset::kemar(), Spread::Cloud, 0);
    let expected = offline(&p, &silent, silent_file.path(), 0, None, None);
    let mut muted = edit(b"AO_bed", b"m+030", 0., 1.);
    muted.base.mute = true;
    // Before the first block a target applies at once; later it ramps over 20 ms (960 frames),
    // after which the muted channel contributes exactly nothing.
    let mut stream = open(&p, &s, file.path(), 0);
    stream.set_overrides(&[muted]).unwrap();
    let got = pull(&mut stream, &[1024], frames as usize);
    assert_eq!(words(&got), words(&expected));
    let mut stream = open(&p, &s, file.path(), 0);
    let mut got = pull(&mut stream, &[1024], 1024);
    stream.set_overrides(&[muted]).unwrap();
    got.extend(pull(&mut stream, &[1024], frames as usize - 1024));
    let settled = 2 * (1024 + 960);
    assert_eq!(words(&got[settled..]), words(&expected[settled..]));
    assert_ne!(words(&got[2048..settled]), words(&expected[2048..settled]));
    // The other channel's label and the whole object do not match.
    let mut other = edit(b"AO_bed", b"M+110", 0., 1.);
    other.base.mute = true;
    let mut stream = open(&p, &s, file.path(), 0);
    stream.set_overrides(&[other]).unwrap();
    let unmuted = offline(&p, &s, file.path(), 0, None, None);
    assert_eq!(
        words(&pull(&mut stream, &[1024], frames as usize)),
        words(&unmuted)
    );
}

#[test]
fn head_tracking_rotates_unless_the_source_is_head_locked() {
    let frames = 8192u64;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving(frames);
    let p = prepare(&s, Dataset::kemar(), Spread::Cloud, 0);
    let pose = [60., 10., -5.];
    let mut stream = open(&p, &s, file.path(), 0);
    stream.set_pose(pose);
    let rotated = pull(&mut stream, &[1024], frames as usize);
    assert_eq!(
        words(&rotated),
        words(&offline(&p, &s, file.path(), 0, None, Some(pose)))
    );
    let mut locked = edit(ID, b"", 0., 1.);
    locked.base.head_locked = Some(true);
    let mut stream = open(&p, &s, file.path(), 0);
    stream.set_pose(pose);
    stream.set_overrides(&[locked]).unwrap();
    let still = pull(&mut stream, &[1024], frames as usize);
    assert_ne!(words(&still), words(&rotated));
    let mut plain = open(&p, &s, file.path(), 0);
    assert_eq!(
        words(&still),
        words(&pull(&mut plain, &[1024], frames as usize))
    );
}

#[test]
fn spreader_groups_and_other_rates_do_not_stream() {
    let frames = 4096u64;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving(frames);
    let p = prepare(&s, Dataset::kemar(), Spread::SafSpreader, 0);
    assert!(!p.groups.is_empty());
    let settings = |sample_rate, spread| StreamSettings {
        inputs: 1,
        frames,
        sample_rate,
        smoothing_frames: 0,
        spread,
    };
    let e = FileStream::open(
        Arc::clone(&p),
        file.path(),
        settings(RATE, Spread::SafSpreader),
    )
    .err()
    .unwrap();
    assert_eq!(
        (e.code, e.message.as_str(), e.location),
        (
            2,
            "binaural realtime stream does not yet support saf_spreader mode (use cloud / none)",
            Location::None
        )
    );
    let e = FileStream::open(Arc::clone(&p), file.path(), settings(44100, Spread::Cloud))
        .err()
        .unwrap();
    assert_eq!(
        (e.code, e.message.as_str(), e.location),
        (
            1,
            "binaural renderer requires 48000 Hz input, got 44100",
            Location::Input
        )
    );
    // The preparation's sources still stream when the plan asks for another spread.
    let mut stream = FileStream::open(p, file.path(), settings(RATE, Spread::Cloud)).unwrap();
    assert_eq!(pull(&mut stream, &[1024], 100).len(), 200);
}

#[test]
fn a_seek_restarts_the_gain_ramps() {
    let frames = 8192u64;
    let file = input(&sine(frames as usize, 0.5));
    let s = moving(frames);
    let p = prepare(&s, Dataset::kemar(), Spread::Cloud, 0);
    let quiet = [edit(ID, b"", -12., 1.)];
    let mut fresh = open(&p, &s, file.path(), 0);
    fresh.set_overrides(&quiet).unwrap();
    let expected = pull(&mut fresh, &[1024], frames as usize);
    // A target set after the ramp started would glide over 20 ms; a seek (here without a
    // pre-roll block to absorb it) applies the current targets at once.
    let mut stream = open(&p, &s, file.path(), 0);
    pull(&mut stream, &[1024], 1024);
    stream.set_overrides(&quiet).unwrap();
    seek(&mut stream, 0);
    assert_eq!(
        words(&pull(&mut stream, &[1024], frames as usize)),
        words(&expected)
    );
}

/// One object at `az` for the whole input, with `width` and `diffuse`, head-locked as given.
fn still(frames: u64, az: f32, width: f32, diffuse: f32, head_locked: bool) -> RenderScene {
    let mut b = object_block(0, frames, az);
    b.width = width;
    b.diffuse = diffuse;
    b.head_locked = u32::from(head_locked);
    scene(
        vec![ObjectSpec {
            id: "AO_1001",
            gain: 1.,
            end: u64::MAX,
            tracks: vec![TrackSpec {
                blocks: vec![b],
                direct: Vec::new(),
            }],
        }],
        frames,
    )
}

fn ears(pcm: &[f32]) -> (f64, f64) {
    pcm.as_chunks::<2>().0.iter().fold((0., 0.), |(l, r), f| {
        (l + f64::from(f[0]).powi(2), r + f64::from(f[1]).powi(2))
    })
}

/// The live gain of the binaural stream (formerly part of the C++ `binaural_render_fixture_test`):
/// an edit after the first sample glides linearly over 20 ms (960 frames) from the old gain to
/// the target, which then holds exactly.
#[test]
fn a_live_gain_glides_over_20_ms_to_its_target() {
    let frames = 16384u64;
    let file = input(&sine(frames as usize, 0.25));
    let s = still(frames, 0., 0., 0., false);
    let p = prepare(&s, Dataset::kemar(), Spread::Cloud, 0);
    let baseline = pull(&mut open(&p, &s, file.path(), 0), &[1024], frames as usize);
    let mut stream = open(&p, &s, file.path(), 0);
    let mut changed = pull(&mut stream, &[1024], 1024);
    stream.set_overrides(&[edit(ID, b"", -20., 1.)]).unwrap();
    changed.extend(pull(&mut stream, &[1024], frames as usize - 1024));
    assert_eq!(changed.len(), baseline.len());
    let (ramp, target) = (960usize, 0.1f32);
    let mut worst = 0f64;
    for offset in 0..ramp + 1024 {
        let gain = if offset < ramp {
            1. + (target - 1.) * offset as f32 / (ramp - 1) as f32
        } else {
            target
        };
        for c in 0..2 {
            let i = (1024 + offset) * 2 + c;
            worst = worst.max(f64::from((changed[i] - baseline[i] * gain).abs()));
        }
    }
    assert!(worst < 2e-5, "{worst}");
}

/// Topology edits (formerly part of the C++ `binaural_render_fixture_test`): the crossfade starts
/// on the intact old state and the new extent shows afterwards; extent and width scales change
/// the output, and scaling back to 1 before pulling reproduces the unscaled render exactly.
#[test]
fn topology_edits_crossfade_from_the_old_state_and_revert_exactly() {
    let frames = 16384u64;
    let file = input(&sine(frames as usize, 0.25));
    let s = still(frames, 0., 90., 0., false);
    let p = prepare(&s, Dataset::kemar(), Spread::Cloud, 0);
    let baseline = pull(&mut open(&p, &s, file.path(), 0), &[1024], frames as usize);
    let mut stream = open(&p, &s, file.path(), 0);
    let mut changed = pull(&mut stream, &[1024], 1024);
    stream.set_overrides(&[edit(ID, b"", 0., 0.)]).unwrap();
    changed.extend(pull(&mut stream, &[1024], frames as usize - 1024));
    assert!(
        (changed[2048] - baseline[2048]).abs() < 1e-6
            && (changed[2049] - baseline[2049]).abs() < 1e-6
    );
    let tail: f64 = changed[(1024 + 2048) * 2..]
        .iter()
        .zip(&baseline[(1024 + 2048) * 2..])
        .map(|(a, b)| f64::from(a - b).powi(2))
        .sum();
    assert!(tail > 1e-5, "{tail}");

    let run = |edits: &[&[LiveOverride]]| {
        let mut stream = open(&p, &s, file.path(), 0);
        for e in edits {
            stream.set_overrides(e).unwrap();
        }
        pull(&mut stream, &[1024], frames as usize)
    };
    let mut width = edit(ID, b"", 0., 1.);
    width.extent_width_scale = 0.;
    assert_ne!(words(&run(&[&[edit(ID, b"", 0., 0.)]])), words(&baseline));
    assert_ne!(words(&run(&[&[width]])), words(&baseline));
    assert_eq!(
        words(&run(&[&[edit(ID, b"", 0., 0.)], &[edit(ID, b"", 0., 1.)]])),
        words(&baseline)
    );
}

/// Live extent scales equal blocks authored with the scaled, range-clamped extent (formerly the
/// C++ `binaural_render_fixture_test` check against the semantic policy's `extent.scale`, which
/// applies the same scale and clamp at import; see `mradm-scene`'s policy goldens).
#[test]
fn live_extent_scales_equal_scaled_blocks() {
    let frames = 4096u64;
    let file = input(&sine(frames as usize, 0.25));
    for cartesian in [false, true] {
        let (w, h, d) = if cartesian {
            (0.8, 0.6, 0.8)
        } else {
            (30., 200., 0.8)
        };
        let (wmax, hmax) = if cartesian { (1., 1.) } else { (360., 360.) };
        let block = |w: f32, h: f32, d: f32| {
            let mut b = object_block(0, frames, 0.);
            if cartesian {
                b.position.cartesian = 1;
                b.position.azimuth = 0.;
                b.position.x = 0.;
                b.position.y = 1.;
                b.position.z = 0.;
            }
            b.width = w;
            b.height = h;
            b.depth = d;
            scene(
                vec![ObjectSpec {
                    id: "AO_1001",
                    gain: 1.,
                    end: u64::MAX,
                    tracks: vec![TrackSpec {
                        blocks: vec![b],
                        direct: Vec::new(),
                    }],
                }],
                frames,
            )
        };
        let base = block(w, h, d);
        let p = prepare(&base, Dataset::kemar(), Spread::Cloud, 0);
        for scale in [0.5f32, 4.] {
            let authored = block(
                (w * scale).min(wmax),
                (h * scale).min(hmax),
                (d * scale).min(1.),
            );
            let pa = prepare(&authored, Dataset::kemar(), Spread::Cloud, 0);
            let expected = pull(
                &mut open(&pa, &authored, file.path(), 0),
                &[1024],
                frames as usize,
            );
            let mut live = edit(ID, b"", 0., 1.);
            live.extent_width_scale = scale;
            live.extent_height_scale = scale;
            live.extent_depth_scale = scale;
            let mut stream = open(&p, &base, file.path(), 0);
            stream.set_overrides(&[live]).unwrap();
            let actual = pull(&mut stream, &[333, 1024, 511], frames as usize);
            assert_eq!(
                words(&actual),
                words(&expected),
                "cartesian={cartesian} scale={scale}"
            );
        }
    }
}

/// Head tracking and head lock (formerly part of the C++ `binaural_render_fixture_test`): turning
/// left swings a front source onto the right ear; a head-locked block stays centred, a gain-only
/// edit keeps the lock, an explicit live false lifts it, the cloud and diffuse branches keep it, and
/// a sweep of the pose stays finite and bounded.
#[test]
fn head_locked_blocks_ignore_the_pose_until_unlocked() {
    let frames = 8192u64;
    let file = input(&sine(frames as usize, 0.25));
    let left = [90f32, 0., 0.];
    let render = |s: &RenderScene, edits: &[LiveOverride], pose: [f32; 3]| {
        let p = prepare(s, Dataset::kemar(), Spread::Cloud, 0);
        let mut stream = open(&p, s, file.path(), 0);
        stream.set_pose(pose);
        stream.set_overrides(edits).unwrap();
        ears(&pull(&mut stream, &[1024], frames as usize))
    };
    let balanced = |(l, r): (f64, f64), tolerance: f64| l > 1e-3 && (l - r).abs() < l * tolerance;
    let free = still(frames, 0., 0., 0., false);
    assert!(balanced(render(&free, &[], [0.; 3]), 0.5));
    let (l, r) = render(&free, &[], left);
    assert!(r > l * 2.);
    let mut lock = edit(ID, b"", 0., 1.);
    lock.base.head_locked = Some(true);
    assert!(balanced(render(&free, &[lock], left), 0.5));

    let locked = still(frames, 0., 0., 0., true);
    assert!(balanced(render(&locked, &[], left), 0.5));
    assert!(balanced(
        render(&locked, &[edit(ID, b"", -3., 1.)], left),
        0.5
    ));
    let mut unlock = edit(ID, b"", 0., 1.);
    unlock.base.head_locked = Some(false);
    let (l, r) = render(&locked, &[unlock], left);
    assert!(r > l * 2.);
    assert!(balanced(
        render(&still(frames, 0., 0.35 * 360., 0.35, true), &[], left),
        0.7
    ));

    let p = prepare(&free, Dataset::kemar(), Spread::Cloud, 0);
    let mut stream = open(&p, &free, file.path(), 0);
    let mut out = Vec::new();
    while out.len() < frames as usize * 2 {
        let yaw = (90. * (out.len() / 2) as f32 / frames as f32).min(90.);
        stream.set_pose([yaw, 0., 0.]);
        let got = pull(&mut stream, &[512], 512);
        if got.is_empty() {
            break;
        }
        out.extend(got);
    }
    assert!(out.iter().all(|v| v.is_finite() && v.abs() < 8.));
    let window = out.len() / 2 / 5;
    let (el, er) = ears(&out[..window * 2]);
    let (ll, lr) = ears(&out[out.len() - window * 2..]);
    assert!((el - er).abs() < el * 0.6 && lr > ll * 1.5);
}
