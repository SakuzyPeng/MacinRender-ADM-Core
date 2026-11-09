//! The Scene stream engine (PCM consistency 5.4 B): the C++ `SceneStreamEngine` rules for errors,
//! the media timeline (rational resampling, preroll, end of stream), backpressure, epochs, fences,
//! backend switches, semantic policy diagnostics and the head-tracking lookahead.
mod common;
use common::backend::TestHost;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, RoomRequest, Screens, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::renderer;
use mradm_engine::scene::stream::{
    Config, Descriptor, FrameInput, PlaneView, State, Stream, Submit, frame_flag, pull_flag,
};
use mradm_render::live::{self, field};
use mradm_render::routing::Error;
use std::sync::Arc;
use std::sync::mpsc;
use std::time::{Duration, Instant};

fn renderer_config(
    selection: Selection,
    layout: &'static [u8],
    rate: u32,
) -> renderer::Config<'static> {
    renderer::Config {
        selection,
        layout_id: layout,
        apple_geometry: false,
        speaker_spread: SpeakerSpread::Automatic,
        binaural_spread: BinauralSpreadMode::Automatic,
        split_power: false,
        smoothing_frames: 0,
        sample_rate: rate,
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
        sink: true,
    }
}

fn config(output_rate: u32, ring: u32) -> Config {
    Config {
        output_sample_rate: output_rate,
        output_ring_frames: ring,
        startup_watermark_frames: 1,
        ..Config::default()
    }
}

fn stream(input_rate: u32, config: &Config) -> Stream {
    Stream::create(
        config,
        &renderer_config(Selection::Saf, b"0+2+0", input_rate),
        Arc::new(TestHost),
    )
    .unwrap_or_else(|e| panic!("{}", text(&e)))
}

fn text(e: &Error) -> String {
    String::from_utf8_lossy(&e.message).into_owned()
}

fn rejected<T>(result: Result<T, Error>) -> String {
    match result {
        Ok(_) => panic!("accepted"),
        Err(e) => text(&e),
    }
}

fn object(id: u64) -> Descriptor {
    Descriptor {
        element_id: id,
        role: 0,
        speaker_label: Vec::new(),
        has_position: false,
        position: [0., 1., 0.],
        flags: 0,
        identity: None,
    }
}

fn complete_state() -> live::State {
    live::State {
        valid_fields: field::KNOWN,
        position: [0.25, 0.75, 0.],
        ..live::State::default()
    }
}

/// One frame of `samples` for element 1, starting at `start`.
fn frame<'a>(
    planes: &'a [PlaneView<'a>],
    start: i64,
    duration: u32,
    initial: &'a [live::Entry],
    updates: &'a [live::Update],
) -> FrameInput<'a, [PlaneView<'a>]> {
    FrameInput {
        epoch: 1,
        generation: 1,
        start,
        duration,
        flags: frame_flag::STATE_COMPLETE,
        planes,
        initial,
        updates,
    }
}

fn plane(samples: &[f32]) -> [PlaneView<'_>; 1] {
    [PlaneView {
        element_id: 1,
        sample_count: samples.len() as u32,
        stride: 1,
        has_signal: true,
        samples: Some(samples),
    }]
}

fn open(s: &Stream, target: i64) {
    s.begin_epoch(1, target).unwrap();
    s.configure_generation(1, 1, &[object(1)]).unwrap();
}

/// Submits `total` frames in blocks of `block` from `start`, then the end marker.
fn feed(s: &Stream, start: i64, total: u32, block: u32) {
    let signal: Vec<f32> = (0..block).map(|i| (i as f32 * 0.01).sin() * 0.25).collect();
    let initial = [live::Entry {
        element_id: 1,
        state: complete_state(),
    }];
    let mut position = 0;
    while position < total {
        let n = block.min(total - position);
        let planes = plane(&signal[..n as usize]);
        let first = position == 0;
        let view = frame(
            &planes,
            start + i64::from(position),
            n,
            if first { &initial } else { &[] },
            &[],
        );
        assert_eq!(s.submit_frame(&view, 5000).unwrap(), Submit::Accepted);
        position += n;
    }
    s.signal_end(1, start + i64::from(total)).unwrap();
}

