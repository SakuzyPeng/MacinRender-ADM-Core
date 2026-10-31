//! Hand-built render scenes, inputs and driver runs shared by the backend tests.
#![allow(dead_code)]
use mradm_adm::Snapshot;
use mradm_adm::records::{DirectBlock, Object, ObjectBlock, Span, Track};
use mradm_render::offline::{self, Driver, Kernel, Window};
use mradm_render::routing::Override;
use mradm_render::stream::{self, FileStream};
use mradm_render::vbap::Binding;
use mradm_scene::{Info, RenderScene, SourceKind};
use mradm_wav::{Format, SampleFormat, Writer, WriterOptions};
use std::fs::File;
use std::path::PathBuf;
use std::sync::Arc;

pub const RATE: u32 = 48000;

pub struct Temp(PathBuf);

impl Temp {
    pub fn new(name: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        Self(std::env::temp_dir().join(format!(
            "mradm-render-test-{}-{name}-{n}.wav",
            std::process::id()
        )))
    }
    pub fn path(&self) -> &str {
        self.0.to_str().unwrap()
    }
}

impl Drop for Temp {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

/// A mono float input of `frames` frames.
pub fn input(samples: &[f32]) -> Temp {
    let temp = Temp::new("input");
    let format = Format {
        channels: 1,
        sample_rate: RATE,
        sample_format: SampleFormat::Float32,
        channel_mask: None,
    };
    let mut writer = Writer::new(
        File::create(&temp.0).unwrap(),
        format,
        WriterOptions::default(),
    )
    .unwrap();
    writer.write_frames(samples).unwrap();
    writer.finish().unwrap();
    temp
}

pub fn sine(frames: usize, amplitude: f32) -> Vec<f32> {
    (0..frames)
        .map(|i| amplitude * (2. * std::f32::consts::PI * 440. * i as f32 / RATE as f32).sin())
        .collect()
}

/// Objects and DirectSpeakers blocks of one track (labels beside each DirectSpeakers block).
#[derive(Default)]
pub struct TrackSpec {
    pub blocks: Vec<ObjectBlock>,
    pub direct: Vec<(Vec<&'static str>, DirectBlock)>,
}

pub struct ObjectSpec {
    pub id: &'static str,
    pub gain: f32,
    pub end: u64,
    pub tracks: Vec<TrackSpec>,
}

/// One object per spec, track `i` on input channel `i` of a mono file.
pub fn scene(objects: Vec<ObjectSpec>, frames: u64) -> RenderScene {
    let mut s = Snapshot::default();
    let string = |s: &mut Snapshot, v: &str| {
        s.strings.push(v.to_owned());
        (s.strings.len() - 1) as u32
    };
    for object in objects {
        let tracks_offset = s.tracks.len();
        for (i, track) in object.tracks.into_iter().enumerate() {
            let blocks = Span {
                offset: s.object_blocks.len(),
                len: track.blocks.len(),
            };
            s.object_blocks.extend(track.blocks);
            let ds_offset = s.direct_blocks.len();
            for (labels, mut block) in track.direct {
                let offset = s.indices.len();
                for label in &labels {
                    let id = string(&mut s, label);
                    s.indices.push(id);
                }
                block.labels = Span {
                    offset,
                    len: labels.len(),
                };
                s.direct_blocks.push(block);
            }
            let name = format!("ATU_{:08}", s.tracks.len() + 1);
            let uid = string(&mut s, &name);
            s.tracks.push(Track {
                uid,
                channel: i as i32,
                blocks,
                ds_blocks: Span {
                    offset: ds_offset,
                    len: s.direct_blocks.len() - ds_offset,
                },
            });
        }
        let id = string(&mut s, object.id);
        s.objects.push(Object {
            id,
            gain: object.gain,
            end: object.end,
            tracks: Span {
                offset: tracks_offset,
                len: s.tracks.len() - tracks_offset,
            },
            ..Object::default()
        });
    }
    RenderScene::new(
        Info {
            sample_rate: RATE,
            channels: 1,
            frames,
            source_kind: SourceKind::Adm,
            input_layout: String::new(),
            input_channel_labels: Vec::new(),
        },
        s,
    )
}

/// The block defaults of the C++ `SceneObjectBlock`.
pub fn object_block(start: u64, end: u64, azimuth: f32) -> ObjectBlock {
    let mut b = ObjectBlock {
        start,
        end,
        gain: 1.,
        azimuth_range: 45.,
        ..ObjectBlock::default()
    };
    b.position.azimuth = azimuth;
    b.position.distance = 1.;
    b
}

pub fn direct_block(start: u64, end: u64) -> DirectBlock {
    DirectBlock {
        start,
        end,
        gain: 1.,
        distance: 1.,
        ..DirectBlock::default()
    }
}

/// The offline driver's PCM (the emitted frames, as the C++ caller writes them).
pub fn render_offline(
    kernel: Kernel,
    input: &Temp,
    outputs: usize,
    frames: u64,
    smoothing: u64,
    window: Option<Window>,
) -> Result<Vec<f32>, offline::Error> {
    let mut driver = Driver::new(
        Driver::open_reader(input.path())?,
        offline::Settings {
            kernel,
            inputs: 1,
            outputs,
            frames,
            sample_rate: RATE,
            default_interp_ms: 5,
            smoothing_frames: smoothing,
            window,
            meter_window: None,
            backend: "test".to_owned(),
        },
    )?;
    let mut pcm = Vec::new();
    while let Some(b) = driver.render()? {
        pcm.extend_from_slice(
            &driver.output()[b.emit_offset * outputs..][..b.emit_frames * outputs],
        );
        driver.commit()?;
    }
    driver.finish()?;
    Ok(pcm)
}

/// The file stream's PCM pulled in `chunks`, and the error that ended it early, if any.
#[allow(clippy::too_many_arguments)]
pub fn render_stream(
    kernel: &Kernel,
    bindings: &[Binding],
    input: &Temp,
    outputs: usize,
    frames: u64,
    smoothing: u64,
    chunks: &[usize],
    overrides: &[Override],
) -> (Vec<f32>, Option<offline::Error>) {
    let mut s = FileStream::open(
        input.path(),
        kernel,
        Arc::from(bindings.to_vec()),
        stream::Settings {
            inputs: 1,
            outputs,
            frames,
            sample_rate: RATE,
            default_interp_ms: 5,
            smoothing_frames: smoothing,
        },
    )
    .unwrap();
    s.set_overrides(overrides).unwrap();
    let mut pcm = Vec::new();
    for chunk in chunks.iter().cycle() {
        let mut buffer = vec![0.; chunk * outputs];
        match s.process(&mut buffer, *chunk) {
            Ok(0) => break,
            Ok(got) => pcm.extend_from_slice(&buffer[..got * outputs]),
            Err(e) => return (pcm, Some(e)),
        }
    }
    (pcm, None)
}

pub fn bits(v: &[f32]) -> Vec<u32> {
    v.iter().map(|x| x.to_bits()).collect()
}

/// Two starts written in reverse chronological order. Each winner is shorter than its overwritten
/// peers and interpolates from its predecessor, so a peer would show up either as the ramp origin
/// or by resuming after the winner ends.
pub fn same_start_track(direct: bool, include_overwritten: bool) -> TrackSpec {
    let mut track = TrackSpec::default();
    for i in if include_overwritten { 0 } else { 39 }..40 {
        for group in [1u64, 0] {
            let winner = i == 39;
            let start = group * 2048;
            let end = start + if winner { 1024 } else { 2048 };
            let azimuth = if winner {
                if group == 0 { 97.5 } else { -37.5 }
            } else {
                -97.5 + 5. * i as f32
            };
            if direct {
                let mut b = direct_block(start, end);
                b.azimuth = azimuth;
                b.has_position = 1;
                b.gain = if winner { 0.375 } else { 0.9 };
                let mut labels = Vec::new();
                if !winner {
                    // An overwritten LFE1 next to a winning LFE2 must not make 22.2 split-power
                    // routing see two semantic LFE channels.
                    b.low_pass_present = 1;
                    b.low_pass = 120.;
                } else if group == 0 {
                    labels.push("LFE2");
                }
                track.direct.push((labels, b));
                continue;
            }
            let mut b = object_block(start, end, azimuth);
            b.position.elevation = if winner { 0. } else { 30. };
            b.gain = if winner { 0.375 } else { 0.9 };
            if !winner {
                b.width = 40.;
                b.height = 20.;
                b.diffuse = 0.25;
                b.divergence = 0.5;
                b.azimuth_range = 30.;
                b.jump = u32::from(i % 2 == 0);
            }
            track.blocks.push(b);
        }
    }
    track
}

pub fn single(id: &'static str, gain: f32, track: TrackSpec) -> Vec<ObjectSpec> {
    vec![ObjectSpec {
        id,
        gain,
        end: u64::MAX,
        tracks: vec![track],
    }]
}

fn open_stream(
    kernel: &Kernel,
    bindings: &[Binding],
    input: &Temp,
    outputs: usize,
    frames: u64,
    smoothing: u64,
) -> FileStream {
    FileStream::open(
        input.path(),
        kernel,
        Arc::from(bindings.to_vec()),
        stream::Settings {
            inputs: 1,
            outputs,
            frames,
            sample_rate: RATE,
            default_interp_ms: 5,
            smoothing_frames: smoothing,
        },
    )
    .unwrap()
}

fn pull(s: &mut FileStream, outputs: usize, chunks: &[usize], limit: usize) -> Vec<f32> {
    let mut pcm = Vec::new();
    for chunk in chunks.iter().cycle() {
        let want = (*chunk).min(limit - pcm.len() / outputs);
        if want == 0 {
            break;
        }
        let mut buffer = vec![0.; want * outputs];
        let got = s.process(&mut buffer, want).unwrap();
        if got == 0 {
            break;
        }
        pcm.extend_from_slice(&buffer[..got * outputs]);
    }
    pcm
}

/// A file stream that played a while and then seeks gives, for any pull partition, exactly the
/// offline window starting at the target (PCM consistency 4.9): the window's block grid, which
/// object smoothing interpolates across, and its pre-roll block for HOA and EAR state.
pub fn assert_seeks_follow_windows(
    kernel: &Kernel,
    bindings: &[Binding],
    input: &Temp,
    outputs: usize,
    frames: u64,
    smoothing: u64,
    label: &str,
) {
    let block = smoothing.max(1024);
    let targets = [
        0,
        511,
        block - 1,
        block,
        block + 476,
        2 * block + 1,
        frames - 7,
        frames,
    ];
    for target in targets {
        let window = render_offline(
            kernel.clone(),
            input,
            outputs,
            frames,
            smoothing,
            Some(Window {
                start: target,
                count: u64::MAX,
            }),
        )
        .unwrap();
        for chunks in [&[1024][..], &[1, 7, 1023, 4096]] {
            let mut s = open_stream(kernel, bindings, input, outputs, frames, smoothing);
            pull(&mut s, outputs, &[333], 600);
            s.seek(target).unwrap();
            assert_eq!(
                bits(&pull(&mut s, outputs, chunks, usize::MAX)),
                bits(&window),
                "{label} smoothing={smoothing} seek={target} chunks={chunks:?}"
            );
        }
    }
}

/// A seek restarts the live gain de-zipper at its targets, so a ramp in progress before the seek
/// and the frames the seek drops leave no trace. The target is 10 frames past a block edge: a
/// loudspeaker mix drops only those, while the HOA and EAR pre-roll block (longer than the 20 ms
/// ramp) shows a leftover ramp only through the state it leaves.
pub fn assert_seeks_restart_live_gain(
    kernel: &Kernel,
    bindings: &[Binding],
    input: &Temp,
    outputs: usize,
    frames: u64,
    overrides: &[Override],
) {
    let target = 2058;
    let mut fresh = open_stream(kernel, bindings, input, outputs, frames, 0);
    fresh.set_overrides(overrides).unwrap();
    fresh.seek(target).unwrap();
    let mut ramping = open_stream(kernel, bindings, input, outputs, frames, 0);
    pull(&mut ramping, outputs, &[64], 64);
    ramping.set_overrides(overrides).unwrap();
    pull(&mut ramping, outputs, &[100], 100);
    ramping.seek(target).unwrap();
    assert_eq!(
        bits(&pull(&mut ramping, outputs, &[512], usize::MAX)),
        bits(&pull(&mut fresh, outputs, &[512], usize::MAX))
    );
}

/// One object moving across three blocks, the later two interpolated, so object smoothing and the
/// block grid change the gains of every block.
pub fn moving_track(frames: u64) -> TrackSpec {
    let mut second = object_block(2900, 6100, -70.);
    second.interpolation_present = 1;
    second.interpolation = 700;
    let mut third = object_block(6100, frames, 110.);
    third.interpolation_present = 1;
    third.interpolation = 2400;
    TrackSpec {
        blocks: vec![object_block(0, 2900, 30.), second, third],
        ..TrackSpec::default()
    }
}

pub fn gain_override(object_id: &'static [u8]) -> [Override<'static>; 1] {
    [Override {
        object_id,
        speaker_label: b"",
        mute: false,
        gain_db: -12.,
        head_locked: None,
    }]
}
