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

#[test]
fn conversion_chunks_do_not_present_the_tail_until_the_next_device_callback() {
    let (_s, output) = session(true, false, 512, 0.25);
    let mut chunk = [0.0; 2048];
    for begins_callback in [true, false] {
        assert!(
            output
                .pull_in_callback(&mut chunk, 1024, begins_callback)
                .eos
        );
        output.release();
    }
    let status = output.status(progress());
    assert_eq!(
        (
            status.state,
            status.consumed_frames,
            status.presented_frames
        ),
        (State::Draining, 512, 0)
    );
    output.pull(&mut chunk, 1024);
    output.release();
    let status = output.status(progress());
    assert_eq!((status.state, status.presented_frames), (State::Ended, 512));
}

// ── R3 D2: the remaining checks of the C++ `stereo_peak_guard_test` ───────────────────────────

/// A stream over `selection` / `layout` at 48 kHz with a one-frame startup watermark.
fn stream_of(selection: Selection, layout: &'static [u8], ring: u32) -> Arc<Stream> {
    let renderer = renderer::Config {
        selection,
        layout_id: layout,
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
        output_ring_frames: ring,
        startup_watermark_frames: 1,
        ..Config::default()
    };
    Arc::new(Stream::create(&config, &renderer, Arc::new(TestHost)).unwrap())
}

