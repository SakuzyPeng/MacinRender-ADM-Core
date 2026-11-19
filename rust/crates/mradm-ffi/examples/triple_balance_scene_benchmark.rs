//! `mr_adm_triple_balance_scene_benchmark [layout=9.1.6] [seconds 10..600 = 60]`: Triple Balance
//! Scene stream rendering (formerly `tests/manual/triple_balance_scene_benchmark.cpp`, which drove
//! the C++ live renderer, `SceneStreamEngine` and `SceneOutputSession`). Release builds only.
//!
//! The scene is an RC 7.1.2 bed and three objects; two of them move and change size every 10 ms
//! block. It renders 1000 blocks through the renderer alone for the real-time factor, then submits
//! `seconds` of blocks to a Scene stream played by a Scene output on a null output clock, and
//! prints the RTF, the elapsed time, underruns, presented frames and the resident memory after
//! 5 s, at the end and its peak. Exit code 0 when RTF < 1, the output ended without failing or
//! underrunning, every frame was presented and the memory grew by at most 4 MiB after 5 s.
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, RoomRequest, Screens, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::output::{DeviceProgress, Session};
use mradm_engine::scene::renderer;
use mradm_engine::scene::stream::{
    Config, Descriptor, FrameInput, PlaneView, Stream, Submit, frame_flag,
};
use mradm_ffi::api::render::FfiHost;
use mradm_render::live::{self, Entry, FrameView, Plane, Role, State, Update, field};
use std::process::ExitCode;
use std::sync::Arc;
use std::time::{Duration, Instant};

mod probe;
use probe::NullClock;

const RATE: u32 = 48000;
const BLOCK: u32 = 480;
const ELEMENTS: u64 = 13;
const LABELS: [&str; 10] = [
    "RC_L", "RC_R", "RC_C", "RC_LFE", "RC_Lss", "RC_Rss", "RC_Lrs", "RC_Rrs", "RC_Lts", "RC_Rts",
];

/// Role 0 object, 1 DirectSpeakers, 2 LFE (`ElementRole`).
fn role(i: u64) -> u32 {
    match i {
        3 => 2,
        0..10 => 1,
        _ => 0,
    }
}

struct Fixture {
    descriptors: Vec<Descriptor>,
    initial: Vec<Entry>,
    pcm: Vec<Vec<f32>>,
    updates: Vec<Update>,
    start: i64,
    first: bool,
}

impl Fixture {
    fn new() -> Self {
        let mut descriptors = Vec::new();
        let mut initial = Vec::new();
        let mut pcm = Vec::new();
        for i in 0..ELEMENTS {
            descriptors.push(Descriptor {
                element_id: i,
                role: role(i),
                speaker_label: if i < 10 {
                    LABELS[i as usize].as_bytes().to_vec()
                } else {
                    Vec::new()
                },
                has_position: false,
                position: [0., 1., 0.],
                flags: 0,
                identity: None,
            });
            let mut state = State {
                valid_fields: field::KNOWN & !field::POSITION,
                ..State::default()
            };
            if i >= 10 {
                let size = if i < 12 { 0.3 } else { 0. };
                state.valid_fields |= field::POSITION;
                state.position = [-0.4, 0.3, 0.5];
                state.width = size;
                state.height = size;
                state.depth = size;
            }
            initial.push(Entry {
                element_id: i,
                state,
            });
            pcm.push(
                (0..BLOCK as usize)
                    .map(|f| ((f as u64 * (i + 1)) as f64 * 0.03).sin() as f32 * 0.01)
                    .collect(),
            );
        }
        Self {
            descriptors,
            initial,
            pcm,
            updates: Vec::new(),
            start: 0,
            first: true,
        }
    }

    fn update(&mut self, block: u64) {
        self.start = (block * u64::from(BLOCK)) as i64;
        self.first = block == 0;
        self.updates.clear();
        for i in [10u64, 11] {
            let mut state = self.initial[i as usize].state;
            let even = block.is_multiple_of(2);
            state.position[0] = if even { -0.4 } else { 0.4 };
            let size = if even { 0.3 } else { 0.6 };
            state.width = size;
            state.height = size;
            state.depth = size;
            self.updates.push(Update {
                element_id: i,
                offset: 0,
                ramp: BLOCK,
                jump: false,
                changed: field::POSITION | field::EXTENT,
                cleared: 0,
                state,
            });
        }
    }

    fn initial(&self) -> &[Entry] {
        if self.first { &self.initial } else { &[] }
    }

    fn elements(&self) -> Vec<live::Element<'_>> {
        self.descriptors
            .iter()
            .map(|d| live::Element {
                id: d.element_id,
                role: Some(match d.role {
                    0 => Role::Object,
                    2 => Role::Lfe,
                    _ => Role::DirectSpeaker,
                }),
                speaker_label: &d.speaker_label,
                object_id: b"",
                has_position: d.has_position,
                position: d.position,
            })
            .collect()
    }
}

fn renderer_config(layout: &str, sink: bool) -> renderer::Config<'_> {
    renderer::Config {
        selection: Selection::TripleBalance,
        layout_id: layout.as_bytes(),
        apple_geometry: false,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread: BinauralSpreadMode::Automatic,
        split_power: false,
        smoothing_frames: 0,
        sample_rate: RATE,
        sofa: None,
        screens: Screens {
            has_reference: false,
            reference: RoomScreenGeometry::default(),
            has_reproduction: false,
            reproduction: RoomScreenGeometry::default(),
        },
        room: RoomRequest {
            package: None,
            hrir: b"",
            metres: 0.,
            quality: 0,
        },
        sink,
    }
}

