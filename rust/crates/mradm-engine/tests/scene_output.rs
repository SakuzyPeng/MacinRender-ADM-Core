//! The Scene output session (PCM consistency 5.4 C): the C++ `SceneOutputSession` rules for the
//! callback pull, the stereo feed's prefetch, HpTF ahead of the sample-peak guard, volume, epochs,
//! the status priority and the error texts.
mod common;
use common::backend::TestHost;
use mradm_dsp::hptf::{BLEND_FRAMES, Coefficients};
use mradm_dsp::peak_guard::CEILING;
use mradm_dsp::room::screen::RoomScreenGeometry;
use mradm_engine::backend::{BinauralSpreadMode, RoomRequest, Screens, SpeakerSpread};
use mradm_engine::request::Selection;
use mradm_engine::scene::output::{DeviceProgress, Session, State};
use mradm_engine::scene::renderer;
use mradm_engine::scene::stream::{
    Config, Descriptor, FrameInput, PlaneView, Stream, Submit, frame_flag,
};
use mradm_render::live::{self, field};
use mradm_render::routing::Error;
use std::sync::Arc;

const RATE: u32 = 48000;

fn stream() -> Arc<Stream> {
    let renderer = renderer::Config {
        selection: Selection::Saf,
        layout_id: b"0+2+0",
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
        sink: true,
    };
    let config = Config {
        output_sample_rate: RATE,
        output_ring_frames: 1 << 16,
        startup_watermark_frames: 1,
        ..Config::default()
    };
    Arc::new(Stream::create(&config, &renderer, Arc::new(TestHost)).unwrap())
}

fn text(e: &Error) -> String {
    String::from_utf8_lossy(&e.message).into_owned()
}

fn rejected<T>(result: Result<T, Error>) -> (i32, String) {
    match result {
        Ok(_) => panic!("accepted"),
        Err(e) => (e.code, text(&e)),
    }
}

/// Opens epoch `epoch` and submits `total` frames of `amplitude`; `end` also ends the stream.
/// Waits until everything submitted is rendered into the ring.
fn feed(s: &Stream, epoch: u64, total: u32, amplitude: f32, end: bool) {
    s.configure_generation(
        epoch,
        1,
        &[Descriptor {
            element_id: 1,
            role: 0,
            speaker_label: Vec::new(),
            has_position: false,
            position: [0., 1., 0.],
            flags: 0,
            identity: None,
        }],
    )
    .unwrap();
    let signal: Vec<f32> = (0..total)
        .map(|i| (i as f32 * 0.05).sin() * amplitude)
        .collect();
    let planes = [PlaneView {
        element_id: 1,
        sample_count: total,
        stride: 1,
        has_signal: true,
        samples: Some(&signal),
    }];
    let initial = [live::Entry {
        element_id: 1,
        state: live::State {
            valid_fields: field::KNOWN,
            position: [0.25, 0.75, 0.],
            ..live::State::default()
        },
    }];
    let frame = FrameInput {
        epoch,
        generation: 1,
        start: 0,
        duration: total,
        flags: frame_flag::STATE_COMPLETE,
        planes: &planes[..],
        initial: &initial,
        updates: &[],
    };
    assert_eq!(s.submit_frame(&frame, 5000).unwrap(), Submit::Accepted);
    if end {
        s.signal_end(epoch, i64::from(total)).unwrap();
    }
    s.wait_idle(10000).unwrap();
}

/// A started epoch-1 session over a stream of `total` frames.
fn session(
    protect: bool,
    device_volume: bool,
    total: u32,
    amplitude: f32,
) -> (Arc<Stream>, Session) {
    let s = stream();
    let out = Session::new(s.clone(), protect, device_volume).unwrap();
    out.check_epoch(1).unwrap();
    out.park();
    out.reset_epoch(1, 0).unwrap();
    feed(&s, 1, total, amplitude, true);
    out.play();
    (s, out)
}

/// Pulls blocks of `block` frames until the end of stream; returns the media PCM.
fn drain(out: &Session, block: usize) -> Vec<f32> {
    let mut pcm = Vec::new();
    let mut buffer = vec![0f32; block * 2];
    for _ in 0..10_000 {
        let p = out.pull(&mut buffer, block);
        assert!(p.active);
        out.release();
        pcm.extend_from_slice(&buffer[..p.frames * 2]);
        if p.eos {
            return pcm;
        }
    }
    panic!("no end of stream");
}

fn progress() -> DeviceProgress {
    DeviceProgress::default()
}

#[test]
fn errors_keep_their_codes_and_texts() {
    let s = stream();
    let out = Session::new(s.clone(), false, false).unwrap();
    assert_eq!(
        rejected(Session::new(s.clone(), true, false)),
        (1, "Scene stream already has an output consumer".into())
    );
    assert_eq!(
        rejected(out.set_volume(1.5)),
        (1, "Scene output volume must be finite and in [0, 1]".into())
    );
    assert_eq!(
        rejected(out.set_volume(f32::NAN)),
        (1, "Scene output volume must be finite and in [0, 1]".into())
    );
    assert_eq!(
        rejected(out.set_hptf(None, 1)),
        (
            2,
            "耳机补偿(HpTF)只适用于立体声耳机输出;系统空间音频与多声道输出不支持".into()
        )
    );
    assert_eq!(
        rejected(out.check_hptf_settings()),
        (2, "耳机补偿(HpTF)只适用于立体声耳机输出".into())
    );
    assert!(!out.hptf_supported());
    assert_eq!(out.hptf_snapshot().revision, 0);
    out.check_epoch(1).unwrap();
    out.reset_epoch(1, 0).unwrap();
    assert_eq!(
        rejected(out.check_epoch(1)),
        (1, "Scene output epoch must increase".into())
    );
    drop(out);
    // Dropping the session detaches it.
    Session::new(s, true, false).unwrap();
}