/// Pulls until the end of stream; returns the media frames.
fn drain(s: &Stream) -> u64 {
    let channels = s.output_format().1 as usize;
    let mut buffer = vec![0f32; 256 * channels];
    let mut frames = 0;
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let p = s.pull(&mut buffer, 256);
        frames += u64::from(p.media_frames);
        assert_eq!(p.flags & pull_flag::FAILED, 0, "stream failed");
        if p.flags & pull_flag::EOS != 0 {
            return frames;
        }
        assert!(Instant::now() < deadline, "no end of stream");
        if p.media_frames == 0 {
            std::thread::sleep(Duration::from_millis(1));
        }
    }
}

#[test]
fn creation_and_control_errors_keep_their_texts() {
    let bad_rate = Stream::create(
        &config(48000, 64),
        &renderer_config(Selection::Saf, b"0+2+0", 7999),
        Arc::new(TestHost),
    );
    assert_eq!(
        rejected(bad_rate),
        "Scene stream sample rates must be in the 8-192 kHz range"
    );
    let mut watermark = config(48000, 64);
    watermark.startup_watermark_frames = 65;
    let capacity = Stream::create(
        &watermark,
        &renderer_config(Selection::Saf, b"0+2+0", 48000),
        Arc::new(TestHost),
    );
    assert_eq!(
        rejected(capacity),
        "Scene stream queue/ring capacities are invalid"
    );

    let s = stream(48000, &config(48000, 64));
    assert_eq!(
        rejected(s.configure_generation(1, 1, &[object(1)])),
        "Scene generation uses an unknown epoch"
    );
    s.begin_epoch(2, 0).unwrap();
    assert_eq!(
        rejected(s.begin_epoch(2, 0)),
        "Scene epoch IDs must increase monotonically"
    );
    let mut role = object(1);
    role.role = 3;
    assert_eq!(
        rejected(s.configure_generation(2, 1, &[role])),
        "Scene generation contains an invalid element role"
    );
    assert_eq!(
        rejected(s.configure_generation(2, 1, &[object(1), object(1)])),
        "Scene generation contains a duplicate element ID"
    );
    let mut far = object(1);
    far.has_position = true;
    far.position = [1.5, 0., 0.];
    assert_eq!(
        rejected(s.configure_generation(2, 1, &[far])),
        "Scene element Cartesian position must use finite canonical [-1, 1] coordinates"
    );
    s.configure_generation(2, 1, &[object(1)]).unwrap();
    let mut moved = object(1);
    moved.speaker_label = b"M+030".to_vec();
    assert_eq!(
        rejected(s.configure_generation(2, 1, &[moved])),
        "Scene generation topology cannot change after configuration"
    );

    let samples = [0.5f32; 8];
    let planes = plane(&samples);
    let mut view = frame(&planes, 0, 8, &[], &[]);
    view.epoch = 2;
    view.flags = 1 << 7;
    assert_eq!(
        rejected(s.submit_frame(&view, 0)),
        "Scene frame has unknown flags"
    );
    view.flags = frame_flag::STATE_COMPLETE;
    view.generation = 9;
    assert_eq!(
        rejected(s.submit_frame(&view, 0)),
        "Scene frame uses an unknown generation"
    );
    view.generation = 1;
    let nan = [f32::NAN; 8];
    let nan_planes = plane(&nan);
    let mut bad = frame(&nan_planes, 0, 8, &[], &[]);
    bad.epoch = 2;
    assert_eq!(
        rejected(s.submit_frame(&bad, 0)),
        "Scene PCM samples must be finite"
    );
    let update = [live::Update {
        element_id: 1,
        offset: 8,
        ramp: 0,
        jump: false,
        changed: field::LINEAR_GAIN,
        cleared: 0,
        state: complete_state(),
    }];
    let mut late = frame(&planes, 0, 8, &[], &update);
    late.epoch = 2;
    assert_eq!(
        rejected(s.submit_frame(&late, 0)),
        "Scene metadata update is invalid"
    );
    assert_eq!(s.submit_frame(&view, 0).unwrap(), Submit::Accepted);
    view.start = 9;
    assert_eq!(
        rejected(s.submit_frame(&view, 0)),
        "Scene frame creates a timeline gap or overlap"
    );
    view.start = 8;
    view.flags |= frame_flag::DISCONTINUITY;
    assert_eq!(
        rejected(s.submit_frame(&view, 0)),
        "Scene discontinuity flag is only valid on the first frame after begin_epoch"
    );
    assert_eq!(
        rejected(s.signal_end(2, 9)),
        "Scene end marker does not match the next expected sample"
    );
    s.signal_end(2, 8).unwrap();
    assert_eq!(
        rejected(s.signal_end(2, 8)),
        "Scene epoch is already closed"
    );
    assert_eq!(
        rejected(s.switch_backend(&renderer_config(Selection::Saf, b"0+2+0", 44100))),
        "live Scene backend switches cannot change the input sample rate"
    );
    assert_eq!(
        rejected(s.switch_backend(&renderer_config(Selection::Saf, b"0+5+0", 48000))),
        "live Scene backend switches cannot change the output channel count"
    );
}