fn text(e: &mradm_render::routing::Error) -> String {
    String::from_utf8_lossy(&e.message).into_owned()
}

fn run() -> Result<bool, String> {
    let args: Vec<String> = std::env::args().collect();
    let layout = args.get(1).map_or("9.1.6", String::as_str).to_owned();
    let seconds: u64 = match args.get(2) {
        Some(s) => s.parse().map_err(|_| "duration must be 10..600 seconds")?,
        None => 60,
    };
    if !(10..=600).contains(&seconds) {
        return Err("duration must be 10..600 seconds".to_owned());
    }
    let mut fixture = Fixture::new();

    let mut created =
        renderer::create(&renderer_config(&layout, false), &FfiHost).map_err(|e| text(&e))?;
    created
        .renderer
        .configure(1, &fixture.elements())
        .map_err(|e| text(&e))?;
    let mut pcm = vec![0f32; BLOCK as usize * created.renderer.output_channels() as usize];
    let start = Instant::now();
    for block in 0..1000 {
        fixture.update(block);
        let planes: Vec<Plane<'_>> = fixture
            .pcm
            .iter()
            .zip(0..)
            .map(|(samples, element_id)| Plane {
                element_id,
                has_signal: true,
                samples,
            })
            .collect();
        let frame = FrameView {
            generation: 1,
            duration: BLOCK,
            planes: &planes[..],
            initial: fixture.initial(),
            updates: &fixture.updates,
            trace: b"",
        };
        created
            .renderer
            .render(&frame, &mut pcm)
            .map_err(|e| text(&e))?;
    }
    let rtf = start.elapsed().as_secs_f64() / 10.;
    drop(created);

    let config = Config {
        output_sample_rate: RATE,
        ..Config::default()
    };
    let stream = Arc::new(
        Stream::create(&config, &renderer_config(&layout, true), Arc::new(FfiHost))
            .map_err(|e| text(&e))?,
    );
    let (_, channels) = stream.output_format();
    // The null backend: a protected output without device volume.
    let output = Session::new(stream.clone(), true, false).map_err(|e| text(&e))?;
    output.reset_epoch(1, 0).map_err(|e| text(&e))?;
    stream
        .configure_generation(1, 1, &fixture.descriptors)
        .map_err(|e| text(&e))?;
    let clock = NullClock::default();
    let (rss_5s, peak, elapsed, status) = std::thread::scope(|scope| -> Result<_, String> {
        scope.spawn(|| {
            clock.run(channels as usize, RATE, |buffer, frames| {
                if output.pull(buffer, frames).active {
                    output.release();
                }
            });
        });
        let result = (|| {
            let (mut early, mut peak) = (0u64, 0u64);
            let started = Instant::now();
            for block in 0..seconds * 100 {
                fixture.update(block);
                let planes: Vec<PlaneView<'_>> = fixture
                    .pcm
                    .iter()
                    .zip(0..)
                    .map(|(samples, element_id)| PlaneView {
                        element_id,
                        sample_count: BLOCK,
                        stride: 1,
                        has_signal: true,
                        samples: Some(samples),
                    })
                    .collect();
                let frame = FrameInput {
                    epoch: 1,
                    generation: 1,
                    start: fixture.start,
                    duration: BLOCK,
                    flags: frame_flag::STATE_COMPLETE,
                    planes: &planes[..],
                    initial: fixture.initial(),
                    updates: &fixture.updates,
                };
                if stream.submit_frame(&frame, 5000).map_err(|e| text(&e))? != Submit::Accepted {
                    return Err("benchmark producer timed out".to_owned());
                }
                if block == 15 {
                    output.play();
                    clock.resume();
                }
                if block == 500 {
                    early = probe::rss();
                }
                if block % 100 == 0 {
                    peak = peak.max(probe::rss());
                }
                if stream.status().failed {
                    return Err("benchmark Scene worker failed".to_owned());
                }
            }
            stream
                .signal_end(1, (seconds * u64::from(RATE)) as i64)
                .map_err(|e| text(&e))?;
            let deadline = Instant::now() + Duration::from_secs(5);
            loop {
                let s = output.status(DeviceProgress::default());
                if s.ended || s.failed || Instant::now() >= deadline {
                    break;
                }
                std::thread::sleep(Duration::from_millis(10));
            }
            Ok((early, peak, started.elapsed().as_secs_f64()))
        })();
        let status = output.status(DeviceProgress::default());
        clock.stop();
        let (early, peak, elapsed) = result?;
        Ok((early, peak, elapsed, status))
    })?;
    let final_rss = probe::rss();
    println!(
        "layout={layout} rtf={rtf} elapsed_s={elapsed} underruns={} frames={} rss_5s={rss_5s} rss_final={final_rss} rss_peak={peak}",
        status.underruns, status.presented_frames
    );
    Ok(rtf < 1.
        && status.ended
        && !status.failed
        && status.underruns == 0
        && status.presented_frames == seconds * u64::from(RATE)
        && final_rss <= rss_5s + 4 * 1024 * 1024)
}

fn main() -> ExitCode {
    if cfg!(debug_assertions) {
        eprintln!("Run this benchmark from a Release build.");
        return ExitCode::from(2);
    }
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(1),
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(1)
        }
    }
}