#[test]
fn a_parked_session_pulls_silence() {
    let (_s, out) = session(true, false, 4800, 0.25);
    out.park();
    let mut buffer = vec![1f32; 512];
    let p = out.pull(&mut buffer, 256);
    assert!(!p.active && p.frames == 0 && !p.eos);
    assert!(buffer.iter().all(|&x| x == 0.));
    assert_eq!(out.status(progress()).state, State::Paused);
}

#[test]
fn the_stereo_feed_drains_the_whole_stream_to_its_end() {
    for block in [1usize, 37, 256, 5000] {
        let (_s, out) = session(true, false, 4800, 0.25);
        let pcm = drain(&out, block);
        assert_eq!(pcm.len(), 4800 * 2, "block {block}");
        let status = out.status(progress());
        assert_eq!(status.consumed_frames, 4800);
        assert_eq!(
            status.state,
            State::Draining,
            "the last pull is not presented yet"
        );
        let mut buffer = vec![0f32; 2];
        let p = out.pull(&mut buffer, 1);
        out.release();
        assert!(p.eos && p.frames == 0);
        assert_eq!(out.status(progress()).state, State::Ended);
    }
}

#[test]
fn stereo_prefetch_reads_only_available_pcm() {
    let s = stream();
    let out = Session::new(s.clone(), true, false).unwrap();
    out.check_epoch(1).unwrap();
    out.reset_epoch(1, 0).unwrap();
    feed(&s, 1, 1000, 0.25, false);
    out.play();
    let mut buffer = vec![0f32; 2 * 4096];
    let p = out.pull(&mut buffer, 4096);
    out.release();
    assert!(!p.eos);
    // Only the one-frame probe of the empty ring counts as an underrun.
    assert_eq!(s.status().underruns, 1);
    assert_eq!(out.status(progress()).state, State::Buffering);
}

#[test]
fn volume_applies_once_on_each_path() {
    let pull = |protect: bool, device: bool, volume: f32| {
        let (_s, out) = session(protect, device, 2400, 0.25);
        out.set_volume(volume).unwrap();
        drain(&out, 480)
    };
    let full = pull(false, false, 1.0);
    let half = pull(false, false, 0.5);
    let device = pull(false, true, 0.5);
    assert!(full.iter().any(|&x| x != 0.));
    assert!(full.iter().zip(&half).all(|(&a, &b)| b == a * 0.5));
    assert_eq!(
        full, device,
        "a device with its own volume gets unscaled PCM"
    );
    let guarded = pull(true, false, 0.5);
    assert_eq!(guarded.len(), full.len());
    assert!(
        guarded
            .iter()
            .zip(&full)
            .all(|(&g, &f)| (g - f * 0.5).abs() < 1e-6)
    );
}

fn gain(preamp_gain: f32) -> Coefficients {
    Coefficients {
        sample_rate: RATE,
        preamp_gain,
        ..Coefficients::default()
    }
}

#[test]
fn hptf_runs_ahead_of_the_peak_guard() {
    let (_s, out) = session(true, false, 9600, 0.6);
    out.set_hptf(Some(gain(4.0)), 7).unwrap();
    let pcm = drain(&out, 512);
    assert!(
        pcm.iter().all(|&x| x.abs() <= CEILING + 1e-6),
        "the guard caught the boost"
    );
    assert!(
        pcm.iter().any(|&x| x.abs() > 0.8),
        "the boost reached the guard"
    );
    assert_eq!(out.hptf_snapshot().revision, 7);
}

#[test]
fn hptf_targets_blend_in_and_epochs_adopt_the_newest() {
    let (_s, out) = session(true, false, 9600, 0.25);
    let plain = {
        let (_s, reference) = session(true, false, 9600, 0.25);
        drain(&reference, 512)
    };
    out.set_hptf(Some(gain(0.5)), 3).unwrap();
    let pcm = drain(&out, 512);
    assert_eq!(out.hptf_snapshot().revision, 3);
    let steady = BLEND_FRAMES * 2 + 512;
    assert!(
        pcm[steady..]
            .iter()
            .zip(&plain[steady..])
            .all(|(&a, &b)| (a - b * 0.5).abs() < 1e-6)
    );
    // A target published while parked is adopted by the epoch reset, with no pull.
    out.set_hptf(Some(gain(0.25)), 4).unwrap();
    out.check_epoch(2).unwrap();
    out.park();
    out.reset_epoch(2, 0).unwrap();
    assert_eq!(out.hptf_snapshot().revision, 4);
    assert_eq!(out.status(progress()).consumed_frames, 0);
}

#[test]
fn status_follows_the_former_priority() {
    let (_s, out) = session(true, false, 2400, 0.25);
    assert_eq!(out.status(progress()).state, State::Buffering);
    let mut buffer = vec![0f32; 2 * 4096];
    let p = out.pull(&mut buffer, 4096);
    out.release();
    assert!(p.eos);
    assert_eq!(out.status(progress()).state, State::Draining);
    out.park();
    assert_eq!(
        out.status(progress()).state,
        State::Paused,
        "paused outranks draining"
    );
    let failed = DeviceProgress {
        failed: true,
        ..progress()
    };
    assert_eq!(out.status(failed).state, State::Failed);
    let media = DeviceProgress {
        media_clock: true,
        presented_frames: 2400,
        underruns: 9,
        ..progress()
    };
    let status = out.status(media);
    assert_eq!(
        (status.state, status.queued_frames, status.underruns),
        (State::Ended, 0, 9)
    );
}
