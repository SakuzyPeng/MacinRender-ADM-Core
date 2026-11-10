//! Fences racing an epoch reset. Inspect the private queue only to ensure all old fences have
//! entered before resetting; completion and cancellation use the normal stream entry points.
use super::*;
use crate::backend::{BinauralSpreadMode, RoomRequest, Screens, SpeakerSpread};
use crate::test_common::backend::TestHost;
use mradm_dsp::room::screen::RoomScreenGeometry;

fn stream() -> Arc<Stream> {
    Arc::new(
        Stream::create(
            &Config {
                output_sample_rate: 48000,
                output_ring_frames: 16,
                startup_watermark_frames: 1,
                ..Config::default()
            },
            &renderer::Config {
                selection: Selection::Saf,
                layout_id: b"0+2+0",
                apple_geometry: false,
                speaker_spread: SpeakerSpread::Automatic,
                binaural_spread: BinauralSpreadMode::Automatic,
                split_power: false,
                smoothing_frames: 0,
                sample_rate: 48000,
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
            },
            Arc::new(TestHost),
        )
        .unwrap(),
    )
}

fn fill_ring(s: &Stream, epoch: u64) {
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
    let samples = [0.125f32; 64];
    let planes = [PlaneView {
        element_id: 1,
        sample_count: 64,
        stride: 1,
        has_signal: true,
        samples: Some(&samples),
    }];
    let initial = [live::Entry {
        element_id: 1,
        state: live::State {
            valid_fields: field::KNOWN,
            position: [0.25, 0.75, 0.],
            ..live::State::default()
        },
    }];
    assert_eq!(
        s.submit_frame(
            &FrameInput {
                epoch,
                generation: 1,
                start: 0,
                duration: 64,
                flags: frame_flag::STATE_COMPLETE,
                planes: &planes[..],
                initial: &initial,
                updates: &[],
            },
            5000,
        )
        .unwrap(),
        Submit::Accepted,
    );
}

#[test]
fn new_epoch_fences_cannot_complete_cancelled_fences() {
    const WAITERS: usize = 64;
    let s = stream();
    s.begin_epoch(1, 0).unwrap();
    for epoch in 1..=16 {
        fill_ring(&s, epoch);
        let waiters: Vec<_> = (0..WAITERS)
            .map(|_| {
                let stream = Arc::clone(&s);
                std::thread::spawn(move || stream.wait_idle(10_000))
            })
            .collect();
        let deadline = Instant::now() + Duration::from_secs(10);
        while s
            .shared
            .lock()
            .queue
            .iter()
            .filter(|item| item.kind == Kind::Fence)
            .count()
            != WAITERS
        {
            assert!(
                Instant::now() < deadline,
                "not all fences entered the queue"
            );
            std::thread::yield_now();
        }
        let next = Arc::clone(&s);
        let next_fence = std::thread::spawn(move || {
            let deadline = Instant::now() + Duration::from_secs(10);
            while next.status().epoch != epoch + 1 {
                assert!(Instant::now() < deadline, "the epoch did not advance");
                std::thread::yield_now();
            }
            next.wait_idle(10_000).unwrap();
        });
        s.begin_epoch(epoch + 1, 0).unwrap();
        next_fence.join().unwrap();
        for waiter in waiters {
            let err = waiter
                .join()
                .unwrap()
                .expect_err("discarded work completed");
            assert_eq!(
                err.message,
                b"Scene replay fence was cancelled by a new epoch",
            );
        }
        s.wait_idle(5000).unwrap();
    }
}