#[test]
fn every_frame_reaches_the_player_once_and_the_stream_ends() {
    let s = stream(48000, &config(48000, 1024));
    open(&s, 0);
    feed(&s, 0, 5000, 700);
    assert_eq!(drain(&s), 5000);
    let status = s.status();
    assert_eq!(status.state, State::Ended);
    assert!(status.ended && status.production_complete && !status.failed);
    assert_eq!(status.media_frames_pulled, 5000);
}

#[test]
fn resampling_rounds_the_combined_timeline_once_at_the_end() {
    let s = stream(48000, &config(44100, 1024));
    open(&s, 0);
    feed(&s, 0, 4999, 1000);
    // ceil(4999 * 44100 / 48000) = ceil(4592.83)
    assert_eq!(drain(&s), 4593);
}

#[test]
fn frames_before_the_epoch_target_stay_hidden() {
    let s = stream(48000, &config(48000, 1024));
    open(&s, 1500);
    feed(&s, 0, 4000, 1024);
    assert_eq!(drain(&s), 2500);
    let r = stream(48000, &config(44100, 1024));
    open(&r, 1500);
    feed(&r, 0, 4000, 1024);
    // ceil(2500 * 44100 / 48000) = ceil(2296.875)
    assert_eq!(drain(&r), 2297);
}

#[test]
fn a_short_read_is_an_underrun_until_the_end() {
    let s = stream(48000, &config(48000, 1024));
    open(&s, 0);
    let samples = [0.25f32; 64];
    let planes = plane(&samples);
    let initial = [live::Entry {
        element_id: 1,
        state: complete_state(),
    }];
    assert_eq!(
        s.submit_frame(&frame(&planes, 0, 64, &initial, &[]), 0)
            .unwrap(),
        Submit::Accepted
    );
    s.wait_idle(5000).unwrap();
    let mut buffer = vec![0f32; 256];
    let p = s.pull(&mut buffer, 128);
    assert_eq!(p.media_frames, 64);
    assert_eq!(p.flags, pull_flag::UNDERRUN);
    assert_eq!(s.status().underruns, 1);
}

#[test]
fn the_input_budget_blocks_until_the_worker_frees_it() {
    let mut c = config(48000, 16);
    c.input_queue_samples = 1024;
    let s = stream(48000, &c);
    open(&s, 0);
    let samples = vec![0.25f32; 512];
    let planes = plane(&samples);
    let initial = [live::Entry {
        element_id: 1,
        state: complete_state(),
    }];
    let mut start = 0;
    let mut blocked = false;
    for _ in 0..16 {
        let view = frame(
            &planes,
            start,
            512,
            if start == 0 { &initial } else { &[] },
            &[],
        );
        match s.submit_frame(&view, 0).unwrap() {
            Submit::Accepted => start += 512,
            Submit::WouldBlock => {
                blocked = true;
                break;
            }
            other => panic!("{other:?}"),
        }
    }
    assert!(blocked, "the ring and queue never filled");
    let view = frame(&planes, start, 512, &[], &[]);
    assert_eq!(s.submit_frame(&view, 10).unwrap(), Submit::TimedOut);
    assert!(s.status().queued_input_samples > 0);
    let mut buffer = vec![0f32; 2 * 64];
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let _ = s.pull(&mut buffer, 64);
        if s.submit_frame(&view, 1).unwrap() == Submit::Accepted {
            break;
        }
        assert!(Instant::now() < deadline, "the budget was never released");
    }
}

#[test]
fn a_fence_behind_a_full_ring_times_out_and_a_new_epoch_recovers() {
    let s = stream(48000, &config(48000, 16));
    open(&s, 0);
    let samples = [0.125f32; 64];
    let planes = plane(&samples);
    let initial = [live::Entry {
        element_id: 1,
        state: complete_state(),
    }];
    assert_eq!(
        s.submit_frame(&frame(&planes, 0, 64, &initial, &[]), 0)
            .unwrap(),
        Submit::Accepted
    );
    assert_eq!(rejected(s.wait_idle(10)), "Scene replay fence timed out");
    assert!(!s.status().failed);
    s.begin_epoch(2, 0).unwrap();
    s.wait_idle(5000).unwrap();
    let status = s.status();
    assert_eq!((status.epoch, status.state), (2, State::Buffering));
    assert_eq!(status.queued_input_samples, 0);
    let mut buffer = [1f32; 8];
    let p = s.pull(&mut buffer, 4);
    assert_eq!(
        (p.epoch, p.flags, p.media_frames),
        (2, pull_flag::BUFFERING, 0)
    );
    assert_eq!(buffer, [0.; 8]);
}

