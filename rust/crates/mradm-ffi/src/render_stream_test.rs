//! Control queries must remain independent of an exclusive worker borrow, including under Miri.
use super::test_common as common;

use super::*;
use std::sync::{
    Barrier,
    atomic::{AtomicPtr, Ordering},
};

const FRAMES: usize = 4096;

struct TestStream {
    handle: *mut RenderTripleBalanceStream,
    _input: common::Temp,
}

impl TestStream {
    fn new() -> Self {
        let input = common::input(&common::sine(FRAMES, 0.5));
        let mut block = common::object_block(0, u64::MAX, 0.);
        block.position.cartesian = 1;
        block.position.y = 1.;
        (block.width, block.height, block.depth) = (0.25, 0.25, 0.25);
        let scene = common::scene(
            common::single(
                "AO_1001",
                1.,
                common::TrackSpec {
                    blocks: vec![block],
                    direct: Vec::new(),
                },
            ),
            FRAMES as u64,
        );
        let p = triple_balance::prepare(
            &triple_balance::Request {
                scene: &scene,
                layout_id: b"9.1.6",
                mode: triple_balance::Mode::Standard,
                spread: triple_balance::Spread::Automatic,
                split_power: false,
                standard_geometry: true,
                smoothing_frames: 0,
                routing: triple_balance::Routing::Automatic,
                has_matrix: false,
                input_path: b"input.wav",
            },
            &mut |_, _| {},
        )
        .result
        .unwrap();
        let (mix, session) = triple_balance_stream::Source::of(&p.plan).unwrap();
        let source = triple_balance_stream::Source {
            mix,
            session,
            bindings: p.bindings.into(),
            size_tracks: p.size_tracks.into(),
            spread_none: false,
            inputs: 1,
            outputs: p.outputs,
            frames: FRAMES as u64,
            sample_rate: common::RATE,
        };
        let stream =
            triple_balance_stream::Stream::open(input.path(), source.clone(), 5, 0).unwrap();
        Self {
            handle: Box::into_raw(Box::new(RenderTripleBalanceStream { source, stream })),
            _input: input,
        }
    }
}

impl Drop for TestStream {
    fn drop(&mut self) {
        // All worker and control calls have returned before the owning test drops this handle.
        unsafe { mradm_render_triple_balance_stream_destroy(self.handle) };
    }
}

fn check_control(handle: *const RenderTripleBalanceStream) {
    let mut edit = RenderTripleBalanceOverride {
        base: RenderOverride {
            object_id: Text::bytes(b"AO_1001"),
            speaker_label: Text::bytes(b""),
            mute: 0,
            gain_db: -6.,
            has_head_locked: 0,
            head_locked: 0,
        },
        extent_scale: 0.5,
        extent_width_scale: 1.,
        extent_height_scale: 1.,
        extent_depth_scale: 1.,
        diffuse_scale: 1.,
        divergence_scale: 1.,
    };
    let mut message = [0u8; 128];
    let mut context = [0u8; 32];
    for expected in [0, 2] {
        let (mut message_len, mut context_len) = (message.len(), context.len());
        let code = unsafe {
            mradm_render_triple_balance_stream_validate(
                handle,
                &edit,
                1,
                message.as_mut_ptr(),
                &mut message_len,
                context.as_mut_ptr(),
                &mut context_len,
            )
        };
        assert_eq!(code, expected);
        if expected == 0 {
            assert_eq!((message_len, context_len), (0, 0));
        } else {
            assert_eq!(
                &message[..message_len],
                b"Triple Balance live editing supports gain, mute and linked isotropic size only"
            );
            assert_eq!(&context[..context_len], b"AO_1001");
        }
        edit.extent_height_scale = 2.;
    }
    assert_eq!(
        unsafe { mradm_render_triple_balance_stream_requires_preroll(handle) },
        1
    );
}

fn render_seeks(worker: &mut triple_balance_stream::Stream) -> Vec<u32> {
    let mut bits = Vec::new();
    for start in [0, 1023, 2049, 17] {
        let mut pending = worker.seek_begin(start).unwrap();
        while pending {
            pending = worker.seek_step().unwrap();
        }
        worker.seek_finish().unwrap();
        for frames in [7, 333, 512] {
            let mut output = vec![0.; frames * worker.outputs()];
            assert_eq!(worker.process(&mut output, frames).unwrap(), frames);
            bits.extend(output.into_iter().map(f32::to_bits));
        }
    }
    bits
}

#[test]
fn control_queries_do_not_borrow_worker_state() {
    let fixture = TestStream::new();
    let worker = unsafe { tb_stream(fixture.handle) }.unwrap();
    let reference = render_seeks(worker);
    fn query_with_worker(
        worker: &mut triple_balance_stream::Stream,
        handle: *const RenderTripleBalanceStream,
    ) -> Vec<u32> {
        // This function's &mut parameter stays protected across the control query.
        // Miri rejects a query that borrows the enclosing handle or worker state.
        check_control(handle);
        render_seeks(worker)
    }
    assert_eq!(query_with_worker(worker, fixture.handle), reference);
}

#[test]
fn concurrent_control_queries_preserve_seek_pcm() {
    let fixture = TestStream::new();
    let worker = unsafe { tb_stream(fixture.handle) }.unwrap();
    let reference = render_seeks(worker);
    let handle = AtomicPtr::new(fixture.handle);
    let start = Barrier::new(2);
    std::thread::scope(|threads| {
        threads.spawn(|| {
            start.wait();
            for _ in 0..64 {
                check_control(handle.load(Ordering::Relaxed));
                std::thread::yield_now();
            }
        });
        start.wait();
        for _ in 0..8 {
            assert_eq!(render_seeks(worker), reference);
        }
    });
}