/// Opens `epoch` with one element of `role` (0 object at x = 0.25, y = 0.75; 2 LFE) and submits
/// `samples` from media frame `start`.
fn submit(s: &Stream, epoch: u64, role: u32, start: i64, samples: &[f32], configure: bool) {
    if configure {
        s.configure_generation(
            epoch,
            1,
            &[Descriptor {
                element_id: 1,
                role,
                speaker_label: Vec::new(),
                has_position: false,
                position: [0., 1., 0.],
                flags: 0,
                identity: None,
            }],
        )
        .unwrap();
    }
    let planes = [PlaneView {
        element_id: 1,
        sample_count: samples.len() as u32,
        stride: 1,
        has_signal: true,
        samples: Some(samples),
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
        start,
        duration: samples.len() as u32,
        flags: frame_flag::STATE_COMPLETE,
        planes: &planes[..],
        initial: if start == 0 { &initial[..] } else { &[] },
        updates: &[],
    };
    assert_eq!(s.submit_frame(&frame, 5000).unwrap(), Submit::Accepted);
}

/// The C++ fixtures' LCG noise: `amplitude * (seed >> 8) / 2^23 - amplitude`.
fn noise(frames: usize, mut seed: u32, amplitude: f32) -> Vec<f32> {
    (0..frames)
        .map(|_| {
            seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            amplitude * ((seed >> 8) as f32 / 8_388_608. - 1.)
        })
        .collect()
}

/// Designs `text` warn-only at 48 kHz.
fn design(text: &[u8]) -> Coefficients {
    let profile = mradm_dsp::hptf::parse_parametric_eq(text).unwrap();
    mradm_dsp::hptf::design(
        &profile.bands,
        profile.preamp_db,
        RATE,
        mradm_dsp::hptf::PreampMode::WarnOnly,
    )
    .unwrap()
}

fn low_boost() -> Coefficients {
    design(b"Preamp: 0 dB\nFilter 1: ON LSC Fc 200 Hz Gain 12 dB Q 0.7\n")
}

/// One active pull of up to `frames`: the media it returned.
fn pull(out: &Session, frames: usize) -> Vec<f32> {
    let mut buffer = vec![0f32; frames * 2];
    let p = out.pull(&mut buffer, frames);
    if p.active {
        out.release();
    }
    buffer.truncate(p.frames * 2);
    buffer
}

#[test]
fn the_guard_keeps_the_media_length_and_a_new_epoch_drops_the_old_lookahead() {
    let s = stream_of(Selection::SafBinaural, b"binaural", 1 << 16);
    let out = Session::new(s.clone(), true, false).unwrap();
    out.set_volume(0.8).unwrap();
    // Epoch 2 is cut after 960 frames with loud lookahead still buffered; epoch 3 must not
    // carry any of it.
    for (epoch, frames, level, cut) in [
        (1u64, 4417usize, 3.0f32, None),
        (2, 4417, 3.0, Some(960)),
        (3, 17, 0.1, None),
    ] {
        out.check_epoch(epoch).unwrap();
        out.park();
        out.reset_epoch(epoch, 0).unwrap();
        submit(&s, epoch, 2, 0, &vec![level; frames], true);
        s.signal_end(epoch, frames as i64).unwrap();
        out.play();
        let mut count = 0;
        let mut checked_pause = false;
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while !out.status(progress()).ended {
            assert!(
                std::time::Instant::now() < deadline,
                "epoch {epoch} timed out"
            );
            if cut.is_some_and(|n| count >= n) {
                break;
            }
            let media = pull(&out, 480);
            count += media.len() / 2;
            for &x in &media {
                assert!(x.abs() <= CEILING + 1e-6, "protected after the volume");
                if epoch == 3 {
                    assert!(
                        x.abs() <= 0.080001,
                        "epoch 3 holds no earlier lookahead: {x}"
                    );
                }
            }
            if epoch == 1 && count >= 480 && !checked_pause {
                out.park();
                assert!(pull(&out, 480).is_empty());
                assert_eq!(out.status(progress()).consumed_frames, count as u64);
                out.play();
                checked_pause = true;
            }
            if media.is_empty() {
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
        }
        if cut.is_some() {
            continue;
        }
        let status = out.status(progress());
        assert_eq!(count, frames, "epoch {epoch}");
        assert_eq!(
            (status.consumed_frames, status.presented_frames),
            (frames as u64, frames as u64)
        );
    }
}

#[test]
fn an_unprotected_session_scales_unbounded_pcm_exactly() {
    let render = |volume: f32| {
        let s = stream_of(Selection::SafBinaural, b"binaural", 1 << 16);
        let out = Session::new(s.clone(), false, false).unwrap();
        out.set_volume(volume).unwrap();
        out.check_epoch(1).unwrap();
        out.reset_epoch(1, 0).unwrap();
        let samples: Vec<f32> = (0..1531).map(|i| (i as f32 * 0.01).sin() * 1.5).collect();
        submit(&s, 1, 2, 0, &samples, true);
        s.signal_end(1, 1531).unwrap();
        out.play();
        let mut pcm = Vec::new();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while !out.status(progress()).ended {
            assert!(std::time::Instant::now() < deadline);
            let media = pull(&out, 127);
            if media.is_empty() {
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
            pcm.extend(media);
        }
        assert_eq!(pcm.len(), 1531 * 2);
        pcm
    };
    let unity = render(1.);
    let scaled = render(0.375);
    assert!(
        unity.iter().any(|x| x.abs() > 1.),
        "the PCM exceeds full scale"
    );
    assert!(unity.iter().zip(&scaled).all(|(&u, &v)| v == u * 0.375));
}

#[test]
fn the_epoch_reset_applies_hptf_metadata_and_a_pending_bypass() {
    let s = stream();
    let out = Session::new(s, true, false).unwrap();
    out.set_hptf(Some(design(b"Preamp: -6 dB\n")), 9).unwrap();
    let pending = out.hptf_snapshot();
    let c = &pending.coefficients[0];
    assert_eq!(
        (pending.revision, c.band_count, c.preamp_gain, c.preamp_db),
        (0, 0, 1., 0.)
    );
    out.check_epoch(1).unwrap();
    out.reset_epoch(1, 0).unwrap();
    let applied = out.hptf_snapshot();
    let c = &applied.coefficients[0];
    assert_eq!(
        (applied.revision, c.band_count, c.preamp_db, c.sample_rate),
        (9, 0, -6., RATE)
    );
    assert!(c.preamp_gain < 1., "the preamp-only profile is active");
    out.set_hptf(None, 10).unwrap();
    out.check_epoch(2).unwrap();
    out.reset_epoch(2, 0).unwrap();
    let bypass = out.hptf_snapshot();
    let c = &bypass.coefficients[0];
    assert_eq!(
        (bypass.revision, c.band_count, c.preamp_gain, c.preamp_db),
        (10, 0, 1., 0.)
    );
}

/// 12000 frames of noise at 3.0 with volume and HpTF changes at fixed media frames, pulled in
/// `sizes` (cycled, cut at each change); the PCM and the applied revision after every pull.
fn partitioned(sizes: &[usize]) -> (Vec<f32>, Vec<u64>) {
    const FRAMES: usize = 12000;
    let s = stream_of(Selection::Saf, b"0+2+0", 16384);
    let out = Session::new(s.clone(), true, false).unwrap();
    out.check_epoch(1).unwrap();
    out.reset_epoch(1, 0).unwrap();
    submit(&s, 1, 0, 0, &noise(FRAMES, 0x2468_ACE1, 3.), true);
    s.signal_end(1, FRAMES as i64).unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
    while !s.status().production_complete {
        assert!(std::time::Instant::now() < deadline, "production timed out");
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    let controls = [0usize, 2048, 2600, 3000, 7000, 8192];
    out.play();
    let (mut pcm, mut revisions) = (Vec::new(), Vec::new());
    let (mut step, mut control, mut short) = (0, 0, false);
    while !out.status(progress()).ended {
        let consumed = pcm.len() / 2;
        if control < controls.len() && consumed == controls[control] {
            match control {
                0 => out.set_volume(0.875).unwrap(),
                1 => out.set_hptf(Some(low_boost()), 1).unwrap(),
                2 => out
                    .set_hptf(
                        Some(design(
                            b"Preamp: -3 dB\nFilter 1: ON PK Fc 1700 Hz Gain 6 dB Q 0.75\n",
                        )),
                        2,
                    )
                    .unwrap(),
                3 => out
                    .set_hptf(
                        Some(design(
                            b"Preamp: -6 dB\nFilter 1: ON HSC Fc 4200 Hz Gain -4 dB Q 0.75\n",
                        )),
                        3,
                    )
                    .unwrap(),
                4 => out.set_hptf(None, 4).unwrap(),
                _ => out.set_volume(0.5).unwrap(),
            }
            control += 1;
        }
        let mut count = sizes[step % sizes.len()];
        step += 1;
        if control < controls.len() {
            count = count.min(controls[control] - consumed);
        }
        let media = pull(&out, count);
        assert!(!short || media.is_empty(), "media after a short pull");
        short |= media.len() / 2 != count;
        pcm.extend(media);
        revisions.push(out.hptf_snapshot().revision);
    }
    assert_eq!(pcm.len(), FRAMES * 2);
    (pcm, revisions)
}

#[test]
fn device_output_is_independent_of_the_pull_partition() {
    let (reference, revisions) = partitioned(&[512]);
    assert_eq!(revisions.last(), Some(&4), "the bypass applies last");
    assert!(
        !revisions.contains(&2),
        "a replaced queued target never applies"
    );
    assert!(
        reference.iter().any(|x| x.abs() > 0.5),
        "the guard is exercised"
    );
    for sizes in [&[1, 7, 127, 511, 1024][..], &[4096], &[3, 1021], &[960]] {
        let (pcm, revisions) = partitioned(sizes);
        let same = pcm
            .iter()
            .zip(&reference)
            .all(|(a, b)| a.to_bits() == b.to_bits());
        assert!(same, "partition {sizes:?}");
        assert!(!revisions.contains(&2), "partition {sizes:?}");
    }
}

/// 12000 frames of noise through HpTF and volume; `starve` stops the feed half way until the
/// device has underrun three times.
fn underrun(starve: bool) -> Vec<f32> {
    const FRAMES: usize = 12000;
    const SPLIT: usize = 6000;
    let s = stream_of(Selection::Saf, b"0+2+0", 16384);
    let out = Session::new(s.clone(), true, false).unwrap();
    out.set_hptf(Some(low_boost()), 1).unwrap();
    out.set_volume(0.875).unwrap();
    out.check_epoch(1).unwrap();
    out.reset_epoch(1, 0).unwrap();
    let samples = noise(FRAMES, 0x1357_9BDF, 3.);
    let feed = |start: usize, end: usize| {
        let configure = start == 0;
        let s = &s;
        // The C++ fixture placed this source at x = -0.5, y = 0.5.
        if configure {
            s.configure_generation(
                1,
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
        }
        let planes = [PlaneView {
            element_id: 1,
            sample_count: (end - start) as u32,
            stride: 1,
            has_signal: true,
            samples: Some(&samples[start..end]),
        }];
        let initial = [live::Entry {
            element_id: 1,
            state: live::State {
                valid_fields: field::KNOWN,
                position: [-0.5, 0.5, 0.],
                ..live::State::default()
            },
        }];
        let frame = FrameInput {
            epoch: 1,
            generation: 1,
            start: start as i64,
            duration: (end - start) as u32,
            flags: frame_flag::STATE_COMPLETE,
            planes: &planes[..],
            initial: if configure { &initial[..] } else { &[] },
            updates: &[],
        };
        assert_eq!(s.submit_frame(&frame, 5000).unwrap(), Submit::Accepted);
        s.wait_idle(20000).unwrap();
    };
    out.play();
    feed(0, if starve { SPLIT } else { FRAMES });
    let mut media = Vec::new();
    if starve {
        loop {
            let got = pull(&out, 512);
            let full = got.len() == 1024;
            media.extend(got);
            if !full {
                break;
            }
        }
        let before = out.status(progress()).underruns;
        for _ in 0..3 {
            assert!(pull(&out, 512).is_empty(), "an underrun carries no media");
        }
        assert!(out.status(progress()).underruns > before);
        feed(SPLIT, FRAMES);
    }
    s.signal_end(1, FRAMES as i64).unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
    while !out.status(progress()).ended {
        let got = pull(&out, 512);
        if got.is_empty() {
            assert!(std::time::Instant::now() < deadline, "no end of stream");
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        media.extend(got);
    }
    assert_eq!(media.len(), FRAMES * 2);
    media
}

#[test]
fn an_underrun_leaves_the_media_unchanged() {
    let reference = underrun(false);
    let starved = underrun(true);
    assert!(
        starved
            .iter()
            .zip(&reference)
            .all(|(a, b)| a.to_bits() == b.to_bits())
    );
}

#[test]
fn idle_streams_are_created_and_dropped_concurrently() {
    let threads: Vec<_> = (0..4)
        .map(|_| {
            std::thread::spawn(|| {
                for _ in 0..128 {
                    let s = stream();
                    std::thread::yield_now();
                    drop(s);
                }
            })
        })
        .collect();
    for t in threads {
        t.join().unwrap();
    }
}