#[test]
fn a_reset_while_the_end_is_padded_returns() {
    // 48 -> 44.1 kHz behind a target pads one frame at the end: 2296 resampled frames of the
    // 2297 the timeline allows. A ring of 2296 frames that nobody drains leaves the worker padding;
    // a new epoch must still be acknowledged (the C++ padding loop never noticed the reset).
    let s = Arc::new(stream(48000, &config(44100, 2296)));
    open(&s, 1500);
    feed(&s, 0, 4000, 1024);
    assert_eq!(settled(&s), 2296);
    assert!(!s.status().production_complete);
    let (sender, receiver) = mpsc::channel();
    let stream = Arc::clone(&s);
    std::thread::spawn(move || {
        let _ = sender.send(stream.begin_epoch(2, 0).is_ok());
    });
    assert_eq!(receiver.recv_timeout(Duration::from_secs(10)), Ok(true));
}

#[test]
fn unmatched_policy_rules_are_semantic_degradations() {
    let s = stream(48000, &config(48000, 1024));
    s.set_semantic_policy_json(
        br#"{"schema":"mradm.semantic-policy.v1","objects":[{"name_glob":"*nothing*","gain":{"mute":true}}]}"#,
        7,
    )
    .unwrap();
    open(&s, 0);
    feed(&s, 0, 1024, 1024);
    assert_eq!(drain(&s), 1024);
    let status = s.status();
    assert_eq!(status.semantic_policy_revision, 7);
    assert_eq!(status.semantic_degradations, 1);
    let d = s.diagnostic(0).unwrap();
    assert_eq!((d.level, d.code, d.epoch, d.generation), (2, 1, 1, 1));
    assert_eq!(
        String::from_utf8(d.message).unwrap(),
        "semantic policy objects[0] did not match this Scene generation"
    );
    assert_eq!(s.diagnostic_count(), 1);
}

#[test]
fn policy_errors_keep_the_former_message_buffer() {
    let s = stream(48000, &config(48000, 64));
    let key = "k".repeat(2000);
    let json = format!(r#"{{"schema":"mradm.semantic-policy.v1","{key}":1}}"#);
    let e = s
        .set_semantic_policy_json(json.as_bytes(), 1)
        .expect_err("unknown key");
    assert_eq!(e.code, 1);
    assert!(e.message.len() <= 1023, "{}", e.message.len());
    assert!(text(&e).contains("kkkk"));
}

#[test]
fn a_backend_switch_keeps_the_timeline() {
    let s = stream(48000, &config(48000, 1024));
    open(&s, 0);
    s.switch_backend(&renderer_config(Selection::SafBinaural, b"binaural", 48000))
        .unwrap();
    feed(&s, 0, 6000, 512);
    assert_eq!(drain(&s), 6000);
    assert!(!s.status().failed);
}

/// The frames the worker has buffered once it stops pushing.
fn settled(s: &Stream) -> u64 {
    let mut last = u64::MAX;
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        std::thread::sleep(Duration::from_millis(30));
        let buffered = s.status().buffered_output_frames;
        if buffered == last {
            return buffered;
        }
        assert!(Instant::now() < deadline);
        last = buffered;
    }
}

#[test]
fn recent_head_tracking_caps_the_lookahead_on_the_manual_clock() {
    let mut c = config(48000, 8192);
    c.manual_clock = Some(1_000_000_000);
    let s = stream(48000, &c);
    open(&s, 0);
    s.set_listener_orientation([10., 0., 0.]);
    let samples = vec![0.25f32; 4096];
    let planes = plane(&samples);
    let initial = [live::Entry {
        element_id: 1,
        state: complete_state(),
    }];
    assert_eq!(
        s.submit_frame(&frame(&planes, 0, 4096, &initial, &[]), 0)
            .unwrap(),
        Submit::Accepted
    );
    assert_eq!(settled(&s), 2048);
    // 750 ms later tracking has expired: the whole ring is the lookahead again.
    s.set_clock(1_750_000_000);
    assert_eq!(settled(&s), 4096);
}
